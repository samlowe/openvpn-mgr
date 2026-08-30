#include "app.h"

#include <errno.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "line_reader.h"

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

static void stream_close_complete(GObject *source_object, GAsyncResult *result,
                                  gpointer user_data)
{
    (void) user_data;
    GError *error = NULL;
    g_io_stream_close_finish(G_IO_STREAM(source_object), result, &error);
    g_clear_error(&error);
}

static void close_stream_async(GIOStream *stream)
{
    g_io_stream_close_async(stream, G_PRIORITY_DEFAULT, NULL,
                            stream_close_complete, NULL);
}

void app_management_connection_close(App *app)
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

void app_management_remove_socket(App *app)
{
    app_management_connection_close(app);
    if (app->management_socket != NULL) {
        g_remove(app->management_socket);
        g_clear_pointer(&app->management_socket, g_free);
    }
    if (app->management_directory != NULL) {
        g_rmdir(app->management_directory);
        g_clear_pointer(&app->management_directory, g_free);
    }
}

gboolean app_management_create_socket(App *app, GError **error)
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
        app_management_remove_socket(app);
        return FALSE;
    }
    if (chmod(app->management_socket, 0600) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot secure management socket: %s", g_strerror(errno));
        app_management_remove_socket(app);
        return FALSE;
    }
    g_signal_connect(app->management_service, "incoming",
                     G_CALLBACK(management_incoming), app);
    g_socket_service_start(app->management_service);
    return TRUE;
}
