#include "app.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

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

static gchar *user_config_path(const gchar *filename)
{
    return g_build_filename(g_get_user_config_dir(), "openvpn-manager",
                            filename, NULL);
}

gchar *app_config_path(void)
{
    return user_config_path("config.ini");
}

gchar *app_state_path(void)
{
    return user_config_path("state.ini");
}

gchar *app_load_last_connected_path(void)
{
    gchar *path = app_state_path();
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

void app_set_status(App *app, const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text);
}

void app_set_state(App *app, AppState state)
{
    app->state = state;
    app_update_buttons(app);
}

void app_update_buttons(App *app)
{
    gboolean active = app->process != NULL || app->launching;
    gboolean can_connect = app->selected_profile != NULL &&
        profile_is_connectable(app->selected_profile) &&
        app->state != APP_STARTING && app->state != APP_CONNECTED &&
        app->state != APP_STOPPING;
    gtk_widget_set_sensitive(app->connect_button, can_connect && !active);
    gtk_widget_set_sensitive(app->disconnect_button, active);
}

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

static void save_state_worker(GTask *task, gpointer source_object, gpointer task_data,
                              GCancellable *cancellable)
{
    (void) source_object;
    (void) cancellable;
    StateSaveInput *input = task_data;
    GError *error = NULL;
    gboolean saved = g_mkdir_with_parents(input->directory, 0700) == 0 &&
        g_file_set_contents(input->state_path, input->profile_path, -1, &error);
    (void) saved;
    g_clear_error(&error);
    g_task_return_boolean(task, saved);
}

void app_save_last_connected_path(App *app)
{
    if (app->active_profile == NULL) {
        return;
    }
    gchar *state = app_state_path();
    StateSaveInput *input = g_new0(StateSaveInput, 1);
    input->directory = g_path_get_dirname(state);
    input->state_path = state;
    input->profile_path = g_strdup(app->active_profile->path);
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, input, (GDestroyNotify) state_save_input_free);
    g_task_run_in_thread(task, save_state_worker);
    g_object_unref(task);
}

void app_cleanup(App *app)
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
        app_management_connection_close(app);
    }
    app_management_remove_socket(app);
    g_clear_object(&app->process);
    g_clear_pointer(&app->profiles, g_ptr_array_unref);
    g_clear_pointer(&app->last_connected_path, g_free);
    app_config_clear(&app->config);
}
