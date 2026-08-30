#ifndef OPENVPN_MANAGER_CORE_H
#define OPENVPN_MANAGER_CORE_H

#include <gio/gio.h>
#include <glib.h>
#include <sys/types.h>

typedef struct {
    gchar *id;
    gchar *pattern;
    gchar *auth_file;
    GRegex *regex;
} CredentialRule;

typedef struct {
    gchar *path;
    gchar *display_name;
    gchar *hostname;
    gchar *credential_hostname;
    gchar *credential_id;
    gchar *auth_file;
    gchar *error;
} Profile;

typedef struct {
    gchar *profile_directory;
    GPtrArray *credential_rules;
} AppConfig;

void credential_rule_free(CredentialRule *rule);
void profile_free(Profile *profile);
void app_config_init(AppConfig *config);
void app_config_clear(AppConfig *config);
AppConfig *app_config_new(void);
void app_config_free(AppConfig *config);

gboolean app_config_load(AppConfig *config, const gchar *path, GError **error);
Profile *profile_parse(const gchar *path, const gchar *display_name);
GPtrArray *profiles_scan(const gchar *directory, const AppConfig *config,
                         GError **error);
GPtrArray *profiles_scan_cancelable(const gchar *directory, const AppConfig *config,
                                    GCancellable *cancellable, GError **error);
gboolean profile_matches(const Profile *profile, const gchar *query);
GPtrArray *profiles_filter(const GPtrArray *profiles, const gchar *query,
                           guint maximum, guint *total_matches);
GPtrArray *profiles_filter_cancelable(const GPtrArray *profiles, const gchar *query,
                                      guint maximum, GCancellable *cancellable,
                                      guint *total_matches);
gboolean profile_is_connectable(const Profile *profile);
gboolean path_is_secure_file(const gchar *path);
gboolean path_is_secure_auth_file(const gchar *path);

#endif
