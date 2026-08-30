#include "../src/core.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

static gchar *make_temp_directory(void)
{
    gchar *template = g_build_filename(g_get_tmp_dir(),
                                       "openvpn-manager-test-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(template));
    return template;
}

static void remove_temp_directory(gchar *directory)
{
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (dir != NULL) {
        const gchar *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar *path = g_build_filename(directory, name, NULL);
            g_remove(path);
            g_free(path);
        }
        g_dir_close(dir);
    }
    g_rmdir(directory);
    g_free(directory);
}

static gchar *write_fixture(const gchar *directory, const gchar *name,
                            const gchar *contents)
{
    gchar *path = g_build_filename(directory, name, NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
    return path;
}

static Profile *make_profile(const gchar *display_name, const gchar *hostname)
{
    Profile *profile = g_new0(Profile, 1);
    profile->display_name = g_strdup(display_name);
    profile->hostname = g_strdup(hostname);
    return profile;
}

static AppConfig *load_config_fixture(const gchar *directory,
                                      const gchar *contents)
{
    gchar *config_path = write_fixture(directory, "config.ini", contents);
    AppConfig *config = app_config_new();
    GError *error = NULL;
    g_assert_true(app_config_load(config, config_path, &error));
    g_assert_no_error(error);
    g_free(config_path);
    return config;
}

static void test_parse_remote(void)
{
    gchar *directory = make_temp_directory();
    gchar *path = write_fixture(directory, "uk.ovpn",
                                 "client\n# comment\nremote uk2242.nordvpn.com 1194 udp\n");
    Profile *profile = profile_parse(path, "uk.ovpn");
    g_assert_cmpstr(profile->hostname, ==, "uk2242.nordvpn.com");
    g_assert_null(profile->error);
    profile_free(profile);
    g_free(path);
    g_rmdir(directory);
    g_free(directory);
}

static void test_rejects_multiple_remote_and_scripts(void)
{
    gchar *directory = make_temp_directory();
    gchar *path = write_fixture(directory, "bad.ovpn",
                                 "remote one.example 1194 udp\n"
                                 "remote two.example 1194 udp\n");
    Profile *profile = profile_parse(path, "bad.ovpn");
    g_assert_nonnull(profile->error);
    g_assert_nonnull(g_strstr_len(profile->error, -1, "multiple"));
    profile_free(profile);
    g_free(path);

    path = write_fixture(directory, "script.ovpn",
                         "remote one.example 1194 udp\nup /tmp/not-allowed\n");
    profile = profile_parse(path, "script.ovpn");
    g_assert_nonnull(profile->error);
    g_assert_nonnull(g_strstr_len(profile->error, -1, "unsupported"));
    profile_free(profile);
    g_free(path);
    g_rmdir(directory);
    g_free(directory);
}

static void test_mapping_and_ambiguity(void)
{
    gchar *directory = make_temp_directory();
    gchar *config_path = write_fixture(
        directory, "config.ini",
        "[manager]\nprofile-directory=/etc/openvpn/ovpn_udp\n"
        "[credential.nordvpn]\n"
        "hostname-regex=(^|\\.)nordvpn\\.com$\n"
        "auth-file=/etc/openvpn/credentials/nordvpn.auth\n");
    AppConfig *config = app_config_new();
    GError *error = NULL;
    g_assert_true(app_config_load(config, config_path, &error));
    g_assert_no_error(error);
    gchar *profile_path = write_fixture(
        directory, "uk2242.nordvpn.com.udp.ovpn",
        "remote 217.146.92.231 1194 udp\n"
        "verify-x509-name CN=uk2242.nordvpn.com\n");
    GPtrArray *profiles = profiles_scan(directory, config, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(profiles->len, ==, 1);
    Profile *profile = g_ptr_array_index(profiles, 0);
    g_assert_cmpstr(profile->credential_id, ==, "nordvpn");
    g_assert_true(profile_is_connectable(profile));
    g_ptr_array_unref(profiles);
    g_free(profile_path);
    g_free(config_path);
    app_config_free(config);
    g_rmdir(directory);
    g_free(directory);
}

static void test_bounded_filter(void)
{
    GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify) profile_free);
    for (guint index = 0; index < 150U; index++) {
        Profile *profile = g_new0(Profile, 1);
        profile->display_name = g_strdup_printf("endpoint-%03u.ovpn", index);
        profile->hostname = g_strdup_printf("uk%03u.nordvpn.com", index);
        g_ptr_array_add(profiles, profile);
    }
    guint total = 0;
    GPtrArray *matches = profiles_filter(profiles, "nordvpn", 100U, &total);
    g_assert_cmpuint(total, ==, 150);
    g_assert_cmpuint(matches->len, ==, 100);
    g_ptr_array_unref(matches);
    g_ptr_array_unref(profiles);
}

static void test_parse_profile_details(void)
{
    gchar *directory = make_temp_directory();

    gchar *path = write_fixture(
        directory, "cn.ovpn",
        "remote 10.0.0.1 1194 udp\n"
        "verify-x509-name CN=vpn.example.com\n");
    Profile *profile = profile_parse(path, "cn.ovpn");
    g_assert_null(profile->error);
    g_assert_cmpstr(profile->hostname, ==, "10.0.0.1");
    g_assert_cmpstr(profile->credential_hostname, ==, "vpn.example.com");
    profile_free(profile);
    g_free(path);

    path = write_fixture(directory, "ipv6.ovpn",
                         "remote [2001:db8::1] 1194 udp\n");
    profile = profile_parse(path, "ipv6.ovpn");
    g_assert_null(profile->error);
    g_assert_cmpstr(profile->hostname, ==, "2001:db8::1");
    profile_free(profile);
    g_free(path);

    path = write_fixture(directory, "dot.ovpn",
                         "remote host.example.com. 1194 udp\n");
    profile = profile_parse(path, "dot.ovpn");
    g_assert_null(profile->error);
    g_assert_cmpstr(profile->hostname, ==, "host.example.com");
    profile_free(profile);
    g_free(path);

    path = write_fixture(directory, "missing-remote.ovpn", "client\n");
    profile = profile_parse(path, "missing-remote.ovpn");
    g_assert_nonnull(profile->error);
    g_assert_nonnull(g_strstr_len(profile->error, -1, "no remote"));
    profile_free(profile);
    g_free(path);

    remove_temp_directory(directory);
}

static void test_config_load_missing_and_defaults(void)
{
    gchar *directory = make_temp_directory();
    gchar *missing = g_build_filename(directory, "missing.ini", NULL);
    AppConfig *config = app_config_new();
    GError *error = NULL;

    g_assert_true(app_config_load(config, missing, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config->profile_directory, ==, "/etc/openvpn/ovpn_udp");
    g_assert_cmpuint(config->credential_rules->len, ==, 0U);

    app_config_free(config);
    g_free(missing);
    remove_temp_directory(directory);
}

static void test_config_load_success_and_reload(void)
{
    gchar *directory = make_temp_directory();
    const gchar *contents =
        "; ignore this comment line\n"
        "[manager]\n"
        "profile-directory=/tmp/openvpn-profiles\n"
        "[credential.work]\n"
        "hostname-regex=work\\.example$\n"
        "auth-file=/etc/openvpn/credentials/work.auth\n";

    AppConfig *config = load_config_fixture(directory, contents);
    g_assert_cmpstr(config->profile_directory, ==, "/tmp/openvpn-profiles");
    g_assert_cmpuint(config->credential_rules->len, ==, 1U);
    CredentialRule *rule = g_ptr_array_index(config->credential_rules, 0);
    g_assert_cmpstr(rule->id, ==, "work");
    g_assert_cmpstr(rule->auth_file, ==, "/etc/openvpn/credentials/work.auth");

    const gchar *replacement =
        "[credential.home]\n"
        "hostname-regex=home\\.example$\n"
        "auth-file=/etc/openvpn/credentials/home.auth\n";
    gchar *config_path = write_fixture(directory, "config.ini", replacement);
    GError *error = NULL;
    g_assert_true(app_config_load(config, config_path, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config->profile_directory, ==, "/etc/openvpn/ovpn_udp");
    g_assert_cmpuint(config->credential_rules->len, ==, 1U);
    rule = g_ptr_array_index(config->credential_rules, 0);
    g_assert_cmpstr(rule->id, ==, "home");

    app_config_free(config);
    g_free(config_path);
    remove_temp_directory(directory);
}

static void test_config_load_validation_errors(void)
{
    gchar *directory = make_temp_directory();
    AppConfig *config = app_config_new();
    GError *error = NULL;
    gchar *path = NULL;

    path = write_fixture(directory, "relative-dir.ini",
                         "[manager]\nprofile-directory=relative/path\n");
    g_assert_false(app_config_load(config, path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);

    path = write_fixture(directory, "bad-group.ini",
                         "[credential.]\n"
                         "hostname-regex=example\n"
                         "auth-file=/etc/openvpn/credentials/x.auth\n");
    g_assert_false(app_config_load(config, path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);

    path = write_fixture(directory, "missing-keys.ini",
                         "[credential.test]\nhostname-regex=example\n");
    g_assert_false(app_config_load(config, path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);

    path = write_fixture(directory, "relative-auth.ini",
                         "[credential.test]\n"
                         "hostname-regex=example\n"
                         "auth-file=credentials/test.auth\n");
    g_assert_false(app_config_load(config, path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);

    path = write_fixture(directory, "bad-regex.ini",
                         "[credential.test]\n"
                         "hostname-regex=[unclosed\n"
                         "auth-file=/etc/openvpn/credentials/test.auth\n");
    g_assert_false(app_config_load(config, path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);

    app_config_free(config);
    remove_temp_directory(directory);
}

static void test_profile_matches_and_connectable(void)
{
    Profile *profile = make_profile("uk2242.nordvpn.com.udp.ovpn",
                                    "uk2242.nordvpn.com");

    g_assert_true(profile_matches(profile, NULL));
    g_assert_true(profile_matches(profile, ""));
    g_assert_true(profile_matches(profile, "nordvpn"));
    g_assert_true(profile_matches(profile, "UK2242"));
    g_assert_true(profile_matches(profile, "2242"));
    g_assert_false(profile_matches(profile, "nomatch"));

    profile->credential_id = g_strdup("nordvpn");
    profile->auth_file = g_strdup("/etc/openvpn/credentials/nordvpn.auth");
    g_assert_true(profile_is_connectable(profile));

    profile->error = g_strdup("broken");
    g_assert_false(profile_is_connectable(profile));
    g_free(profile->error);
    profile->error = NULL;
    g_assert_true(profile_is_connectable(profile));

    g_free(profile->credential_id);
    profile->credential_id = NULL;
    g_assert_false(profile_is_connectable(profile));

    g_assert_false(profile_is_connectable(NULL));
    profile_free(profile);
}

static void test_credential_mapping_errors(void)
{
    gchar *directory = make_temp_directory();
    AppConfig *config = load_config_fixture(
        directory,
        "[credential.a]\n"
        "hostname-regex=conflict\\.example$\n"
        "auth-file=/etc/openvpn/credentials/a.auth\n"
        "[credential.b]\n"
        "hostname-regex=conflict\\.example$\n"
        "auth-file=/etc/openvpn/credentials/b.auth\n");
    write_fixture(directory, "conflict.example.ovpn",
                  "remote conflict.example 1194 udp\n");
    GError *error = NULL;
    GPtrArray *profiles = profiles_scan(directory, config, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(profiles->len, ==, 1U);
    Profile *profile = g_ptr_array_index(profiles, 0);
    g_assert_nonnull(profile->error);
    g_assert_nonnull(g_strstr_len(profile->error, -1, "multiple credential rules"));
    g_assert_false(profile_is_connectable(profile));
    g_ptr_array_unref(profiles);

    app_config_clear(config);
    const gchar *single_rule =
        "[credential.only]\n"
        "hostname-regex=allowed\\.example$\n"
        "auth-file=/etc/openvpn/credentials/only.auth\n";
    gchar *config_path = write_fixture(directory, "config.ini", single_rule);
    g_assert_true(app_config_load(config, config_path, &error));
    g_assert_no_error(error);
    write_fixture(directory, "other.example.ovpn",
                  "remote other.example 1194 udp\n");
    profiles = profiles_scan(directory, config, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(profiles->len, ==, 2U);
    for (guint index = 0; index < profiles->len; index++) {
        profile = g_ptr_array_index(profiles, index);
        if (g_str_has_suffix(profile->display_name, "other.example.ovpn")) {
            g_assert_nonnull(profile->error);
            g_assert_nonnull(
                g_strstr_len(profile->error, -1, "no credential rule matches"));
            g_assert_false(profile_is_connectable(profile));
        }
    }
    g_ptr_array_unref(profiles);

    app_config_free(config);
    g_free(config_path);
    remove_temp_directory(directory);
}

static void test_profiles_scan_sorting(void)
{
    gchar *directory = make_temp_directory();
    AppConfig *config = app_config_new();
    write_fixture(directory, "zz-last.ovpn", "remote zz.example 1194 udp\n");
    write_fixture(directory, "aa-first.ovpn", "remote aa.example 1194 udp\n");
    write_fixture(directory, "notes.txt", "not a profile\n");
    GError *error = NULL;
    GPtrArray *profiles = profiles_scan(directory, config, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(profiles->len, ==, 2U);
    Profile *first = g_ptr_array_index(profiles, 0);
    Profile *second = g_ptr_array_index(profiles, 1);
    g_assert_cmpstr(first->display_name, ==, "aa-first.ovpn");
    g_assert_cmpstr(second->display_name, ==, "zz-last.ovpn");
    g_ptr_array_unref(profiles);
    app_config_free(config);
    remove_temp_directory(directory);
}

static void test_filter_cancelled(void)
{
    GPtrArray *profiles = g_ptr_array_new_with_free_func((GDestroyNotify) profile_free);
    g_ptr_array_add(profiles, make_profile("one.ovpn", "one.example"));
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    guint total = 0;
    GPtrArray *matches = profiles_filter_cancelable(
        profiles, "one", 10U, cancellable, &total);
    g_assert_null(matches);
    g_ptr_array_unref(profiles);
    g_object_unref(cancellable);
}

static void test_path_security_checks(void)
{
    g_assert_false(path_is_secure_file(NULL));
    g_assert_false(path_is_secure_file("relative/path"));
    g_assert_false(path_is_secure_file("/tmp/not-under-openvpn"));

    gchar *directory = make_temp_directory();
    gchar *path = write_fixture(directory, "auth", "user\npassword\n");
    g_assert_false(path_is_secure_file(path));
    g_assert_false(path_is_secure_auth_file(path));
    g_free(path);
    remove_temp_directory(directory);
}

static void test_unsafe_file(void)
{
    gchar *directory = make_temp_directory();
    gchar *path = write_fixture(directory, "auth", "user\npassword\n");
    g_assert_false(path_is_secure_file(path));
    g_free(path);
    remove_temp_directory(directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/core/parse-remote", test_parse_remote);
    g_test_add_func("/core/parse-profile-details", test_parse_profile_details);
    g_test_add_func("/core/reject-unsafe-profile", test_rejects_multiple_remote_and_scripts);
    g_test_add_func("/core/config-load-missing", test_config_load_missing_and_defaults);
    g_test_add_func("/core/config-load-success", test_config_load_success_and_reload);
    g_test_add_func("/core/config-load-validation", test_config_load_validation_errors);
    g_test_add_func("/core/profile-matches", test_profile_matches_and_connectable);
    g_test_add_func("/core/mapping", test_mapping_and_ambiguity);
    g_test_add_func("/core/mapping-errors", test_credential_mapping_errors);
    g_test_add_func("/core/scan-sorting", test_profiles_scan_sorting);
    g_test_add_func("/core/bounded-filter", test_bounded_filter);
    g_test_add_func("/core/filter-cancelled", test_filter_cancelled);
    g_test_add_func("/core/secure-file", test_unsafe_file);
    g_test_add_func("/core/path-security", test_path_security_checks);
    return g_test_run();
}
