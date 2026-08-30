#ifndef OPENVPN_MANAGER_CORE_H
#define OPENVPN_MANAGER_CORE_H

#include <gio/gio.h>
#include <glib.h>
#include <sys/types.h>

/**
 * CredentialRule:
 * @id: credential identifier from the config group name
 * @pattern: hostname regular expression to match
 * @auth_file: absolute path to the username/password file
 * @regex: compiled form of @pattern
 *
 * Maps VPN endpoint hostnames to an auth credentials file.
 */
typedef struct {
    gchar *id;
    gchar *pattern;
    gchar *auth_file;
    GRegex *regex;
} CredentialRule;

/**
 * Profile:
 * @path: absolute path to the .ovpn file
 * @display_name: filename shown in the UI
 * @hostname: host from the first `remote` directive
 * @credential_hostname: CN from `verify-x509-name`, if present
 * @credential_id: matched credential rule id, when mapping succeeds
 * @auth_file: auth file from the matched credential rule
 * @error: parse or mapping error message, or %NULL when valid
 *
 * Parsed OpenVPN profile with optional credential mapping applied.
 */
typedef struct {
    gchar *path;
    gchar *display_name;
    gchar *hostname;
    gchar *credential_hostname;
    gchar *credential_id;
    gchar *auth_file;
    gchar *error;
} Profile;

/**
 * AppConfig:
 * @profile_directory: directory scanned for .ovpn files
 * @credential_rules: array of owned #CredentialRule values
 *
 * Application configuration loaded from the manager config file.
 */
typedef struct {
    gchar *profile_directory;
    GPtrArray *credential_rules;
} AppConfig;

/**
 * credential_rule_free:
 * @rule: (nullable): a #CredentialRule
 *
 * Frees @rule and all fields it owns.
 */
void credential_rule_free(CredentialRule *rule);

/**
 * profile_free:
 * @profile: (nullable): a #Profile
 *
 * Frees @profile and all fields it owns.
 */
void profile_free(Profile *profile);

/**
 * app_config_init:
 * @config: an uninitialized #AppConfig
 *
 * Initializes @config with default values. The caller must eventually call
 * app_config_clear() or app_config_free().
 */
void app_config_init(AppConfig *config);

/**
 * app_config_clear:
 * @config: (nullable): a #AppConfig
 *
 * Releases resources owned by @config without freeing @config itself.
 */
void app_config_clear(AppConfig *config);

/**
 * app_config_new:
 *
 * Creates a new #AppConfig initialized with defaults.
 *
 * Returns: (transfer full): a new #AppConfig
 */
AppConfig *app_config_new(void);

/**
 * app_config_free:
 * @config: (nullable): a #AppConfig
 *
 * Frees @config and all resources it owns.
 */
void app_config_free(AppConfig *config);

/**
 * app_config_load:
 * @config: a #AppConfig
 * @path: absolute path to the manager config file
 * @error: return location for a #GError
 *
 * Loads manager settings and credential rules from @path. A missing file is
 * treated as success and leaves defaults in place.
 *
 * Returns: %TRUE on success, or %FALSE on error
 */
gboolean app_config_load(AppConfig *config, const gchar *path, GError **error);

/**
 * profile_parse:
 * @path: absolute path to a .ovpn file
 * @display_name: filename to store on the returned profile
 *
 * Parses @path for supported OpenVPN directives. The returned profile may
 * contain an @error message instead of a hostname when validation fails.
 *
 * Returns: (transfer full): a new #Profile
 */
Profile *profile_parse(const gchar *path, const gchar *display_name);

/**
 * profiles_scan:
 * @directory: directory to scan for .ovpn files
 * @config: credential mapping configuration
 * @error: return location for a #GError
 *
 * Scans @directory and returns parsed, credential-mapped profiles sorted by
 * display name.
 *
 * Returns: (element-type Profile) (transfer full): sorted profile array, or
 *   %NULL on error
 */
GPtrArray *profiles_scan(const gchar *directory, const AppConfig *config,
                         GError **error);

/**
 * profiles_scan_cancelable:
 * @directory: directory to scan for .ovpn files
 * @config: credential mapping configuration
 * @cancellable: optional cancellation object
 * @error: return location for a #GError
 *
 * Like profiles_scan(), but stops early when @cancellable is cancelled.
 *
 * Returns: (element-type Profile) (transfer full): sorted profile array, or
 *   %NULL on error or cancellation
 */
GPtrArray *profiles_scan_cancelable(const gchar *directory, const AppConfig *config,
                                    GCancellable *cancellable, GError **error);

/**
 * profile_matches:
 * @profile: a #Profile
 * @query: case-insensitive search string
 *
 * Returns: %TRUE when @query is empty or matches the display name or hostname
 */
gboolean profile_matches(const Profile *profile, const gchar *query);

/**
 * profiles_filter:
 * @profiles: array of #Profile values
 * @query: case-insensitive search string
 * @maximum: maximum number of matches to return
 * @total_matches: (out) (optional): total matches before truncation
 *
 * Returns profiles from @profiles that match @query, up to @maximum entries.
 *
 * Returns: (element-type Profile) (transfer full): matching profiles without
 *   transferring ownership of the source array elements
 */
GPtrArray *profiles_filter(const GPtrArray *profiles, const gchar *query,
                           guint maximum, guint *total_matches);

/**
 * profiles_filter_cancelable:
 * @profiles: array of #Profile values
 * @query: case-insensitive search string
 * @maximum: maximum number of matches to return
 * @cancellable: optional cancellation object
 * @total_matches: (out) (optional): total matches before truncation
 *
 * Like profiles_filter(), but returns %NULL when @cancellable is cancelled.
 *
 * Returns: (element-type Profile) (transfer full): matching profiles, or %NULL
 *   on cancellation
 */
GPtrArray *profiles_filter_cancelable(const GPtrArray *profiles, const gchar *query,
                                      guint maximum, GCancellable *cancellable,
                                      guint *total_matches);

/**
 * profile_is_connectable:
 * @profile: (nullable): a #Profile
 *
 * Returns: %TRUE when @profile parsed successfully and has credential mapping
 */
gboolean profile_is_connectable(const Profile *profile);

/**
 * path_is_secure_file:
 * @path: absolute filesystem path
 *
 * Checks that @path is a root-owned regular file under /etc/openvpn/ with
 * secure parent directories.
 *
 * Returns: %TRUE when @path meets the security policy
 */
gboolean path_is_secure_file(const gchar *path);

/**
 * path_is_secure_auth_file:
 * @path: absolute filesystem path
 *
 * Returns: %TRUE when @path passes path_is_secure_file() and has mode 0600
 */
gboolean path_is_secure_auth_file(const gchar *path);

#endif
