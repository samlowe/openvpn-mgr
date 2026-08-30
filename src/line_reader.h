#ifndef OPENVPN_MANAGER_LINE_READER_H
#define OPENVPN_MANAGER_LINE_READER_H

#include <gio/gio.h>
#include <glib.h>

typedef struct App App;

#define LINE_READER_BUFFER_SIZE 4096U
#define LINE_READER_MAX_OUTPUT_LINE 8192U

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
typedef struct LineReader {
    GInputStream *stream;
    GByteArray *pending;
    App *app;
    gboolean management;
    guint generation;
    guint8 buffer[LINE_READER_BUFFER_SIZE];
} LineReader;

LineReader *line_reader_new(App *app, GInputStream *stream, gboolean management);

#endif
