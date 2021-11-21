#include <stdlib.h>
#include <unistd.h>

#include <check.h>

#include "../config.h"
#include "../user-notification.h"

struct file {
    char *path;
    int fd;
};

static struct config conf = {0};
static struct file conf_file;
static user_notifications_t user_notifications = tll_init();
static config_override_t overrides = tll_init();

static void
conf_setup(void)
{
    memset(&conf, 0, sizeof(conf));
}

static void
conf_teardown(void)
{
    config_free(conf);
    user_notifications_free(&user_notifications);
    tll_free(overrides);
}

static void
conf_file_setup(void)
{
    static char template[] = "/tmp/test-foot-config-file-XXXXXX";
    int fd = mkstemp(template);

    conf_file.path = NULL;
    conf_file.fd = -1;

    ck_assert_int_ge(fd, 0);

    conf_file.path = template;
    conf_file.fd = fd;
}

static void
conf_file_teardown(void)
{
    if (conf_file.fd >= 0)
        ck_assert_int_eq(close(conf_file.fd), 0);
    if (conf_file.path != NULL)
        ck_assert_int_eq(unlink(conf_file.path), 0);
}

static bool
write_string(const char *config)
{
    const size_t len = strlen(config);
    return write(conf_file.fd, config, len) == len;
}

START_TEST(config_invalid_path)
{
    ck_assert(
        !config_load(
            &conf, "/invalid-path", &user_notifications, &overrides, true));
}

START_TEST(config_empty_config)
{
    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
}

START_TEST(config_invalid_section)
{
    static const char *config = "[invalid-section]\n";
    ck_assert(write_string(config));

    ck_assert(
        !config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
}

START_TEST(config_main_empty)
{
    static const char *config = "[main]\n";
    ck_assert(write_string(config));

    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
}

static bool
add_string_option(const char *section, const char *option, const char *value)
{
    return write_string("[") &&
        write_string(section) &&
        write_string("]\n") &&
        write_string(option) &&
        write_string("=") &&
        write_string(value) &&
        write_string("\n");
}

static void
test_string_option(const char *section, const char *option, const char **ptr)
{
    ck_assert(add_string_option(section, option, "a generic string"));
    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
    ck_assert_str_eq(*ptr, "a generic string");
}

static void
test_bool_option(const char *section, const char *option, const bool *ptr)
{
    ck_assert(add_string_option(section, option, "on"));
    ck_assert(add_string_option(section, option, "true"));
    ck_assert(add_string_option(section, option, "yes"));
    ck_assert(add_string_option(section, option, "1"));

    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
    ck_assert(*ptr);

    config_free(conf);
    memset(&conf, 0, sizeof(conf));

    ck_assert(add_string_option(section, option, "off"));
    ck_assert(add_string_option(section, option, "false"));
    ck_assert(add_string_option(section, option, "no"));
    ck_assert(add_string_option(section, option, "0"));

    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
    ck_assert(!*ptr);

    config_free(conf);
    memset(&conf, 0, sizeof(conf));

    ck_assert(add_string_option(section, option, "not-a-boolean"));
    ck_assert(
        !config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
}

static void
test_pt_or_px_option(const char *section, const char *option,
                     const struct pt_or_px *ptr)
{
    ck_assert(add_string_option(section, option, "13"));
    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
    ck_assert_int_eq(ptr->pt, 13);
    ck_assert_int_eq(ptr->px, 0);

    config_free(conf);
    memset(&conf, 0, sizeof(conf));

    ck_assert(add_string_option(section, option, "37px"));
    ck_assert(
        config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
    ck_assert_int_eq(ptr->pt, 0);
    ck_assert_int_eq(ptr->px, 37);

    config_free(conf);
    memset(&conf, 0, sizeof(conf));

    ck_assert(add_string_option(section, option, "not-a-pt-or-px"));
    ck_assert(
        !config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
}

START_TEST(config_main_shell)
{
    test_string_option("main", "shell", (const char **)&conf.shell);
}

START_TEST(config_main_login_shell)
{
    test_bool_option("main", "login-shell", &conf.login_shell);
}

START_TEST(config_main_line_height)
{
    test_pt_or_px_option("main", "line-height", &conf.line_height);
}

START_TEST(config_main_invalid_option)
{
    static const char *config = "foo=bar\n";
    ck_assert(write_string(config));

    ck_assert(
        !config_load(
            &conf, conf_file.path, &user_notifications, &overrides, true));
}

static Suite *
foot_suite(void)
{
    Suite *suite = suite_create("foot");
    TCase *config = tcase_create("config");
    tcase_add_checked_fixture(config, &conf_setup, &conf_teardown);
    tcase_add_checked_fixture(config, &conf_file_setup, &conf_file_teardown);
    tcase_add_test(config, config_invalid_path);
    tcase_add_test(config, config_empty_config);
    tcase_add_test(config, config_invalid_section);
    tcase_add_test(config, config_main_empty);
    tcase_add_test(config, config_main_shell);
    tcase_add_test(config, config_main_login_shell);
    // TODO: main.font{,-bold,-italic,-bold-italic}
    // TODO: main.include
    tcase_add_test(config, config_main_line_height);
    tcase_add_test(config, config_main_invalid_option);
    suite_add_tcase(suite, config);
    return suite;
}

int
main(int argc, const char *const *argv)
{
    Suite *suite = foot_suite();
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed;
}
