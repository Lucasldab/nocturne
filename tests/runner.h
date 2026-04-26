#ifndef NOCTURNE_TAGCHECK_TEST_RUNNER_H
#define NOCTURNE_TAGCHECK_TEST_RUNNER_H

#include <stdio.h>

extern int g_test_passed;
extern int g_test_failed;

/* Record a passed or failed assertion. `cond` is the boolean; `msg`
 * describes the behaviour. */
void test_assert(const char *suite, const char *msg, int cond);

#define expect(cond, msg) test_assert(__FILE__, msg, (cond))

/* Each test_*.c calls test_finish at the end of main(); returns 0 if all
 * passed, 1 otherwise. */
int test_finish(const char *suite);

#endif
