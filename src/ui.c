#include "app.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

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

static void search_changed(GtkEditable *editable, gpointer data);

static void scan_input_free(ScanInput *input)
{
    if (input == NULL) {
        return;
    }
    g_free(input->config_path);
    g_free(input);
}

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
    result->last_connected_path = app_load_last_connected_path();
    result->profiles = profiles_scan_cancelable(
        result->config->profile_directory, result->config, cancellable, &error);
    if (result->profiles == NULL) {
        scan_result_free(result);
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, result, (GDestroyNotify) scan_result_free);
}

static void clear_list_box(App *app)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->list_box));
    for (GList *item = children; item != NULL; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

static void update_selected_profile(App *app)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->list_box));
    app->selected_profile = row == NULL
        ? NULL : g_object_get_data(G_OBJECT(row), "profile");
    app_update_buttons(app);
}

static void row_selected(GtkListBox *list_box, GtkListBoxRow *row, gpointer data)
{
    (void) list_box;
    (void) row;
    update_selected_profile(data);
}

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
    if (result->total_matches > APP_MAX_VISIBLE_RESULTS) {
        gchar *text = g_strdup_printf("%u matches (showing first %u)",
                                      result->total_matches,
                                      APP_MAX_VISIBLE_RESULTS);
        app_set_status(app, text);
        g_free(text);
    } else {
        gchar *text = g_strdup_printf("%u profile%s", result->total_matches,
                                      result->total_matches == 1U ? "" : "s");
        app_set_status(app, text);
        g_free(text);
    }
}

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

static void filter_result_free(FilterResult *result)
{
    if (result == NULL) {
        return;
    }
    g_clear_pointer(&result->source_profiles, g_ptr_array_unref);
    g_clear_pointer(&result->matches, g_ptr_array_unref);
    g_free(result);
}

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

static void filter_worker(GTask *task, gpointer source_object, gpointer task_data,
                          GCancellable *cancellable)
{
    (void) source_object;
    FilterInput *input = task_data;
    FilterResult *result = g_new0(FilterResult, 1);
    result->source_profiles = g_ptr_array_ref(input->profiles);
    result->matches = profiles_filter_cancelable(
        input->profiles, input->query, APP_MAX_VISIBLE_RESULTS, cancellable,
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
        app_set_state(app, APP_ERROR);
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

static gboolean begin_scan(gpointer data)
{
    App *app = data;
    gchar *path = app_config_path();
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

static void connect_clicked(GtkButton *button, gpointer data)
{
    (void) button;
    App *app = data;
    if (!profile_is_connectable(app->selected_profile)) {
        return;
    }
    app_session_connect(app, app->selected_profile);
}

static void disconnect_clicked(GtkButton *button, gpointer data)
{
    (void) button;
    app_session_disconnect(data);
}

static gboolean window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void) widget;
    (void) event;
    App *app = data;
    if (app->process != NULL || app->launching) {
        app->quitting = TRUE;
        app_session_disconnect(app);
        return TRUE;
    }
    return FALSE;
}

void app_ui_activate(GtkApplication *application, App *app)
{
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
