#include "app.h"

#include "line_reader.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

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

static void process_wait_complete(GObject *source_object, GAsyncResult *result,
                                  gpointer user_data);

static void launch_complete(GObject *source_object, GAsyncResult *result,
                            gpointer user_data)
{
    (void) source_object;
    App *app = user_data;
    GError *error = NULL;
    GSubprocess *process = g_task_propagate_pointer(G_TASK(result), &error);
    app->launching = FALSE;
    if (process == NULL) {
        app_management_remove_socket(app);
        app_set_state(app, APP_ERROR);
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
        app_management_connection_close(app);
    } else {
        app_set_status(app, "Waiting for authorization / OpenVPN");
    }
}

void app_session_process_line(App *app, const gchar *line, gboolean management,
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
            app_set_state(app, APP_CONNECTED);
            app_set_status(app, "Connected");
            app_save_last_connected_path(app);
        } else if (parts[1] != NULL && g_strcmp0(parts[1], "RECONNECTING") == 0) {
            app_set_status(app, "Reconnecting");
        }
        g_strfreev(parts);
    } else if (strstr(clean, "Initialization Sequence Completed") != NULL) {
        app_set_state(app, APP_CONNECTED);
        app_set_status(app, "Connected");
        app_save_last_connected_path(app);
    } else if (strstr(clean, "AUTH_FAILED") != NULL) {
        app_set_status(app, "OpenVPN authentication failed");
    } else if (strstr(clean, "FATAL") != NULL || strstr(clean, "ERROR") != NULL) {
        app_set_status(app, "OpenVPN reported an error");
    }
    g_free(clean);
}

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
    app_management_remove_socket(app);
    
    if (app->quitting) {
        g_application_quit(G_APPLICATION(app->application));
        return;
    }
    if (success || app->state == APP_STOPPING) {
        app_set_state(app, APP_IDLE);
        app_set_status(app, "Disconnected");
    } else {
        app_set_state(app, APP_ERROR);
        app_set_status(app, "OpenVPN exited before connecting");
    }
}

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

void app_session_disconnect(App *app)
{
    if (app->process == NULL && !app->launching) {
        return;
    }
    app->stop_requested = TRUE;
    if (app->state != APP_STOPPING) {
        app_set_state(app, APP_STOPPING);
        app_set_status(app, "Disconnecting");
    }
    app_management_connection_close(app);
    if (app->stop_timeout_id == 0U) {
        app->stop_timeout_id = g_timeout_add_seconds(APP_STOP_TIMEOUT_SECONDS,
                                                     stop_timeout, app);
    }
}

void app_session_connect(App *app, Profile *profile)
{
    GError *error = NULL;
    if (!app_management_create_socket(app, &error)) {
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
    app_set_state(app, APP_STARTING);
    app_set_status(app, "Waiting for authorization / OpenVPN");
    GTask *task = g_task_new(NULL, NULL, launch_complete, app);
    g_task_set_task_data(task, input, (GDestroyNotify) launch_input_free);
    g_task_run_in_thread(task, launch_worker);
    g_object_unref(task);
}
