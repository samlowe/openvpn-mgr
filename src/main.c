#include "app.h"

#include <gtk/gtk.h>

int main(int argc, char **argv)
{
    (void) argv;
    App app = {0};
    app_config_init(&app.config);
    GtkApplication *application = gtk_application_new(
        "com.example.OpenVPNManager", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(app_ui_activate), &app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    app_cleanup(&app);
    g_object_unref(application);
    return status;
}
