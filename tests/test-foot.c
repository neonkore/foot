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
populate_config(struct file *file, const char *config)
{
    const size_t len = strlen(config);
    return write(file->fd, config, len) == len;
}

START_TEST(config_invalid_path)
{
    bool success = config_load(
        &conf, "/invalid-path", &user_notifications, &overrides, true);
    ck_assert(!success);
}

START_TEST(config_empty_config)
{
    bool success = config_load(
        &conf, conf_file.path, &user_notifications, &overrides, true);
    ck_assert(success);
}

START_TEST(config_invalid_section)
{
    static const char *config = "[invalid-section]\n";
    ck_assert(populate_config(&conf_file, config));

    bool success = config_load(
        &conf, conf_file.path, &user_notifications, &overrides, true);

    ck_assert(!success);
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
