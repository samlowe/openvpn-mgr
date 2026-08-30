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

static gchar *write_fixture(const gchar *directory, const gchar *name,
                            const gchar *contents)
{
    gchar *path = g_build_filename(directory, name, NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
    return path;
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

static void test_unsafe_file(void)
{
    gchar *directory = make_temp_directory();
    gchar *path = write_fixture(directory, "auth", "user\npassword\n");
    g_assert_false(path_is_secure_file(path));
    g_free(path);
    g_rmdir(directory);
    g_free(directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/core/parse-remote", test_parse_remote);
    g_test_add_func("/core/reject-unsafe-profile", test_rejects_multiple_remote_and_scripts);
    g_test_add_func("/core/mapping", test_mapping_and_ambiguity);
    g_test_add_func("/core/bounded-filter", test_bounded_filter);
    g_test_add_func("/core/secure-file", test_unsafe_file);
    return g_test_run();
}
