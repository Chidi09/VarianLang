#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

/* Run all tests in a directory (recursive scan for *_test.vn).
 * filter: if non-NULL, only tests whose description contains this substring run.
 * timeout_ms: if > 0, per-test wall-clock budget; an exceeding test fails. */
int test_run_dir(const char *dir, const char *filter, int timeout_ms);

#endif /* TEST_RUNNER_H */
