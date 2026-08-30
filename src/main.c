#include "core.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gio/gunixsocketaddress.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_VISIBLE_RESULTS 100U
#define READ_BUFFER_SIZE 4096U
#define MAX_OUTPUT_LINE 8192U
#define STOP_TIMEOUT_SECONDS 10U

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

typedef struct _App App;

/**
 * LineReader:
 * @stream: input stream being read incrementally
 * @pending: bytes accumulated since the last complete line
 * @app: owning application instance
 * @management: %TRUE when reading the management interface
 * @generation: session generation used to ignore stale callbacks
 * @buffer: stack buffer passed to g_input_stream_read_async()
 *
 * Reads newline-delimited text asynchronously from an OpenVPN stream.
 */
typedef struct {
    GInputStream *stream;
    GByteArray *pending;
    App *app;
    gboolean management;
    guint generation;
    guint8 buffer[READ_BUFFER_SIZE];
} LineReader;

/**
 * ScanInput:
 * @config_path: path to the manager config file
 *
 * Input payload for the profile scan background task.
 */
typedef struct {
    gchar *config_path;
} ScanInput;

/**
 * ScanResult:
 * @config: loaded application configuration
 * @profiles: scanned and mapped profiles
 * @last_connected_path: persisted last-connected profile path
 *
 * Output payload from the profile scan background task.
 */
typedef struct {
    AppConfig *config;
    GPtrArray *profiles;
    gchar *last_connected_path;
} ScanResult;

/**
 * FilterInput:
 * @profiles: profiles to search
 * @query: case-insensitive filter string
 * @default_path: last-connected profile path to promote in results
 * @generation: filter generation used to ignore stale callbacks
 *
 * Input payload for the profile filter background task.
 */
typedef struct {
    GPtrArray *profiles;
    gchar *query;
    gchar *default_path;
    guint generation;
} FilterInput;

/**
 * FilterResult:
 * @source_profiles: referenced source profile array
 * @matches: profiles matching the query, truncated for display
 * @total_matches: total matches before truncation
 *
 * Output payload from the profile filter background task.
 */
typedef struct {
    GPtrArray *source_profiles;
    GPtrArray *matches;
    guint total_matches;
} FilterResult;

/**
 * LaunchInput:
 * @profile_path: OpenVPN profile to connect
 * @auth_path: credentials file for the profile
 * @management_socket: Unix socket path for OpenVPN management
 *
 * Input payload for the OpenVPN launch background task.
 */
typedef struct {
    gchar *profile_path;
    gchar *auth_path;
    gchar *management_socket;
} LaunchInput;

/**
 * StateSaveInput:
 * @directory: directory containing the state file
 * @state_path: path to the last-connected state file
 * @profile_path: profile path to persist
 *
 * Input payload for the asynchronous state save task.
 */
typedef struct {
    gchar *directory;
    gchar *state_path;
    gchar *profile_path;
} StateSaveInput;

/**
 * App:
 *
 * GTK application state: profile list, active session, and management socket.
 */
struct _App {
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
};

static void app_update_buttons(App *app);
static void app_request_disconnect(App *app);
static void management_connection_close(App *app);
static void remove_management_socket(App *app);
static LineReader *line_reader_new(App *app, GInputStream *stream,
                                   gboolean management);
static void process_wait_complete(GObject *source_object, GAsyncResult *result,
                                  gpointer user_data);

/**
 * user_config_path:
 * @filename: basename within the openvpn-manager config directory
 *
 * Returns: (transfer full): path to a file under $XDG_CONFIG_HOME/openvpn-manager
 */
static gchar *user_config_path(const gchar *filename)
{
    return g_build_filename(g_get_user_config_dir(), "openvpn-manager",
                            filename, NULL);
}

/**
 * config_path:
 *
 * Returns: (transfer full): path to the manager config file
 */
static gchar *config_path(void)
{
    return user_config_path("config.ini");
}

/**
 * state_path:
 *
 * Returns: (transfer full): path to the last-connected profile state file
 */
static gchar *state_path(void)
{
    return user_config_path("state.ini");
}

/**
 * load_last_connected_path:
 *
 * Reads the last successfully connected profile path from persistent state.
 *
 * Returns: (nullable): absolute profile path, or %NULL when missing/invalid
 */
static gchar *load_last_connected_path(void)
{
    gchar *path = state_path();
    gchar *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        g_clear_error(&error);
        g_free(path);
        return NULL;
    }
    g_free(path);
    g_strstrip(contents);
    if (!g_path_is_absolute(contents) || *contents == '\0') {
        g_free(contents);
        return NULL;
    }
    return contents;
}

/**
 * app_set_status:
 * @app: application instance
 * @text: status line to display
 */
static void app_set_status(App *app, const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text);
}

/**
 * set_state:
 * @app: application instance
 * @state: new high-level connection state
 */
static void set_state(App *app, AppState state)
{
    app->state = state;
    app_update_buttons(app);
}

/**
 * app_update_buttons:
 * @app: application instance
 *
 * Enables or disables Connect and Disconnect based on session state.
 */
static void app_update_buttons(App *app)
{
    gboolean active = app->process != NULL || app->launching;
    gboolean can_connect = app->selected_profile != NULL &&
        profile_is_connectable(app->selected_profile) &&
        app->state != APP_STARTING && app->state != APP_CONNECTED &&
        app->state != APP_STOPPING;
    gtk_widget_set_sensitive(app->connect_button, can_connect && !active);
    gtk_widget_set_sensitive(app->disconnect_button, active);
}

/**
 * scan_input_free:
 * @input: (nullable): scan task input
 */
static void scan_input_free(ScanInput *input)
{
    if (input == NULL) {
        return;
    }
    g_free(input->config_path);
    g_free(input);
}

/**
 * scan_result_free:
 * @result: (nullable): scan task result
 */
static void scan_result_free(ScanResult *result)
{
    if (result == NULL) {
        return;
    }
    app_config_free(result->config);
    g_clear_pointer(&result->profiles, g_ptr_array_unref);
    g_free(result->last_connected_path);
    g_free(result);
}

/**
 * scan_worker:
 *
 * Background task that loads config, restores last-connected state, and scans
 * the profile directory.
 */
static void scan_worker(GTask *task, gpointer source_object, gpointer task_data,
                        GCancellable *cancellable)
{
    (void) source_object;
    ScanInput *input = task_data;
    ScanResult *result = g_new0(ScanResult, 1);
    result->config = app_config_new();
    GError *error = NULL;
    if (!app_config_load(result->config, input->config_path, &error)) {
        scan_result_free(result);
        g_task_return_error(task, error);
        return;
    }
    result->last_connected_path = load_last_connected_path();
    result->profiles = profiles_scan_cancelable(
        result->config->profile_directory, result->config, cancellable, &error);
    if (result->profiles == NULL) {
        scan_result_free(result);
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, result, (GDestroyNotify) scan_result_free);
}

/**
 * clear_list_box:
 * @app: application instance
 *
 * Removes all rows from the profile list box.
 */
static void clear_list_box(App *app)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->list_box));
    for (GList *item = children; item != NULL; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

/**
 * update_selected_profile:
 * @app: application instance
 *
 * Updates @app selected profile from the current list box selection.
 */
static void update_selected_profile(App *app)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->list_box));
    app->selected_profile = row == NULL
        ? NULL : g_object_get_data(G_OBJECT(row), "profile");
    app_update_buttons(app);
}

/**
 * row_selected:
 *
 * GTK callback invoked when the profile list selection changes.
 */
static void row_selected(GtkListBox *list_box, GtkListBoxRow *row, gpointer data)
{
    (void) list_box;
    (void) row;
    update_selected_profile(data);
}

/**
 * profile_row:
 * @profile: profile to display
 *
 * Builds a list box row showing profile name, hostname, and status.
 *
 * Returns: (transfer full): new list box row widget
 */
static GtkWidget *profile_row(Profile *profile)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "profile", profile);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    GtkWidget *name = gtk_label_new(profile->display_name);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_widget_set_halign(name, GTK_ALIGN_FILL);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);

    gchar *details = NULL;
    if (profile->error != NULL) {
        details = g_strdup_printf("%s  —  %s", profile->hostname != NULL
                                  ? profile->hostname : "invalid profile",
                                  profile->error);
    } else {
        details = g_strdup_printf("%s  —  %s", profile->hostname,
                                  profile->credential_id);
    }
    GtkWidget *subtitle = gtk_label_new(details);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0F);
    gtk_widget_set_halign(subtitle, GTK_ALIGN_FILL);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle),
                                profile->error == NULL ? "dim-label" : "error");
    gtk_box_pack_start(GTK_BOX(box), subtitle, FALSE, FALSE, 0);
    g_free(details);

    gtk_container_add(GTK_CONTAINER(row), box);
    return row;
}

/**
 * show_filter_result:
 * @app: application instance
 * @result: filtered profile results
 * @generation: filter generation that must match to update the UI
 *
 * Repopulates the profile list from a completed filter task.
 */
static void show_filter_result(App *app, FilterResult *result, guint generation)
{
    if (generation != app->filter_generation) {
        return;
    }
    clear_list_box(app);
    GtkListBoxRow *default_row = NULL;
    for (guint index = 0; index < result->matches->len; index++) {
        Profile *profile = g_ptr_array_index(result->matches, index);
        GtkWidget *row = profile_row(profile);
        if (app->last_connected_path != NULL &&
            g_strcmp0(profile->path, app->last_connected_path) == 0) {
            default_row = GTK_LIST_BOX_ROW(row);
        }
        gtk_list_box_insert(GTK_LIST_BOX(app->list_box), row, -1);
    }
    gtk_widget_show_all(app->list_box);
    app->selected_profile = NULL;
    if (default_row != NULL) {
        gtk_list_box_select_row(GTK_LIST_BOX(app->list_box), default_row);
    }
    app_update_buttons(app);
    if (result->total_matches > MAX_VISIBLE_RESULTS) {
        gchar *text = g_strdup_printf("%u matches (showing first %u)",
                                      result->total_matches, MAX_VISIBLE_RESULTS);
        app_set_status(app, text);
        g_free(text);
    } else {
        gchar *text = g_strdup_printf("%u profile%s", result->total_matches,
                                      result->total_matches == 1U ? "" : "s");
        app_set_status(app, text);
        g_free(text);
    }
}

/**
 * filter_input_free:
 * @input: (nullable): filter task input
 */
static void filter_input_free(FilterInput *input)
{
    if (input == NULL) {
        return;
    }
    g_clear_pointer(&input->profiles, g_ptr_array_unref);
    g_free(input->query);
    g_free(input->default_path);
    g_free(input);
}

/**
 * filter_result_free:
 * @result: (nullable): filter task result
 */
static void filter_result_free(FilterResult *result)
{
    if (result == NULL) {
        return;
    }
    g_clear_pointer(&result->source_profiles, g_ptr_array_unref);
    g_clear_pointer(&result->matches, g_ptr_array_unref);
    g_free(result);
}

/**
 * promote_default_profile:
 * @matches: visible filtered profile list to reorder
 * @profiles: full source profile list
 * @default_path: last-connected profile path
 * @query: active search query
 *
 * Moves the last-connected profile to the top of @matches when it matches @query.
 */
static void promote_default_profile(GPtrArray *matches, GPtrArray *profiles,
                                    const gchar *default_path, const gchar *query)
{
    Profile *default_profile = NULL;
    for (guint index = 0; index < profiles->len; index++) {
        Profile *profile = g_ptr_array_index(profiles, index);
        if (g_strcmp0(profile->path, default_path) == 0 &&
            profile_matches(profile, query)) {
            default_profile = profile;
            break;
        }
    }
    if (default_profile == NULL) {
        return;
    }
    guint visible_index = G_MAXUINT;
    for (guint index = 0; index < matches->len; index++) {
        if (g_ptr_array_index(matches, index) == default_profile) {
            visible_index = index;
            break;
        }
    }
    if (visible_index != G_MAXUINT && visible_index != 0U) {
        g_ptr_array_remove_index(matches, visible_index);
        g_ptr_array_insert(matches, 0, default_profile);
    } else if (visible_index == G_MAXUINT && matches->len > 0U) {
        g_ptr_array_index(matches, 0) = default_profile;
    }
}

/**
 * filter_worker:
 *
 * Background task that filters the profile list and promotes the last-connected
 * profile to the top of visible results when it matches the query.
 */
static void filter_worker(GTask *task, gpointer source_object, gpointer task_data,
                          GCancellable *cancellable)
{
    (void) source_object;
    FilterInput *input = task_data;
    FilterResult *result = g_new0(FilterResult, 1);
    result->source_profiles = g_ptr_array_ref(input->profiles);
    result->matches = profiles_filter_cancelable(
        input->profiles, input->query, MAX_VISIBLE_RESULTS, cancellable,
        &result->total_matches);

    if (result->matches != NULL && input->default_path != NULL) {
        promote_default_profile(result->matches, input->profiles,
                                input->default_path, input->query);
    }

    if (result->matches == NULL) {
        filter_result_free(result);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "filter cancelled");
        return;
    }
    g_task_return_pointer(task, result, (GDestroyNotify) filter_result_free);
}

/**
 * filter_complete:
 *
 * GTK callback invoked when a profile filter background task finishes.
 */
static void filter_complete(GObject *source_object, GAsyncResult *result,
                            gpointer user_data)
{
    (void) source_object;
    App *app = user_data;
    GError *error = NULL;
    FilterInput *input = g_task_get_task_data(G_TASK(result));
    guint generation = input != NULL ? input->generation : 0U;
    FilterResult *filter_result = g_task_propagate_pointer(G_TASK(result), &error);
    if (input != NULL && input->generation == app->filter_generation) {
        g_clear_object(&app->filter_cancellable);
    }
    if (filter_result == NULL) {
        if (error != NULL) {
            app_set_status(app, error->message);
            g_clear_error(&error);
        }
        return;
    }
    show_filter_result(app, filter_result, generation);
    filter_result_free(filter_result);
}

/**
 * start_filter:
 * @data: application instance
 *
 * Starts a debounced profile filter task.
 *
 * Returns: %G_SOURCE_REMOVE
 */
static gboolean start_filter(gpointer data)
{
    App *app = data;
    app->filter_timeout_id = 0;
    FilterInput *input = g_new0(FilterInput, 1);
    input->profiles = g_ptr_array_ref(app->profiles);
    input->query = g_strdup(gtk_entry_get_text(GTK_ENTRY(app->search_entry)));
    input->default_path = g_strdup(app->last_connected_path);
    input->generation = app->filter_generation;
    if (app->filter_cancellable != NULL) {
        g_cancellable_cancel(app->filter_cancellable);
        g_clear_object(&app->filter_cancellable);
    }
    app->filter_cancellable = g_cancellable_new();
    GTask *task = g_task_new(NULL, app->filter_cancellable, filter_complete, app);
    g_task_set_task_data(task, input, (GDestroyNotify) filter_input_free);
    g_task_run_in_thread(task, filter_worker);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

/**
 * search_changed:
 *
 * GTK callback invoked when the search entry text changes.
 */
static void search_changed(GtkEditable *editable, gpointer data)
{
    (void) editable;
    App *app = data;
    app->filter_generation++;
    if (app->filter_timeout_id != 0U) {
        g_source_remove(app->filter_timeout_id);
    }
    app->filter_timeout_id = g_timeout_add(120U, start_filter, app);
}

/**
 * scan_complete:
 *
 * GTK callback invoked when the initial profile scan finishes.
 */
static void scan_complete(GObject *source_object, GAsyncResult *result,
                          gpointer user_data)
{
    (void) source_object;
    App *app = user_data;
    GError *error = NULL;
    ScanResult *scan_result = g_task_propagate_pointer(G_TASK(result), &error);
    gtk_spinner_stop(GTK_SPINNER(app->progress));
    gtk_widget_hide(app->progress);
    g_clear_object(&app->scan_cancellable);
    if (scan_result == NULL) {
        set_state(app, APP_ERROR);
        app_set_status(app, error != NULL ? error->message : "Profile scan failed");

        g_clear_error(&error);
        return;
    }
    app_config_clear(&app->config);
    AppConfig *loaded_config = scan_result->config;
    scan_result->config = NULL;
    app->config.profile_directory = loaded_config->profile_directory;
    app->config.credential_rules = loaded_config->credential_rules;
    loaded_config->profile_directory = NULL;
    loaded_config->credential_rules = NULL;
    app_config_free(loaded_config);
    g_clear_pointer(&app->profiles, g_ptr_array_unref);
    app->profiles = scan_result->profiles;
    scan_result->profiles = NULL;
    g_free(app->last_connected_path);
    app->last_connected_path = scan_result->last_connected_path;
    scan_result->last_connected_path = NULL;
    scan_result_free(scan_result);
    search_changed(NULL, app);
}

/**
 * begin_scan:
 * @data: application instance
 *
 * Starts the initial profile scan after window creation.
 *
 * Returns: %G_SOURCE_REMOVE
 */
static gboolean begin_scan(gpointer data)
{
    App *app = data;
    gchar *path = config_path();
    ScanInput *input = g_new0(ScanInput, 1);
    input->config_path = path;
    g_clear_object(&app->scan_cancellable);
    app->scan_cancellable = g_cancellable_new();
    gtk_spinner_start(GTK_SPINNER(app->progress));
    gtk_widget_show(app->progress);
    GTask *task = g_task_new(NULL, app->scan_cancellable, scan_complete, app);
    g_task_set_task_data(task, input, (GDestroyNotify) scan_input_free);
    g_task_run_in_thread(task, scan_worker);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

/**
 * line_reader_free:
 * @reader: (nullable): line reader to destroy
 */
static void line_reader_free(LineReader *reader)
{
    if (reader == NULL) {
        return;
    }
    g_clear_object(&reader->stream);
    g_clear_pointer(&reader->pending, g_byte_array_unref);
    g_free(reader);
}

/**
 * state_save_input_free:
 * @input: (nullable): state save task input
 */
static void state_save_input_free(StateSaveInput *input)
{
    if (input == NULL) {
        return;
    }
    g_free(input->directory);
    g_free(input->state_path);
    g_free(input->profile_path);
    g_free(input);
}

/**
 * save_state_worker:
 *
 * Background task that persists the last-connected profile path.
 */
static void save_state_worker(GTask *task, gpointer source_object, gpointer task_data,
                              GCancellable *cancellable)
{
    (void) source_object;
    (void) cancellable;
    StateSaveInput *input = task_data;
    GError *error = NULL;
    gboolean saved = g_mkdir_with_parents(input->directory, 0700) == 0 &&
        g_file_set_contents(input->state_path, input->profile_path, -1, &error);
    g_clear_error(&error);
    g_task_return_boolean(task, saved);
}

/**
 * save_last_connected_path:
 * @app: application instance
 *
 * Asynchronously writes the active profile path to persistent state.
 */
static void save_last_connected_path(App *app)
{
    if (app->active_profile == NULL) {
        return;
    }
    gchar *state = state_path();
    StateSaveInput *input = g_new0(StateSaveInput, 1);
    input->directory = g_path_get_dirname(state);
    input->state_path = state;
    input->profile_path = g_strdup(app->active_profile->path);
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, input, (GDestroyNotify) state_save_input_free);
    g_task_run_in_thread(task, save_state_worker);
    g_object_unref(task);
}

/**
 * process_openvpn_line:
 * @app: application instance
 * @line: single line of OpenVPN stdout, stderr, or management output
 * @management: %TRUE when @line came from the management interface
 * @generation: session generation that must match to apply updates
 *
 * Updates connection status from OpenVPN log and management state events.
 */
static void process_openvpn_line(App *app, const gchar *line, gboolean management,
                                 guint generation)
{
    if (generation != app->session_generation) {
        return;
    }
    gchar *clean = g_strdup(line);
    g_strchomp(clean);
    if (management && g_str_has_prefix(clean, ">STATE:")) {
        gchar **parts = g_strsplit(clean + strlen(">STATE:"), ",", 4);
        if (parts[1] != NULL && g_strcmp0(parts[1], "CONNECTED") == 0) {
            set_state(app, APP_CONNECTED);
            app_set_status(app, "Connected");
            save_last_connected_path(app);
        } else if (parts[1] != NULL && g_strcmp0(parts[1], "RECONNECTING") == 0) {
            app_set_status(app, "Reconnecting");
        }
        g_strfreev(parts);
    } else if (strstr(clean, "Initialization Sequence Completed") != NULL) {
        set_state(app, APP_CONNECTED);
        app_set_status(app, "Connected");
        save_last_connected_path(app);
    } else if (strstr(clean, "AUTH_FAILED") != NULL) {
        app_set_status(app, "OpenVPN authentication failed");
    } else if (strstr(clean, "FATAL") != NULL || strstr(clean, "ERROR") != NULL) {
        app_set_status(app, "OpenVPN reported an error");
    }
    g_free(clean);
}

static void line_reader_read(LineReader *reader);

/**
 * line_reader_read_complete:
 *
 * Async read callback that extracts complete lines from OpenVPN output.
 */
static void line_reader_read_complete(GObject *source_object, GAsyncResult *result,
                                       gpointer user_data)
{
    (void) source_object;
    LineReader *reader = user_data;
    GError *error = NULL;
    gssize count = g_input_stream_read_finish(reader->stream, result, &error);
    if (count <= 0) {
        g_clear_error(&error);
        if (reader->management && reader->app->management_reader == reader) {
            reader->app->management_reader = NULL;
        }
        line_reader_free(reader);
        return;
    }
    g_byte_array_append(reader->pending, reader->buffer, (guint) count);
    while (TRUE) {
        guint newline = G_MAXUINT;
        for (guint index = 0; index < reader->pending->len; index++) {
            if (reader->pending->data[index] == '\n') {
                newline = index;
                break;
            }
        }
        if (newline == G_MAXUINT) {
            break;
        }
        guint line_length = MIN(newline, MAX_OUTPUT_LINE);
        gchar *line = g_strndup((const gchar *) reader->pending->data, line_length);
        process_openvpn_line(reader->app, line, reader->management,
                             reader->generation);
        g_free(line);
        g_byte_array_remove_range(reader->pending, 0, newline + 1U);
    }
    if (reader->pending->len > MAX_OUTPUT_LINE) {
        g_byte_array_set_size(reader->pending, 0);
    }
    line_reader_read(reader);
}

/**
 * line_reader_read:
 * @reader: line reader to feed
 *
 * Starts or continues asynchronous reads from @reader stream.
 */
static void line_reader_read(LineReader *reader)
{
    g_input_stream_read_async(reader->stream, reader->buffer, READ_BUFFER_SIZE,
                              G_PRIORITY_DEFAULT, NULL,
                              line_reader_read_complete, reader);
}

/**
 * line_reader_new:
 * @app: owning application instance
 * @stream: input stream to read from
 * @management: %TRUE when reading the management interface
 *
 * Creates a line reader and starts reading from @stream.
 *
 * Returns: (transfer full): new line reader
 */
static LineReader *line_reader_new(App *app, GInputStream *stream,
                                   gboolean management)
{
    LineReader *reader = g_new0(LineReader, 1);
    reader->app = app;
    reader->stream = g_object_ref(stream);
    reader->pending = g_byte_array_new();
    reader->management = management;
    reader->generation = app->session_generation;
    line_reader_read(reader);
    return reader;
}

/**
 * management_write_complete:
 *
 * Ignores completion of a management interface write.
 */
static void management_write_complete(GObject *source_object, GAsyncResult *result,
                                       gpointer user_data)
{
    (void) source_object;
    (void) user_data;
    GError *error = NULL;
    gsize bytes_written = 0;
    g_output_stream_write_all_finish(G_OUTPUT_STREAM(source_object), result,
                                     &bytes_written, &error);
    (void) bytes_written;
    g_clear_error(&error);
}

/**
 * stream_close_complete:
 *
 * Ignores completion of an asynchronous stream close.
 */
static void stream_close_complete(GObject *source_object, GAsyncResult *result,
                                  gpointer user_data)
{
    (void) user_data;
    GError *error = NULL;
    g_io_stream_close_finish(G_IO_STREAM(source_object), result, &error);
    g_clear_error(&error);
}

/**
 * close_stream_async:
 * @stream: stream to close without blocking the main loop
 */
static void close_stream_async(GIOStream *stream)
{
    g_io_stream_close_async(stream, G_PRIORITY_DEFAULT, NULL,
                            stream_close_complete, NULL);
}

/**
 * management_connection_close:
 * @app: application instance
 *
 * Closes the active management connection and stops its socket service.
 */
static void management_connection_close(App *app)
{
    if (app->management_connection != NULL) {
        close_stream_async(G_IO_STREAM(app->management_connection));
        g_clear_object(&app->management_connection);
    }
    if (app->management_service != NULL) {
        g_socket_service_stop(app->management_service);
        g_clear_object(&app->management_service);
    }
}

/**
 * management_incoming:
 *
 * Accepts the OpenVPN management interface connection and enables state events.
 */
static gboolean management_incoming(GSocketService *service,
                                     GSocketConnection *connection,
                                     GObject *source_object, gpointer user_data)
{
    (void) service;
    (void) source_object;
    App *app = user_data;
    if (app->management_connection != NULL ||
        (app->process == NULL && !app->launching)) {
        close_stream_async(G_IO_STREAM(connection));
        return TRUE;
    }
    app->management_connection = g_object_ref(connection);
    app->management_reader = line_reader_new(
        app, g_io_stream_get_input_stream(G_IO_STREAM(connection)), TRUE);
    const gchar command[] = "state on\n";
    g_output_stream_write_all_async(
        g_io_stream_get_output_stream(G_IO_STREAM(connection)), command,
        sizeof(command) - 1U, G_PRIORITY_DEFAULT, NULL,
        management_write_complete, app);
    return TRUE;
}

/**
 * remove_management_socket:
 * @app: application instance
 *
 * Tears down the management socket and its private runtime directory.
 */
static void remove_management_socket(App *app)
{
    management_connection_close(app);
    if (app->management_socket != NULL) {
        g_remove(app->management_socket);
        g_clear_pointer(&app->management_socket, g_free);
    }
    if (app->management_directory != NULL) {
        g_rmdir(app->management_directory);
        g_clear_pointer(&app->management_directory, g_free);
    }
}

/**
 * create_management_socket:
 * @app: application instance
 * @error: return location for a #GError
 *
 * Creates a private runtime directory and Unix management socket for OpenVPN.
 *
 * Returns: %TRUE on success
 */
static gboolean create_management_socket(App *app, GError **error)
{
    const gchar *runtime = g_get_user_runtime_dir();
    gchar *template = g_build_filename(runtime, "openvpn-manager-XXXXXX", NULL);
    if (g_mkdtemp(template) == NULL) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot create private runtime directory: %s", g_strerror(errno));
        g_free(template);
        return FALSE;
    }
    if (chmod(template, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot secure runtime directory: %s", g_strerror(errno));
        g_rmdir(template);
        g_free(template);
        return FALSE;
    }
    app->management_directory = template;
    app->management_socket = g_build_filename(template, "management.sock", NULL);
    app->management_service = g_socket_service_new();
    GSocketAddress *address = g_unix_socket_address_new(app->management_socket);
    GSocketAddress *effective = NULL;
    gboolean added = g_socket_listener_add_address(
        G_SOCKET_LISTENER(app->management_service), address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT, NULL, &effective, error);
    g_clear_object(&effective);
    g_object_unref(address);
    if (!added) {
        remove_management_socket(app);
        return FALSE;
    }
    if (chmod(app->management_socket, 0600) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot secure management socket: %s", g_strerror(errno));
        remove_management_socket(app);
        return FALSE;
    }
    g_signal_connect(app->management_service, "incoming",
                     G_CALLBACK(management_incoming), app);
    g_socket_service_start(app->management_service);
    return TRUE;
}

/**
 * process_wait_complete:
 *
 * GTK callback invoked when the OpenVPN subprocess exits.
 */
static void process_wait_complete(GObject *source_object, GAsyncResult *result,
                                  gpointer user_data)
{
    App *app = user_data;
    GError *error = NULL;
    gboolean exited = g_subprocess_wait_finish(G_SUBPROCESS(source_object), result,
                                               &error);
    if (!exited) {
        app_set_status(app, error != NULL ? error->message : "OpenVPN wait failed");
        g_clear_error(&error);
    }
    if (app->stop_timeout_id != 0U) {
        g_source_remove(app->stop_timeout_id);
        app->stop_timeout_id = 0U;
    }
    gboolean success = g_subprocess_get_if_exited(app->process) &&
        g_subprocess_get_exit_status(app->process) == 0;
    g_clear_object(&app->process);
    remove_management_socket(app);
    if (app->quitting) {
        g_application_quit(G_APPLICATION(app->application));
        return;
    }
    if (success || app->state == APP_STOPPING) {
        set_state(app, APP_IDLE);
        app_set_status(app, "Disconnected");
    } else {
        set_state(app, APP_ERROR);
        app_set_status(app, "OpenVPN exited before connecting");
    }
}

/**
 * stop_timeout:
 * @data: application instance
 *
 * Forces OpenVPN to exit when a graceful stop takes too long.
 *
 * Returns: %G_SOURCE_REMOVE
 */
static gboolean stop_timeout(gpointer data)
{
    App *app = data;
    app->stop_timeout_id = 0;
    if (app->process != NULL) {
        app_set_status(app, "OpenVPN did not stop promptly; closing it now");
        g_subprocess_force_exit(app->process);
    } else if (app->launching) {
        app_set_status(app, "Waiting for OpenVPN launch to finish");
    }
    if (app->quitting) {
        g_application_quit(G_APPLICATION(app->application));
    }
    return G_SOURCE_REMOVE;
}

/**
 * app_request_disconnect:
 * @app: application instance
 *
 * Begins stopping the active or launching OpenVPN session.
 */
static void app_request_disconnect(App *app)
{
    if (app->process == NULL && !app->launching) {
        return;
    }
    app->stop_requested = TRUE;
    if (app->state != APP_STOPPING) {
        set_state(app, APP_STOPPING);
        app_set_status(app, "Disconnecting");
    }
    management_connection_close(app);
    if (app->stop_timeout_id == 0U) {
        app->stop_timeout_id = g_timeout_add_seconds(STOP_TIMEOUT_SECONDS,
                                                     stop_timeout, app);
    }
}

/**
 * launch_input_free:
 * @input: (nullable): launch task input
 */
static void launch_input_free(LaunchInput *input)
{
    if (input == NULL) {
        return;
    }
    g_free(input->profile_path);
    g_free(input->auth_path);
    g_free(input->management_socket);
    g_free(input);
}

/**
 * launch_worker:
 *
 * Background task that spawns OpenVPN via pkexec.
 */
static void launch_worker(GTask *task, gpointer source_object, gpointer task_data,
                          GCancellable *cancellable)
{
    (void) source_object;
    (void) cancellable;
    LaunchInput *input = task_data;
    gchar *argv[] = {
        (gchar *) "/usr/bin/pkexec",
        (gchar *) "/usr/sbin/openvpn",
        (gchar *) "--config", input->profile_path,
        (gchar *) "--auth-user-pass", input->auth_path,
        (gchar *) "--auth-nocache",
        (gchar *) "--management-client",
        (gchar *) "--management", input->management_socket,
        (gchar *) "unix",
        (gchar *) "--verb", (gchar *) "3", NULL
    };
    GError *error = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    GSubprocess *process = g_subprocess_launcher_spawnv(
        launcher, (const gchar *const *) argv, &error);
    g_object_unref(launcher);
    if (process == NULL) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, process, g_object_unref);
}

/**
 * launch_complete:
 *
 * GTK callback invoked when OpenVPN launch finishes.
 */
static void launch_complete(GObject *source_object, GAsyncResult *result,
                            gpointer user_data)
{
    (void) source_object;
    App *app = user_data;
    GError *error = NULL;
    GSubprocess *process = g_task_propagate_pointer(G_TASK(result), &error);
    app->launching = FALSE;
    if (process == NULL) {
        remove_management_socket(app);
        set_state(app, APP_ERROR);
        app_set_status(app, error != NULL ? error->message : "OpenVPN launch failed");
        g_clear_error(&error);
        if (app->quitting) {
            g_application_quit(G_APPLICATION(app->application));
        }
        return;
    }
    app->process = process;
    line_reader_new(app, g_subprocess_get_stdout_pipe(process), FALSE);
    line_reader_new(app, g_subprocess_get_stderr_pipe(process), FALSE);
    g_subprocess_wait_async(process, NULL, process_wait_complete, app);
    if (app->stop_requested) {
        management_connection_close(app);
    } else {
        app_set_status(app, "Waiting for authorization / OpenVPN");
    }
}

/**
 * connect_clicked:
 *
 * GTK callback invoked when Connect is pressed.
 */
static void connect_clicked(GtkButton *button, gpointer data)
{
    (void) button;
    App *app = data;
    Profile *profile = app->selected_profile;
    if (!profile_is_connectable(profile)) {
        return;
    }
    GError *error = NULL;
    if (!create_management_socket(app, &error)) {
        app_set_status(app, error->message);
        g_clear_error(&error);
        return;
    }
    LaunchInput *input = g_new0(LaunchInput, 1);
    input->profile_path = g_strdup(profile->path);
    input->auth_path = g_strdup(profile->auth_file);
    input->management_socket = g_strdup(app->management_socket);
    app->launching = TRUE;
    app->stop_requested = FALSE;
    app->active_profile = profile;
    app->session_generation++;
    set_state(app, APP_STARTING);
    app_set_status(app, "Waiting for authorization / OpenVPN");
    GTask *task = g_task_new(NULL, NULL, launch_complete, app);
    g_task_set_task_data(task, input, (GDestroyNotify) launch_input_free);
    g_task_run_in_thread(task, launch_worker);
    g_object_unref(task);
}

/**
 * disconnect_clicked:
 *
 * GTK callback invoked when Disconnect is pressed.
 */
static void disconnect_clicked(GtkButton *button, gpointer data)
{
    (void) button;
    app_request_disconnect(data);
}

/**
 * window_delete_event:
 *
 * GTK callback that disconnects before closing when a session is active.
 */
static gboolean window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void) widget;
    (void) event;
    App *app = data;
    if (app->process != NULL || app->launching) {
        app->quitting = TRUE;
        app_request_disconnect(app);
        return TRUE;
    }
    return FALSE;
}

/**
 * activate:
 *
 * GTK application activate handler that creates the main window.
 */
static void activate(GtkApplication *application, gpointer user_data)
{
    App *app = user_data;
    if (app->window != NULL) {
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }
    app->application = application;
    app->profiles = g_ptr_array_new_with_free_func((GDestroyNotify) profile_free);
    app->state = APP_IDLE;
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "OpenVPN Manager");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 620, 500);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(window_delete_event), app);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(app->window), outer);

    app->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->search_entry),
                                   "Search endpoint filename or hostname");
    g_signal_connect(app->search_entry, "changed", G_CALLBACK(search_changed), app);
    gtk_box_pack_start(GTK_BOX(outer), app->search_entry, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    app->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list_box), GTK_SELECTION_SINGLE);
    g_signal_connect(app->list_box, "row-selected", G_CALLBACK(row_selected), app);
    gtk_container_add(GTK_CONTAINER(scroll), app->list_box);

    app->status_label = gtk_label_new("Starting…");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0F);
    gtk_box_pack_start(GTK_BOX(outer), app->status_label, FALSE, FALSE, 0);
    app->progress = gtk_spinner_new();
    gtk_widget_set_no_show_all(app->progress, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), app->progress, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_pack_start(GTK_BOX(outer), buttons, FALSE, FALSE, 0);
    app->connect_button = gtk_button_new_with_label("Connect");
    app->disconnect_button = gtk_button_new_with_label("Disconnect");
    g_signal_connect(app->connect_button, "clicked", G_CALLBACK(connect_clicked), app);
    g_signal_connect(app->disconnect_button, "clicked", G_CALLBACK(disconnect_clicked), app);
    gtk_container_add(GTK_CONTAINER(buttons), app->connect_button);
    gtk_container_add(GTK_CONTAINER(buttons), app->disconnect_button);
    app_update_buttons(app);

    gtk_widget_show_all(app->window);
    g_timeout_add(50U, begin_scan, app);
}

/**
 * app_cleanup:
 * @app: application instance
 *
 * Releases resources owned by @app at shutdown.
 */
static void app_cleanup(App *app)
{
    if (app->filter_timeout_id != 0U) {
        g_source_remove(app->filter_timeout_id);
    }
    if (app->scan_cancellable != NULL) {
        g_cancellable_cancel(app->scan_cancellable);
    }
    if (app->filter_cancellable != NULL) {
        g_cancellable_cancel(app->filter_cancellable);
    }
    g_clear_object(&app->scan_cancellable);
    g_clear_object(&app->filter_cancellable);
    if (app->process != NULL) {
        management_connection_close(app);
    }
    remove_management_socket(app);
    g_clear_object(&app->process);
    g_clear_pointer(&app->profiles, g_ptr_array_unref);
    g_clear_pointer(&app->last_connected_path, g_free);
    app_config_clear(&app->config);
}

/**
 * main:
 *
 * Entry point: runs the GTK application event loop.
 */
int main(int argc, char **argv)
{
    (void) argv;
    App app = {0};
    app_config_init(&app.config);
    GtkApplication *application = gtk_application_new(
        "com.example.OpenVPNManager", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    app_cleanup(&app);
    g_object_unref(application);
    return status;
}
