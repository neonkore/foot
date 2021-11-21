#include <check.h>
#include "../config.h"
#include "../user-notification.h"

static struct config conf = {0};
static user_notifications_t user_notifications = tll_init();

static void
conf_setup(void)
{
    memset(&conf, 0, sizeof(conf));
}

static void
conf_teardown(void)
{
    config_free(conf);
}

static void
user_notifications_setup(void)
{
    ck_assert_int_eq(tll_length(user_notifications), 0);
}

static void
user_notifications_teardown(void)
{
    user_notifications_free(&user_notifications);
}

START_TEST(config_invalid_path)
{
    bool success = config_load(
        &conf, "/invalid-path", &user_notifications, NULL, true);
    ck_assert(!success);
}

static Suite *
foot_suite(void)
{
    Suite *suite = suite_create("foot");
    TCase *config = tcase_create("config");
    tcase_add_checked_fixture(config, &conf_setup, &conf_teardown);
    tcase_add_checked_fixture(
        config, &user_notifications_setup, &user_notifications_teardown);
    tcase_add_test(config, config_invalid_path);
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
