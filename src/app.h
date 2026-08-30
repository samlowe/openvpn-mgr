#ifndef OPENVPN_MANAGER_APP_H
#define OPENVPN_MANAGER_APP_H

#include "core.h"
#include "line_reader.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#define APP_MAX_VISIBLE_RESULTS 100U
#define APP_STOP_TIMEOUT_SECONDS 10U

/**
 * AppState:
 * @APP_IDLE: no active OpenVPN session
 * @APP_STARTING: launch in progress or awaiting connection
 * @APP_CONNECTED: tunnel established
 * @APP_STOPPING: disconnect requested
 * @APP_ERROR: last operation failed
 *
 * High-level connection lifecycle state for the UI.
 */
typedef enum {
    APP_IDLE,
    APP_STARTING,
    APP_CONNECTED,
    APP_STOPPING,
    APP_ERROR
} AppState;

/**
 * App:
 *
 * GTK application state: profile list, active session, and management socket.
 */
typedef struct App {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *search_entry;
    GtkWidget *list_box;
    GtkWidget *status_label;
    GtkWidget *connect_button;
    GtkWidget *disconnect_button;
    GtkWidget *progress;

    AppConfig config;
    GPtrArray *profiles;
    Profile *selected_profile;
    AppState state;
    guint filter_generation;
    guint filter_timeout_id;
    gboolean quitting;
    gboolean launching;
    gboolean stop_requested;
    guint session_generation;
    gchar *last_connected_path;
    Profile *active_profile;
    GCancellable *scan_cancellable;
    GCancellable *filter_cancellable;

    GSubprocess *process;
    GSocketService *management_service;
    GSocketConnection *management_connection;
    LineReader *management_reader;
    gchar *management_directory;
    gchar *management_socket;
    guint stop_timeout_id;
} App;

void app_set_status(App *app, const gchar *text);
void app_set_state(App *app, AppState state);
void app_update_buttons(App *app);
void app_save_last_connected_path(App *app);
gchar *app_load_last_connected_path(void);
gchar *app_config_path(void);
gchar *app_state_path(void);
void app_cleanup(App *app);

void app_ui_activate(GtkApplication *application, App *app);

void app_session_connect(App *app, Profile *profile);
void app_session_disconnect(App *app);
void app_session_process_line(App *app, const gchar *line, gboolean management,
                             guint generation);

gboolean app_management_create_socket(App *app, GError **error);
void app_management_remove_socket(App *app);
void app_management_connection_close(App *app);

#endif
