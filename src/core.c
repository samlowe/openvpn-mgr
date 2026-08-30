#define _XOPEN_SOURCE 700

#include "core.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>

#define DEFAULT_PROFILE_DIRECTORY "/etc/openvpn/ovpn_udp"
#define MAX_PROFILE_BYTES (1024U * 1024U)
#define MAX_PROFILE_LINE_BYTES 8192U
#define MAX_PROFILE_COUNT 50000U
#define MAX_REGEX_BYTES 1024U

void credential_rule_free(CredentialRule *rule)
{
    if (rule == NULL) {
        return;
    }
    g_free(rule->id);
    g_free(rule->pattern);
    g_free(rule->auth_file);
    g_clear_pointer(&rule->regex, g_regex_unref);
    g_free(rule);
}

void profile_free(Profile *profile)
{
    if (profile == NULL) {
        return;
    }
    g_free(profile->path);
    g_free(profile->display_name);
    g_free(profile->hostname);
    g_free(profile->credential_hostname);
    g_free(profile->credential_id);
    g_free(profile->auth_file);
    g_free(profile->error);
    g_free(profile);
}

void app_config_init(AppConfig *config)
{
    g_return_if_fail(config != NULL);
    config->profile_directory = g_strdup(DEFAULT_PROFILE_DIRECTORY);
    config->credential_rules = g_ptr_array_new_with_free_func(
        (GDestroyNotify) credential_rule_free);
}

void app_config_clear(AppConfig *config)
{
    if (config == NULL) {
        return;
    }
    g_clear_pointer(&config->profile_directory, g_free);
    g_clear_pointer(&config->credential_rules, g_ptr_array_unref);
}

AppConfig *app_config_new(void)
{
    AppConfig *config = g_new0(AppConfig, 1);
    app_config_init(config);
    return config;
}

void app_config_free(AppConfig *config)
{
    if (config == NULL) {
        return;
    }
    app_config_clear(config);
    g_free(config);
}

static gboolean get_required_key(GKeyFile *key_file, const gchar *group,
                                 const gchar *key, gchar **value, GError **error)
{
    GError *local_error = NULL;
    gchar *loaded = g_key_file_get_value(key_file, group, key, &local_error);
    if (loaded == NULL) {
        g_propagate_error(error, local_error);
        return FALSE;
    }
    if (*loaded == '\0') {
        g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                    "%s/%s is empty", group, key);
        g_free(loaded);
        return FALSE;
    }
    *value = loaded;
    return TRUE;
}

gboolean app_config_load(AppConfig *config, const gchar *path, GError **error)
{
    g_return_val_if_fail(config != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    GKeyFile *key_file = g_key_file_new();
    GError *local_error = NULL;
    gchar *file_data = NULL;
    gsize file_length = 0;
    if (!g_file_get_contents(path, &file_data, &file_length, &local_error)) {
        if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(&local_error);
            g_key_file_unref(key_file);
            return TRUE;
        }
        g_propagate_error(error, local_error);
        g_key_file_unref(key_file);
        return FALSE;
    }
    GString *key_file_data = g_string_sized_new(file_length);
    gchar **config_lines = g_strsplit(file_data, "\n", -1);
    for (gsize line_index = 0; config_lines[line_index] != NULL; line_index++) {
        gchar *line = g_strdup(config_lines[line_index]);
        gchar *trimmed = g_strstrip(line);
        if (*trimmed != ';') {
            g_string_append(key_file_data, config_lines[line_index]);
            g_string_append_c(key_file_data, '\n');
        }
        g_free(line);
    }
    g_strfreev(config_lines);
    g_free(file_data);
    if (!g_key_file_load_from_data(key_file, key_file_data->str,
                                   key_file_data->len, G_KEY_FILE_NONE,
                                   &local_error)) {
        g_propagate_error(error, local_error);
        g_string_free(key_file_data, TRUE);
        g_key_file_unref(key_file);
        return FALSE;
    }
    g_string_free(key_file_data, TRUE);

    gchar *profile_directory = NULL;
    if (g_key_file_has_key(key_file, "manager", "profile-directory", NULL)) {
        if (!get_required_key(key_file, "manager", "profile-directory",
                              &profile_directory, error)) {
            g_key_file_unref(key_file);
            return FALSE;
        }
        if (!g_path_is_absolute(profile_directory)) {
            g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                        "profile-directory must be absolute");
            g_free(profile_directory);
            g_key_file_unref(key_file);
            return FALSE;
        }
    }

    gsize group_count = 0;
    gchar **groups = g_key_file_get_groups(key_file, &group_count);
    GPtrArray *rules = g_ptr_array_new_with_free_func(
        (GDestroyNotify) credential_rule_free);
    for (gsize index = 0; index < group_count; index++) {
        const gchar *group = groups[index];
        if (!g_str_has_prefix(group, "credential.")) {
            continue;
        }
        const gchar *id = group + strlen("credential.");
        if (*id == '\0' || strlen(id) > 64U) {
            g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                        "invalid credential group name");
            g_ptr_array_unref(rules);
            g_strfreev(groups);
            g_free(profile_directory);
            g_key_file_unref(key_file);
            return FALSE;
        }

        CredentialRule *rule = g_new0(CredentialRule, 1);
        rule->id = g_strdup(id);
        if (!get_required_key(key_file, group, "hostname-regex",
                              &rule->pattern, error) ||
            !get_required_key(key_file, group, "auth-file",
                              &rule->auth_file, error)) {
            credential_rule_free(rule);
            g_ptr_array_unref(rules);
            g_strfreev(groups);
            g_free(profile_directory);
            g_key_file_unref(key_file);
            return FALSE;
        }
        if (strlen(rule->pattern) > MAX_REGEX_BYTES ||
            !g_path_is_absolute(rule->auth_file)) {
            g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                        "invalid credential rule %s", rule->id);
            credential_rule_free(rule);
            g_ptr_array_unref(rules);
            g_strfreev(groups);
            g_free(profile_directory);
            g_key_file_unref(key_file);
            return FALSE;
        }
        rule->regex = g_regex_new(rule->pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE,
                                  0, &local_error);
        if (rule->regex == NULL) {
            g_propagate_error(error, local_error);
            credential_rule_free(rule);
            g_ptr_array_unref(rules);
            g_strfreev(groups);
            g_free(profile_directory);
            g_key_file_unref(key_file);
            return FALSE;
        }
        g_ptr_array_add(rules, rule);
    }

    g_free(config->profile_directory);
    config->profile_directory = profile_directory != NULL
        ? profile_directory : g_strdup(DEFAULT_PROFILE_DIRECTORY);
    g_clear_pointer(&config->credential_rules, g_ptr_array_unref);
    config->credential_rules = rules;

    g_strfreev(groups);
    g_key_file_unref(key_file);
    return TRUE;
}

static gchar *trimmed_line(const gchar *line)
{
    gchar *copy = g_strdup(line);
    g_strstrip(copy);
    if (*copy == '#' || *copy == ';' || *copy == '\0') {
        g_clear_pointer(&copy, g_free);
        return NULL;
    }
    gchar *comment = strpbrk(copy, "#;");
    if (comment != NULL && (comment == copy || g_ascii_isspace((guchar) comment[-1]))) {
        *comment = '\0';
        g_strstrip(copy);
    }
    if (*copy == '\0') {
        g_free(copy);
        return NULL;
    }
    return copy;
}

static gboolean is_rejected_directive(const gchar *directive)
{
    static const gchar *const rejected[] = {
        "daemon", "management", "plugin", "config", "up", "down",
        "route-up", "route-pre-down", "tls-verify", "learn-address",
        "client-connect", "client-disconnect", "ipchange", "writepid",
        "log", "log-append", "status", "status-version", "chroot", "cd",
        "user", "group", "tmp-dir", "setenv", "script-security", "askpass",
        NULL
    };
    for (gsize index = 0; rejected[index] != NULL; index++) {
        if (g_strcmp0(directive, rejected[index]) == 0 ||
            (g_str_has_prefix(directive, "management") &&
             g_strcmp0(rejected[index], "management") == 0)) {
            return TRUE;
        }
    }
    return FALSE;
}

static gchar *parse_remote_hostname(gchar **tokens, gsize token_count)
{
    if (token_count < 2U || token_count > 4U) {
        return NULL;
    }
    const gchar *raw = tokens[1];
    if (*raw == '[') {
        const gchar *closing = strchr(raw, ']');
        if (closing == NULL || closing == raw + 1) {
            return NULL;
        }
        return g_strndup(raw + 1, (gsize) (closing - raw - 1));
    }
    if (strchr(raw, ':') != NULL) {
        return NULL;
    }
    gchar *hostname = g_strdup(raw);
    g_strstrip(hostname);
    gsize length = strlen(hostname);
    if (length > 0U && hostname[length - 1U] == '.') {
        hostname[length - 1U] = '\0';
    }
    if (*hostname == '\0' || g_hostname_is_non_ascii(hostname)) {
        g_free(hostname);
        return NULL;
    }
    return hostname;
}

static void set_profile_error(Profile *profile, const gchar *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    g_free(profile->error);
    profile->error = g_strdup_vprintf(format, arguments);
    va_end(arguments);
}

Profile *profile_parse(const gchar *path, const gchar *display_name)
{
    Profile *profile = g_new0(Profile, 1);
    profile->path = g_strdup(path);
    profile->display_name = g_strdup(display_name);

    struct stat metadata;
    if (stat(path, &metadata) != 0) {
        set_profile_error(profile, "cannot inspect profile: %s", g_strerror(errno));
        return profile;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size > (off_t) MAX_PROFILE_BYTES) {
        set_profile_error(profile, "profile is not a regular file or is too large");
        return profile;
    }

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &error)) {
        set_profile_error(profile, "cannot read profile: %s", error->message);
        g_clear_error(&error);
        return profile;
    }

    gchar **lines = g_strsplit(contents, "\n", -1);
    guint remote_count = 0;
    for (gsize index = 0; lines[index] != NULL; index++) {
        if (strlen(lines[index]) > MAX_PROFILE_LINE_BYTES) {
            set_profile_error(profile, "profile contains an overlong line");
            break;
        }
        gchar *line = trimmed_line(lines[index]);
        if (line == NULL) {
            continue;
        }
        gchar **tokens = g_strsplit_set(line, " \t\r", -1);
        GPtrArray *compact = g_ptr_array_new_with_free_func(g_free);
        for (gsize token_index = 0; tokens[token_index] != NULL; token_index++) {
            if (*tokens[token_index] != '\0') {
                g_ptr_array_add(compact, g_strdup(tokens[token_index]));
            }
        }
        g_ptr_array_add(compact, NULL);
        if (compact->len > 1U) {
            gchar **values = (gchar **) compact->pdata;
            gchar *directive = g_ascii_strdown(values[0], -1);
            if (is_rejected_directive(directive)) {
                set_profile_error(profile, "unsupported directive: %s", directive);
            } else if (g_strcmp0(directive, "remote") == 0) {
                remote_count++;
                if (remote_count == 1U) {
                    profile->hostname = parse_remote_hostname(values, compact->len - 1U);
                    if (profile->hostname == NULL) {
                        set_profile_error(profile, "invalid remote directive");
                    }
                }
            } else if (g_strcmp0(directive, "verify-x509-name") == 0 &&
                       compact->len > 2U && g_str_has_prefix(values[1], "CN=")) {
                profile->credential_hostname = g_strdup(values[1] + 3);
            }
            g_free(directive);
        }
        g_ptr_array_unref(compact);
        g_strfreev(tokens);
        g_free(line);
        if (profile->error != NULL) {
            break;
        }
    }
    if (profile->error == NULL && remote_count == 0U) {
        set_profile_error(profile, "profile has no remote directive");
    } else if (profile->error == NULL && remote_count > 1U) {
        set_profile_error(profile, "profile has multiple remote directives");
    }

    g_strfreev(lines);
    g_free(contents);
    (void) length;
    return profile;
}

static void apply_credential_mapping(Profile *profile, const AppConfig *config)
{
    if (profile->error != NULL || profile->hostname == NULL) {
        return;
    }
    const gchar *candidates[] = {
        profile->hostname, profile->credential_hostname, profile->display_name, NULL
    };
    const CredentialRule *match = NULL;
    for (guint index = 0; config->credential_rules->len > index; index++) {
        const CredentialRule *rule = g_ptr_array_index(config->credential_rules, index);
        gboolean matched = FALSE;
        for (gsize candidate_index = 0; candidates[candidate_index] != NULL;
             candidate_index++) {
            if (g_regex_match(rule->regex, candidates[candidate_index], 0, NULL)) {
                matched = TRUE;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (match != NULL) {
            set_profile_error(profile, "endpoint matches multiple credential rules");
            return;
        }
        match = rule;
    }
    if (match == NULL) {
        set_profile_error(profile, "no credential rule matches this endpoint");
        return;
    }
    profile->credential_id = g_strdup(match->id);
    profile->auth_file = g_strdup(match->auth_file);
}

static gint profile_compare(gconstpointer first, gconstpointer second)
{
    const Profile *left = *(Profile *const *) first;
    const Profile *right = *(Profile *const *) second;
    return g_ascii_strcasecmp(left->display_name, right->display_name);
}

GPtrArray *profiles_scan_cancelable(const gchar *directory, const AppConfig *config,
                                    GCancellable *cancellable, GError **error)
{
    GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify) profile_free);
    GDir *dir = g_dir_open(directory, 0, error);
    if (dir == NULL) {
        g_ptr_array_unref(profiles);
        return NULL;
    }

    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
            g_dir_close(dir);
            g_ptr_array_unref(profiles);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "profile scan cancelled");
            return NULL;
        }
        if (profiles->len >= MAX_PROFILE_COUNT) {
            break;
        }
        if (!g_str_has_suffix(name, ".ovpn")) {
            continue;
        }
        gchar *path = g_build_filename(directory, name, NULL);
        struct stat metadata;
        gboolean regular = g_lstat(path, &metadata) == 0 && S_ISREG(metadata.st_mode);
        if (regular) {
            gchar *display_name = g_filename_display_name(name);
            Profile *profile = profile_parse(path, display_name);
            g_free(display_name);
            apply_credential_mapping(profile, config);
            g_ptr_array_add(profiles, profile);
        }
        g_free(path);
    }
    g_dir_close(dir);
    g_ptr_array_sort(profiles, profile_compare);
    return profiles;
}

GPtrArray *profiles_scan(const gchar *directory, const AppConfig *config, GError **error)
{
    return profiles_scan_cancelable(directory, config, NULL, error);
}

gboolean profile_matches(const Profile *profile, const gchar *query)
{
    if (query == NULL || *query == '\0') {
        return TRUE;
    }
    gchar *folded_query = g_utf8_casefold(query, -1);
    gchar *folded_name = g_utf8_casefold(profile->display_name, -1);
    gchar *folded_host = g_utf8_casefold(profile->hostname != NULL ? profile->hostname : "", -1);
    gboolean result = g_strstr_len(folded_name, -1, folded_query) != NULL ||
        g_strstr_len(folded_host, -1, folded_query) != NULL;
    g_free(folded_query);
    g_free(folded_name);
    g_free(folded_host);
    return result;
}

static GPtrArray *profiles_filter_internal(const GPtrArray *profiles,
                                            const gchar *query, guint maximum,
                                            GCancellable *cancellable,
                                            guint *total_matches)
{
    GPtrArray *matches = g_ptr_array_new();
    guint total = 0;
    for (guint index = 0; index < profiles->len; index++) {
        if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
            g_ptr_array_unref(matches);
            return NULL;
        }
        Profile *profile = g_ptr_array_index((GPtrArray *) profiles, index);
        if (!profile_matches(profile, query)) {
            continue;
        }
        total++;
        if (matches->len < maximum) {
            g_ptr_array_add(matches, profile);
        }
    }
    if (total_matches != NULL) {
        *total_matches = total;
    }
    return matches;
}

GPtrArray *profiles_filter(const GPtrArray *profiles, const gchar *query,
                           guint maximum, guint *total_matches)
{
    return profiles_filter_internal(profiles, query, maximum, NULL, total_matches);
}

GPtrArray *profiles_filter_cancelable(const GPtrArray *profiles, const gchar *query,
                                      guint maximum, GCancellable *cancellable,
                                      guint *total_matches)
{
    return profiles_filter_internal(profiles, query, maximum, cancellable,
                                    total_matches);
}

gboolean profile_is_connectable(const Profile *profile)
{
    return profile != NULL && profile->error == NULL &&
        profile->credential_id != NULL && profile->auth_file != NULL;
}

static gboolean path_component_secure(const gchar *path)
{
    struct stat metadata;
    if (g_lstat(path, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        metadata.st_uid != 0 || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return FALSE;
    }
    return TRUE;
}

gboolean path_is_secure_file(const gchar *path)
{
    if (path == NULL || !g_path_is_absolute(path) ||
        !g_str_has_prefix(path, "/etc/openvpn/")) {
        return FALSE;
    }
    struct stat metadata;
    if (g_lstat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != 0 || (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return FALSE;
    }

    gchar *parent = g_path_get_dirname(path);
    while (parent != NULL && g_strcmp0(parent, "/") != 0) {
        if (!path_component_secure(parent)) {
            g_free(parent);
            return FALSE;
        }
        gchar *next = g_path_get_dirname(parent);
        g_free(parent);
        parent = next;
    }
    gboolean root_secure = parent != NULL && path_component_secure("/");
    g_free(parent);
    return root_secure;
}

gboolean path_is_secure_auth_file(const gchar *path)
{
    if (!path_is_secure_file(path)) {
        return FALSE;
    }
    struct stat metadata;
    return g_lstat(path, &metadata) == 0 && (metadata.st_mode & 0777) == 0600;
}
