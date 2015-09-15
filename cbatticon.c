/*
 * Copyright (C) 2011-2013 Colin Jones
 * Copyright (C) 2014-2015 Valère Monseur
 *
 * Based on code by Matteo Marchesotti
 * Copyright (C) 2007 Matteo Marchesotti <matteo.marchesotti@fsfe.org>
 *
 * cbatticon: a lightweight and fast battery icon that sits in your system tray.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define CBATTICON_VERSION_NUMBER 1.5.0
#define CBATTICON_VERSION_STRING "1.5.0"
#define CBATTICON_TEXT_DOMAIN "cbatticon"

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <gtk/gtk.h>
#ifdef WITH_NOTIFY
#include <libnotify/notify.h>
#endif

#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <math.h>
#include <syslog.h>

static gint get_options (int argc, char **argv);
static gboolean get_power_supply (gchar *battery_suffix, gboolean list_power_supply);

static gboolean get_sysattr_string (gchar *path, gchar *attribute, gchar **value);
static gboolean get_sysattr_double (gchar *path, gchar *attribute, gdouble *value);

static gboolean get_battery_present (gboolean *present);
static gboolean get_battery_status (gint *status);
static gboolean get_ac_online (gboolean *online);

static gboolean get_battery_full_capacity (gboolean *use_charge, gdouble *capacity);
static gboolean get_battery_remaining_capacity (gboolean use_charge, gdouble *capacity);
static gboolean get_battery_current_rate (gboolean use_charge, gdouble *rate);

static gboolean get_battery_charge (gboolean remaining, gint *percentage, gint *time);
static gboolean get_battery_time_estimation (gdouble remaining_capacity, gdouble y, gint *time);
static void reset_battery_time_estimation (void);

static void create_tray_icon (void);
static gboolean update_tray_icon (GtkStatusIcon *tray_icon);
static void update_tray_icon_status (GtkStatusIcon *tray_icon);
static void tray_icon_on_click (GtkStatusIcon *status_icon, gpointer user_data);

#ifdef WITH_NOTIFY
static void notify_message (NotifyNotification **notification, gchar *summary, gchar *body, gint timeout, NotifyUrgency urgency);
#define NOTIFY_MESSAGE(...) notify_message(__VA_ARGS__)
#else
#define NOTIFY_MESSAGE(...)
#endif

static gchar* get_tooltip_string (gchar *battery, gchar *time);
static gchar* get_battery_string (gint state, gint percentage);
static gchar* get_time_string (gint minutes);
static gchar* get_icon_name (gint state, gint percentage);

#define SYSFS_PATH "/sys/class/power_supply"

#define DEFAULT_UPDATE_INTERVAL 5
#define DEFAULT_WEIGHT          0.1
#define DEFAULT_LOW_LEVEL       20
#define DEFAULT_CRITICAL_LEVEL  5

#define STR_LTH 256

enum {
    UNKNOWN_ICON = 0,
    BATTERY_ICON,
    BATTERY_ICON_SYMBOLIC,
    BATTERY_ICON_NOTIFICATION
};

enum {
    MISSING = 0,
    UNKNOWN,
    CHARGED,
    CHARGING,
    DISCHARGING,
    NOT_CHARGING,
    LOW_LEVEL,
    CRITICAL_LEVEL
};

struct configuration {
    gint     update_interval;
    gdouble  weight;
    gint     icon_type;
    gint     low_level;
    gint     critical_level;
    gchar   *command_critical_level;
    gchar   *command_left_click;
#ifdef WITH_NOTIFY
    gboolean hide_notification;
#endif
    gboolean debug_output;
} configuration = {
    DEFAULT_UPDATE_INTERVAL,
    UNKNOWN_ICON,
    DEFAULT_LOW_LEVEL,
    DEFAULT_CRITICAL_LEVEL,
    NULL,
    NULL,
#ifdef WITH_NOTIFY
    FALSE,
#endif
    FALSE
};

static gchar *battery_path = NULL;
static gchar *ac_path      = NULL;

/*
 * store previous time value for weighted average
 */
static gint previous_time = -1;

/*
 * workaround for limited/bugged batteries/drivers that don't provide current rate
 * the next 4 variables are used to calculate estimated time
 */

static gboolean estimation_needed             = FALSE;
static gdouble  estimation_remaining_capacity = -1;
static gint     estimation_time               = -1;
static GTimer  *estimation_timer              = NULL;

/*
 * command line options function
 */

static gint get_options (int argc, char **argv)
{
    GOptionContext *option_context;
    GError *error = NULL;

    gchar *icon_type_string       = NULL;
    gboolean display_version      = FALSE;
    gboolean list_icon_type       = FALSE;
    gboolean list_power_supply    = FALSE;
    GOptionEntry option_entries[] = {
        { "version"               , 'v', 0, G_OPTION_ARG_NONE  , &display_version                     , N_("Display the version")                                      , NULL },
        { "debug"                 , 'd', 0, G_OPTION_ARG_NONE  , &configuration.debug_output          , N_("Display debug information")                                , NULL },
        { "update-interval"       , 'u', 0, G_OPTION_ARG_INT   , &configuration.update_interval       , N_("Set update interval (in seconds)")                         , NULL },
        { "weight"                , 'w', 0, G_OPTION_ARG_DOUBLE   , &configuration.weight                , N_("Set weight coefficient(0 - 1) for exponential average") , NULL },
        { "icon-type"             , 'i', 0, G_OPTION_ARG_STRING, &icon_type_string                    , N_("Set icon type ('standard', 'notification' or 'symbolic')") , NULL },
        { "low-level"             , 'l', 0, G_OPTION_ARG_INT   , &configuration.low_level             , N_("Set low battery level (in percent)")                       , NULL },
        { "critical-level"        , 'r', 0, G_OPTION_ARG_INT   , &configuration.critical_level        , N_("Set critical battery level (in percent)")                  , NULL },
        { "command-critical-level", 'c', 0, G_OPTION_ARG_STRING, &configuration.command_critical_level, N_("Command to execute when critical battery level is reached"), NULL },
        { "command-left-click"    , 'x', 0, G_OPTION_ARG_STRING, &configuration.command_left_click    , N_("Command to execute when left clicking on tray icon")       , NULL },
#ifdef WITH_NOTIFY
        { "hide-notification"     , 'n', 0, G_OPTION_ARG_NONE  , &configuration.hide_notification     , N_("Hide the notification popups")                             , NULL },
#endif
        { "list-icon-types"       , 't', 0, G_OPTION_ARG_NONE  , &list_icon_type                      , N_("List available icon types")                                , NULL },
        { "list-power-supplies"   , 'p', 0, G_OPTION_ARG_NONE  , &list_power_supply                   , N_("List available power supplies (battery and AC)")           , NULL },
        { NULL }
    };

    option_context = g_option_context_new (_("[BATTERY ID]"));
    g_option_context_add_main_entries (option_context, option_entries, CBATTICON_TEXT_DOMAIN);

    if (g_option_context_parse (option_context, &argc, &argv, &error) == FALSE) {
        g_printerr (_("Cannot parse command line arguments: %s\n"), error->message);
        g_error_free (error); error = NULL;

        return -1;
    }

    g_option_context_free (option_context);

    /* option : display the version */

    if (display_version == TRUE) {
        g_print (_("cbatticon: a lightweight and fast battery icon that sits in your system tray\n"));
        g_print (_("version %s\n"), CBATTICON_VERSION_STRING);

        return 0;
    }

    /* option : list available power supplies (battery and AC) */

    if (list_power_supply == TRUE) {
        g_print (_("List of available power supplies:\n"));
        get_power_supply (NULL, TRUE);

        return 0;
    }

    /* option : list available icon types */

    gtk_init (&argc, &argv); /* gtk is required as from this point */

    #define HAS_STANDARD_ICON_TYPE     gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), "battery-full")
    #define HAS_NOTIFICATION_ICON_TYPE gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), "notification-battery-100")
    #define HAS_SYMBOLIC_ICON_TYPE     gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), "battery-full-symbolic")

    if (list_icon_type == TRUE) {
        g_print (_("List of available icon types:\n"));
        g_print ("standard\t%s\n"    , HAS_STANDARD_ICON_TYPE     == TRUE ? _("available") : _("unavailable"));
        g_print ("notification\t%s\n", HAS_NOTIFICATION_ICON_TYPE == TRUE ? _("available") : _("unavailable"));
        g_print ("symbolic\t%s\n"    , HAS_SYMBOLIC_ICON_TYPE     == TRUE ? _("available") : _("unavailable"));

        return 0;
    }

    /* option : set icon type */

    if (icon_type_string != NULL) {
        if (g_strcmp0 (icon_type_string, "standard") == 0 && HAS_STANDARD_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON;
        else if (g_strcmp0 (icon_type_string, "notification") == 0 && HAS_NOTIFICATION_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_NOTIFICATION;
        else if (g_strcmp0 (icon_type_string, "symbolic") == 0 && HAS_SYMBOLIC_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_SYMBOLIC;
        else g_printerr (_("Unknown icon type: %s\n"), icon_type_string);

        g_free (icon_type_string);
    }

    if (configuration.icon_type == UNKNOWN_ICON) {
        if (HAS_STANDARD_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON;
        else if (HAS_NOTIFICATION_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_NOTIFICATION;
        else if (HAS_SYMBOLIC_ICON_TYPE == TRUE)
            configuration.icon_type = BATTERY_ICON_SYMBOLIC;
        else g_printerr (_("No icon type found!\n"));
    }

    /* option : update interval */

    if (configuration.update_interval <= 0) {
        configuration.update_interval = DEFAULT_UPDATE_INTERVAL;
        g_printerr (_("Invalid update interval! It has been reset to default (%d seconds)\n"), DEFAULT_UPDATE_INTERVAL);
    }

    /* option : low and critical levels */

    if (configuration.low_level < 0 || configuration.low_level > 100) {
        configuration.low_level = DEFAULT_LOW_LEVEL;
        g_printerr (_("Invalid low level! It has been reset to default (%d percent)\n"), DEFAULT_LOW_LEVEL);
    }

    if (configuration.critical_level < 0 || configuration.critical_level > 100) {
        configuration.critical_level = DEFAULT_CRITICAL_LEVEL;
        g_printerr (_("Invalid critical level! It has been reset to default (%d percent)\n"), DEFAULT_CRITICAL_LEVEL);
    }

    if (configuration.critical_level > configuration.low_level) {
        configuration.critical_level = DEFAULT_CRITICAL_LEVEL;
        configuration.low_level = DEFAULT_LOW_LEVEL;
        g_printerr (_("Critical level is higher than low level! They have been reset to default\n"));
    }

    return 1;
}

/*
 * sysfs functions
 */

static gboolean get_power_supply (gchar *battery_suffix, gboolean list_power_supply)
{
    GDir *directory;
    GError *error = NULL;
    const gchar *file;
    gchar *path;
    gchar *sysattr_value;
    gboolean sysattr_status;

    directory = g_dir_open (SYSFS_PATH, 0, &error);
    if (directory != NULL) {
        file = g_dir_read_name (directory);
        while (file != NULL) {
            path = g_build_filename (SYSFS_PATH, file, NULL);

            sysattr_status = get_sysattr_string (path, "type", &sysattr_value);
            if (sysattr_status == TRUE) {
                /* process battery */
                if (g_str_has_prefix (sysattr_value, "Battery") == TRUE) {
                    if (list_power_supply == TRUE) {
                        gchar *power_supply_id = g_path_get_basename (path);
                        g_print (_("type: %-*.*s\tid: %-*.*s\tpath: %s\n"), 12, 12, _("Battery"), 12, 12, power_supply_id, path);
                        g_free (power_supply_id);
                    }

                    if (battery_path == NULL) {
                        if (battery_suffix == NULL ||
                            g_str_has_suffix (path, battery_suffix) == TRUE) {
                            battery_path = g_strdup (path);

                            /* workaround for limited/bugged batteries/drivers */
                            /* that don't provide current rate                 */
                            if (get_battery_current_rate (FALSE, NULL) == FALSE &&
                                get_battery_current_rate (TRUE, NULL) == FALSE) {
                                estimation_needed = TRUE;
                                estimation_timer = g_timer_new ();

                                if (configuration.debug_output == TRUE) {
                                    g_printf (_("workaround: current rate is not available, estimating rate\n"));
                                }
                            }

                            if (configuration.debug_output == TRUE) {
                                g_printf (_("battery path: %s\n"), battery_path);
                            }
                        }
                    }
                }

                /* process AC */
                if (g_str_has_prefix (sysattr_value, "Mains") == TRUE) {
                    if (list_power_supply == TRUE) {
                        gchar *power_supply_id = g_path_get_basename (path);
                        g_print (_("type: %-*.*s\tid: %-*.*s\tpath: %s\n"), 12, 12, _("AC"), 12, 12, power_supply_id, path);
                        g_free (power_supply_id);
                    }

                    if (ac_path == NULL) {
                        ac_path = g_strdup (path);

                        if (configuration.debug_output == TRUE) {
                            g_printf (_("ac path: %s\n"), ac_path);
                        }
                    }
                }

                g_free (sysattr_value);
            }

            g_free (path);
            file = g_dir_read_name (directory);
        }

        g_dir_close (directory);
    } else {
        g_printerr (_("Cannot open sysfs directory: %s (%s)\n"), SYSFS_PATH, error->message);
        g_error_free (error); error = NULL;
        return FALSE;
    }

    if (list_power_supply == FALSE && battery_path == NULL) {
        if (battery_suffix != NULL) {
            g_printerr (_("No battery with suffix %s found!\n"), battery_suffix);
            return FALSE;
        }

        if (ac_path == NULL) {
            g_printerr (_("No battery nor AC power supply found!\n"));
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean get_sysattr_string (gchar *path, gchar *attribute, gchar **value)
{
    gchar *sysattr_filename;
    gboolean sysattr_status;

    g_return_val_if_fail (path != NULL, FALSE);
    g_return_val_if_fail (attribute != NULL, FALSE);
    g_return_val_if_fail (value != NULL, FALSE);

    sysattr_filename = g_build_filename (path, attribute, NULL);
    sysattr_status = g_file_get_contents (sysattr_filename, value, NULL, NULL);
    g_free (sysattr_filename);

    return sysattr_status;
}

static gboolean get_sysattr_double (gchar *path, gchar *attribute, gdouble *value)
{
    gchar *sysattr_filename, *sysattr_value;
    gboolean sysattr_status;

    g_return_val_if_fail (path != NULL, FALSE);
    g_return_val_if_fail (attribute != NULL, FALSE);

    sysattr_filename = g_build_filename (path, attribute, NULL);
    sysattr_status = g_file_get_contents (sysattr_filename, &sysattr_value, NULL, NULL);
    g_free (sysattr_filename);

    if (sysattr_status == TRUE) {
        gdouble double_value = g_ascii_strtod (sysattr_value, NULL);

        if (errno != 0 || double_value < 0.01) {
            sysattr_status = FALSE;
        }

        if (value != NULL) {
            *value = double_value;
        }

        g_free (sysattr_value);
    }

    return sysattr_status;
}

static gboolean get_battery_present (gboolean *present)
{
    gchar *sysattr_value;
    gboolean sysattr_status;

    g_return_val_if_fail (present != NULL, FALSE);

    sysattr_status = get_sysattr_string (battery_path, "present", &sysattr_value);
    if (sysattr_status == TRUE) {
        *present = g_str_has_prefix (sysattr_value, "1") ? TRUE : FALSE;

        if (configuration.debug_output == TRUE) {
            g_printf (_("battery present: %s"), sysattr_value);
        }

        g_free (sysattr_value);
    }

    return sysattr_status;
}

static gboolean get_battery_status (gint *status)
{
    gchar *sysattr_value;
    gboolean sysattr_status;

    g_return_val_if_fail (status != NULL, FALSE);

    sysattr_status = get_sysattr_string (battery_path, "status", &sysattr_value);
    if (sysattr_status == TRUE) {
        if (g_str_has_prefix (sysattr_value, "Charging") == TRUE)
            *status = CHARGING;
        else if (g_str_has_prefix (sysattr_value, "Discharging") == TRUE)
            *status = DISCHARGING;
        else if (g_str_has_prefix (sysattr_value, "Not charging") == TRUE)
            *status = NOT_CHARGING;
        else if (g_str_has_prefix (sysattr_value, "Full") == TRUE)
            *status = CHARGED;
        else
            *status = UNKNOWN;

        if (configuration.debug_output == TRUE) {
            g_printf (_("battery status: %d - %s"), *status, sysattr_value);
        }

        g_free (sysattr_value);
    }

    return sysattr_status;
}

static gboolean get_ac_online (gboolean *online)
{
    gchar *sysattr_value;
    gboolean sysattr_status;

    g_return_val_if_fail (online != NULL, FALSE);

    if (ac_path == NULL) {
        return FALSE;
    }

    sysattr_status = get_sysattr_string (ac_path, "online", &sysattr_value);
    if (sysattr_status == TRUE) {
        *online = g_str_has_prefix (sysattr_value, "1") ? TRUE : FALSE;

        if (configuration.debug_output == TRUE) {
            g_printf (_("ac online: %s"), sysattr_value);
        }

        g_free (sysattr_value);
    }

    return sysattr_status;
}

static gboolean get_battery_full_capacity (gboolean *use_charge, gdouble *capacity)
{
    gboolean sysattr_status;

    g_return_val_if_fail (use_charge != NULL, FALSE);
    g_return_val_if_fail (capacity != NULL, FALSE);

    sysattr_status = get_sysattr_double (battery_path, "energy_full", capacity);
    *use_charge = FALSE;

    if (sysattr_status == FALSE) {
        sysattr_status = get_sysattr_double (battery_path, "charge_full", capacity);
        *use_charge = TRUE;
    }

    return sysattr_status;
}

static gboolean get_battery_remaining_capacity (gboolean use_charge, gdouble *capacity)
{
    g_return_val_if_fail (capacity != NULL, FALSE);

    if (use_charge == FALSE) {
        return get_sysattr_double (battery_path, "energy_now", capacity);
    } else {
        return get_sysattr_double (battery_path, "charge_now", capacity);
    }
}

static gboolean get_battery_current_rate (gboolean use_charge, gdouble *rate)
{
    if (use_charge == FALSE) {
        return get_sysattr_double (battery_path, "power_now", rate);
    } else {
        return get_sysattr_double (battery_path, "current_now", rate);
    }
}

/*
 * computation functions
 */

static gboolean get_battery_charge (gboolean remaining, gint *percentage, gint *time)
{
    gdouble full_capacity, remaining_capacity, current_rate;
    gboolean use_charge;

    g_return_val_if_fail (percentage != NULL, FALSE);

    if (get_battery_full_capacity (&use_charge, &full_capacity) == FALSE) {
        if (configuration.debug_output == TRUE) {
            g_printf (_("full capacity: %s\n"), _("unavailable"));
        }

        return FALSE;
    }

    if (get_battery_remaining_capacity (use_charge, &remaining_capacity) == FALSE) {
        if (configuration.debug_output == TRUE) {
            g_printf (_("remaining capacity: %s\n"), _("unavailable"));
        }

        return FALSE;
    }

    *percentage = (gint)fmin (floor (remaining_capacity / full_capacity * 100.0), 100.0);

    if (time == NULL) {
        return TRUE;
    }

    if (estimation_needed == TRUE) {
        if (remaining == TRUE) {
            return get_battery_time_estimation (remaining_capacity, 0, time);
        } else {
            return get_battery_time_estimation (remaining_capacity, full_capacity, time);
        }
    }

    if (get_battery_current_rate (use_charge, &current_rate) == FALSE) {
        if (configuration.debug_output == TRUE) {
            g_printf (_("current rate: %s\n"), _("unavailable"));
        }

        return FALSE;
    }

    if (remaining == TRUE) {
        *time = (gint)(remaining_capacity / current_rate * 60.0);
        //apply weighted average
        if (previous_time != -1) {
            *time = (gint) (*time * configuration.weight + (1 - configuration.weight) * previous_time);
        }

    } else {
        *time = (gint)((full_capacity - remaining_capacity) / current_rate * 60.0);
        //apply weighted average
        if (previous_time != -1) {
            *time = (gint) (*time * configuration.weight + (1 - configuration.weight) * previous_time);
        }
    }

    previous_time = *time;

    return TRUE;
}

static gboolean get_battery_time_estimation (gdouble remaining_capacity, gdouble y, gint *time)
{
    if (estimation_remaining_capacity == -1) {
        estimation_remaining_capacity = remaining_capacity;
    }

    /*
     * y = mx + b ... x = (y - b) / m
     * solving for when y = 0 (discharging) or full_capacity (charging)
     */

    if (remaining_capacity != estimation_remaining_capacity) {
        gdouble estimation_elapsed = g_timer_elapsed (estimation_timer, NULL);
        gdouble estimation_current_rate = (remaining_capacity - estimation_remaining_capacity) / estimation_elapsed;
        gdouble estimation_seconds = (y - remaining_capacity) / estimation_current_rate;

        *time = (gint)((estimation_seconds) / 60.0);

        estimation_remaining_capacity = remaining_capacity;
        estimation_time               = *time;
        g_timer_start (estimation_timer);
    } else {
        *time = estimation_time;
    }

    return TRUE;
}

static void reset_battery_time_estimation (void)
{
    estimation_remaining_capacity = -1;
    estimation_time               = -1;
    g_timer_start (estimation_timer);
}

/*
 * tray icon functions
 */

static void create_tray_icon (void)
{
    GtkStatusIcon *tray_icon = gtk_status_icon_new ();

    gtk_status_icon_set_tooltip_text (tray_icon, "cbatticon");
    gtk_status_icon_set_visible (tray_icon, TRUE);

    update_tray_icon (tray_icon);
    g_timeout_add_seconds (configuration.update_interval, (GSourceFunc)update_tray_icon, (gpointer)tray_icon);
    g_signal_connect (G_OBJECT (tray_icon), "activate", G_CALLBACK (tray_icon_on_click), NULL);
}

static gboolean update_tray_icon (GtkStatusIcon *tray_icon)
{
    g_return_val_if_fail (tray_icon != NULL, FALSE);

    update_tray_icon_status (tray_icon);

    return TRUE;
}

static void update_tray_icon_status (GtkStatusIcon *tray_icon)
{
    gboolean battery_present = FALSE;
    gboolean ac_online       = FALSE;

    gint battery_status            = -1;
    static gint old_battery_status = -1;

    static gboolean battery_low            = FALSE;
    static gboolean battery_critical       = FALSE;
    static gboolean spawn_command_critical = FALSE;

    gint percentage, time;
    gchar *battery_string, *time_string;
    GError *error = NULL;
#ifdef WITH_NOTIFY
    NotifyNotification *notification = NULL;
#endif

    g_return_if_fail (tray_icon != NULL);

    /* update tray icon for AC only */
    if (battery_path == NULL) {
        static gboolean ac_notified = FALSE;

        if (ac_notified == FALSE) {
            ac_notified = TRUE;
            NOTIFY_MESSAGE (&notification, _("AC only, no battery!"), NULL, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_NORMAL);
        }

        gtk_status_icon_set_tooltip_text (tray_icon, _("AC only, no battery!"));
        gtk_status_icon_set_from_icon_name (tray_icon, "ac-adapter");

        return;
    }

    /* update tray icon for battery */
    if (get_battery_present (&battery_present) == FALSE) {
        return;
    }

    if (battery_present == FALSE) {
        battery_status = MISSING;
    } else {
        if (get_battery_status (&battery_status) == FALSE) {
            return;
        }

        /* workaround for limited/bugged batteries/drivers */
        /* that unduly return unknown status               */
        if (battery_status == UNKNOWN && get_ac_online (&ac_online) == TRUE) {
            if (ac_online == TRUE) {
                battery_status = CHARGING;

                if (get_battery_charge (FALSE, &percentage, NULL) == TRUE && percentage >= 99) {
                    battery_status = CHARGED;
                }
            } else {
                battery_status = DISCHARGING;
            }
        }
    }

    #define HANDLE_BATTERY_STATUS(PCT,TIM,EXP,URG)                                                          \
                                                                                                            \
            percentage = PCT;                                                                               \
                                                                                                            \
            battery_string = get_battery_string (battery_status, percentage);                               \
            time_string    = get_time_string (TIM);                                                         \
                                                                                                            \
            if (old_battery_status != battery_status) {                                                     \
                old_battery_status  = battery_status;                                                       \
                NOTIFY_MESSAGE (&notification, battery_string, time_string, EXP, URG);                      \
            }                                                                                               \
                                                                                                            \
            gtk_status_icon_set_tooltip_text (tray_icon, get_tooltip_string (battery_string, time_string)); \
            gtk_status_icon_set_from_icon_name (tray_icon, get_icon_name (battery_status, percentage));

    switch (battery_status) {
        case MISSING:
            HANDLE_BATTERY_STATUS (0, -1, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_NORMAL)
            break;

        case UNKNOWN:
            HANDLE_BATTERY_STATUS (0, -1, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL)
            break;

        case CHARGED:
            HANDLE_BATTERY_STATUS (100, -1, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL)
            break;

        case CHARGING:
            if (old_battery_status != CHARGING && estimation_needed == TRUE) {
                reset_battery_time_estimation ();
            }
            if (old_battery_status != CHARGING) {
                previous_time = -1;
            }

            if (get_battery_charge (FALSE, &percentage, &time) == FALSE) {
                return;
            }

            HANDLE_BATTERY_STATUS (percentage, time, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL)
            break;

        case DISCHARGING:
        case NOT_CHARGING:
            if (old_battery_status != DISCHARGING && estimation_needed == TRUE) {
                reset_battery_time_estimation ();
            }

            if (old_battery_status != DISCHARGING) {
                previous_time = -1;
            }

            if (get_battery_charge (TRUE, &percentage, &time) == FALSE) {
                return;
            }

            battery_string = get_battery_string (battery_status, percentage);
            time_string    = get_time_string (time);

            if (old_battery_status != DISCHARGING) {
                old_battery_status  = DISCHARGING;
                NOTIFY_MESSAGE (&notification, battery_string, time_string, NOTIFY_EXPIRES_DEFAULT, NOTIFY_URGENCY_NORMAL);

                battery_low            = FALSE;
                battery_critical       = FALSE;
                spawn_command_critical = FALSE;
            }

            if (battery_low == FALSE && percentage <= configuration.low_level) {
                battery_low = TRUE;

                battery_string = get_battery_string (LOW_LEVEL, percentage);
                NOTIFY_MESSAGE (&notification, battery_string, time_string, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_NORMAL);
            }

            if (battery_critical == FALSE && percentage <= configuration.critical_level) {
                battery_critical = TRUE;

                battery_string = get_battery_string (CRITICAL_LEVEL, percentage);
                NOTIFY_MESSAGE (&notification, battery_string, time_string, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_CRITICAL);

                spawn_command_critical = TRUE;
            }

            gtk_status_icon_set_tooltip_text (tray_icon, get_tooltip_string (battery_string, time_string));
            gtk_status_icon_set_from_icon_name (tray_icon, get_icon_name (battery_status, percentage));

            if (spawn_command_critical == TRUE) {
                spawn_command_critical = FALSE;

                if (configuration.command_critical_level != NULL) {
                    syslog (LOG_CRIT, _("Spawning critical battery level command in 30 seconds: %s"), configuration.command_critical_level);
                    g_usleep (G_USEC_PER_SEC * 30);

                    if (g_spawn_command_line_async (configuration.command_critical_level, &error) == FALSE) {
                        syslog (LOG_CRIT, _("Cannot spawn critical battery level command: %s"), error->message);
                        g_error_free (error); error = NULL;

                        NOTIFY_MESSAGE (&notification, _("Cannot spawn critical battery level command!"), configuration.command_critical_level, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_CRITICAL);
                    }
                }
            }
            break;
    }
}

static void tray_icon_on_click (GtkStatusIcon *status_icon, gpointer user_data)
{
    GError *error = NULL;
#ifdef WITH_NOTIFY
    NotifyNotification *notification = NULL;
#endif

    if (configuration.command_left_click != NULL) {
        if (g_spawn_command_line_async (configuration.command_left_click, &error) == FALSE) {
            syslog (LOG_ERR, _("Cannot spawn left click command: %s"), error->message);
            g_error_free (error); error = NULL;

            NOTIFY_MESSAGE (&notification, _("Cannot spawn left click command!"), configuration.command_left_click, NOTIFY_EXPIRES_NEVER, NOTIFY_URGENCY_CRITICAL);
        }
    }
}

#ifdef WITH_NOTIFY
static void notify_message (NotifyNotification **notification, gchar *summary, gchar *body, gint timeout, NotifyUrgency urgency)
{
    g_return_if_fail (notification != NULL);
    g_return_if_fail (summary != NULL);

    if (configuration.hide_notification == TRUE) {
        return;
    }

    if (*notification == NULL) {
#if NOTIFY_CHECK_VERSION (0, 7, 0)
        *notification = notify_notification_new (summary, body, NULL);
#else
        *notification = notify_notification_new (summary, body, NULL, NULL);
#endif
    } else {
        notify_notification_update (*notification, summary, body, NULL);
    }

    notify_notification_set_timeout (*notification, timeout);
    notify_notification_set_urgency (*notification, urgency);

    notify_notification_show (*notification, NULL);
}
#endif

static gchar* get_tooltip_string (gchar *battery, gchar *time)
{
    static gchar tooltip_string[STR_LTH];

    tooltip_string[0] = '\0';

    g_return_val_if_fail (battery != NULL, tooltip_string);

    g_strlcpy (tooltip_string, battery, STR_LTH);

    if (configuration.debug_output == TRUE) {
        g_printf (_("tooltip: %s\n"), battery);
    }

    if (time != NULL) {
        g_strlcat (tooltip_string, "\n", STR_LTH);
        g_strlcat (tooltip_string, time, STR_LTH);

        if (configuration.debug_output == TRUE) {
            g_printf (_("tooltip: %s\n"), time);
        }
    }

    return tooltip_string;
}

static gchar* get_battery_string (gint state, gint percentage)
{
    static gchar battery_string[STR_LTH];

    switch (state) {
        case MISSING:
            g_strlcpy (battery_string, _("Battery is missing!"), STR_LTH);
            break;

        case UNKNOWN:
            g_strlcpy (battery_string, _("Battery status is unknown!"), STR_LTH);
            break;

        case CHARGED:
            g_strlcpy (battery_string, _("Battery is charged!"), STR_LTH);
            break;

        case DISCHARGING:
            g_snprintf (battery_string, STR_LTH, _("Battery is discharging (%i% remaining)"), percentage);
            break;

        case NOT_CHARGING:
            g_snprintf (battery_string, STR_LTH, _("Battery is not charging (%i% remaining)"), percentage);
            break;

        case LOW_LEVEL:
            g_snprintf (battery_string, STR_LTH, _("Battery level is low! (%i% remaining)"), percentage);
            break;

        case CRITICAL_LEVEL:
            g_snprintf (battery_string, STR_LTH, _("Battery level is critical! (%i% remaining)"), percentage);
            break;

        case CHARGING:
            g_snprintf (battery_string, STR_LTH, _("Battery is charging (%i%)"), percentage);
            break;

        default:
            battery_string[0] = '\0';
            break;
    }

    if (configuration.debug_output == TRUE) {
        g_printf (_("battery string: %s\n"), battery_string);
    }

    return battery_string;
}

static gchar* get_time_string (gint minutes)
{
    static gchar time_string[STR_LTH];
    gint hours;

    if (minutes < 0) {
        return NULL;
    }

    hours   = minutes / 60;
    minutes = minutes % 60;

    if (hours > 0) {
        g_sprintf (time_string, _("%2d hours, %2d minutes remaining"), hours, minutes);
    } else {
        g_sprintf (time_string, _("%2d minutes remaining"), minutes);
    }

    if (configuration.debug_output == TRUE) {
        g_printf (_("time string: %s\n"), time_string);
    }

    return time_string;
}

static gchar* get_icon_name (gint state, gint percentage)
{
    static gchar icon_name[STR_LTH];

    if (configuration.icon_type == BATTERY_ICON_NOTIFICATION) {
        g_strlcpy (icon_name, "notification-battery", STR_LTH);
    } else {
        g_strlcpy (icon_name, "battery", STR_LTH);
    }

    if (state == MISSING || state == UNKNOWN) {
        if (configuration.icon_type == BATTERY_ICON_NOTIFICATION) {
            g_strlcat (icon_name, "-empty", STR_LTH);
        } else {
            g_strlcat (icon_name, "-missing", STR_LTH);
        }
    } else {
        if (configuration.icon_type == BATTERY_ICON_NOTIFICATION) {
                 if (percentage <= 20)  g_strlcat (icon_name, "-020", STR_LTH);
            else if (percentage <= 40)  g_strlcat (icon_name, "-040", STR_LTH);
            else if (percentage <= 60)  g_strlcat (icon_name, "-060", STR_LTH);
            else if (percentage <= 80)  g_strlcat (icon_name, "-080", STR_LTH);
            else                        g_strlcat (icon_name, "-100", STR_LTH);

                 if (state == CHARGING) g_strlcat (icon_name, "-plugged", STR_LTH);
            else if (state == CHARGED)  g_strlcat (icon_name, "-plugged", STR_LTH);
        } else {
                 if (percentage <= 20)  g_strlcat (icon_name, "-caution", STR_LTH);
            else if (percentage <= 40)  g_strlcat (icon_name, "-low", STR_LTH);
            else if (percentage <= 80)  g_strlcat (icon_name, "-good", STR_LTH);
            else                        g_strlcat (icon_name, "-full", STR_LTH);

                 if (state == CHARGING) g_strlcat (icon_name, "-charging", STR_LTH);
            else if (state == CHARGED)  g_strlcat (icon_name, "-charged", STR_LTH);
        }
    }

    if (configuration.icon_type == BATTERY_ICON_SYMBOLIC) {
        g_strlcat (icon_name, "-symbolic", STR_LTH);
    }

    if (configuration.debug_output == TRUE) {
        g_printf (_("icon name: %s\n"), icon_name);
    }

    return icon_name;
}

int main (int argc, char **argv)
{
    gint ret;

    setlocale (LC_ALL, "");
    bindtextdomain (CBATTICON_TEXT_DOMAIN, NLSDIR);
    bind_textdomain_codeset (CBATTICON_TEXT_DOMAIN, "UTF-8");
    textdomain (CBATTICON_TEXT_DOMAIN);

    ret = get_options (argc, argv);
    if (ret <= 0)
        return ret;

#ifdef WITH_NOTIFY
    if (configuration.hide_notification == FALSE) {
        if (notify_init ("cbatticon") == FALSE) {
            return -1;
        }
    }
#endif

    if (get_power_supply (argc > 1 ? argv[1] : NULL, FALSE) == FALSE) {
        return -1;
    }

    create_tray_icon ();
    gtk_main();

    return 0;
}
