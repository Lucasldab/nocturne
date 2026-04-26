#include "runner.h"

#include <stdio.h>

int g_test_passed = 0;
int g_test_failed = 0;

void test_assert(const char *suite, const char *msg, int cond)
{
    if (cond) {
        g_test_passed++;
        printf("[PASS] %s: %s\n", suite, msg);
    } else {
        g_test_failed++;
        fprintf(stderr, "[FAIL] %s: %s\n", suite, msg);
    }
}

int test_finish(const char *suite)
{
    fprintf(stdout, "---- %s: passed=%d failed=%d ----\n",
            suite, g_test_passed, g_test_failed);
    return g_test_failed == 0 ? 0 : 1;
}
