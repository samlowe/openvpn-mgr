#include "line_reader.h"

#include "app.h"

#include <glib.h>
#include <string.h>

static void line_reader_free(LineReader *reader)
{
    if (reader == NULL) {
        return;
    }
    g_clear_object(&reader->stream);
    g_clear_pointer(&reader->pending, g_byte_array_unref);
    g_free(reader);
}

static void line_reader_read(LineReader *reader);

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
        guint line_length = MIN(newline, LINE_READER_MAX_OUTPUT_LINE);
        gchar *line = g_strndup((const gchar *) reader->pending->data, line_length);
        app_session_process_line(reader->app, line, reader->management,
                                 reader->generation);
        g_free(line);
        g_byte_array_remove_range(reader->pending, 0, newline + 1U);
    }
    if (reader->pending->len > LINE_READER_MAX_OUTPUT_LINE) {
        g_byte_array_set_size(reader->pending, 0);
    }
    line_reader_read(reader);
}

static void line_reader_read(LineReader *reader)
{
    g_input_stream_read_async(reader->stream, reader->buffer,
                              LINE_READER_BUFFER_SIZE, G_PRIORITY_DEFAULT, NULL,
                              line_reader_read_complete, reader);
}

LineReader *line_reader_new(App *app, GInputStream *stream, gboolean management)
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
