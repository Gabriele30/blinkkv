#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "store.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static int g_current_test_failed = 0;

#define COLOR_RESET  "\033[0m"
#define COLOR_RED    "\033[31m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_BOLD   "\033[1m"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    " COLOR_RED "[FAIL]" COLOR_RESET " %s:%d: Assertion failed: %s\n", \
               __FILE__, __LINE__, #cond); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_MSG(cond, msg) do { \
    if (!(cond)) { \
        printf("\n    " COLOR_RED "[FAIL]" COLOR_RESET " %s:%d: %s (%s)\n", \
               __FILE__, __LINE__, msg, #cond); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_STATUS(actual, expected) do { \
    bk_status _act = (actual); \
    bk_status _exp = (expected); \
    if (_act != _exp) { \
        printf("\n    " COLOR_RED "[FAIL]" COLOR_RESET " %s:%d: Expected status %d (%s), got %d (%s)\n", \
               __FILE__, __LINE__, _exp, bk_status_str(_exp), _act, bk_status_str(_act)); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_EQ_UINT64(actual, expected) do { \
    uint64_t _act = (uint64_t)(actual); \
    uint64_t _exp = (uint64_t)(expected); \
    if (_act != _exp) { \
        printf("\n    " COLOR_RED "[FAIL]" COLOR_RESET " %s:%d: Expected %llu, got %llu\n", \
               __FILE__, __LINE__, (unsigned long long)_exp, (unsigned long long)_act); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_EQ_SIZE(actual, expected) do { \
    size_t _act = (size_t)(actual); \
    size_t _exp = (size_t)(expected); \
    if (_act != _exp) { \
        printf("\n    " COLOR_RED "[FAIL]" COLOR_RESET " %s:%d: Expected size %zu, got %zu\n", \
               __FILE__, __LINE__, _exp, _act); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_MEM_EQ(actual, expected, len) do { \
    size_t _len = (size_t)(len); \
    if (memcmp((actual), (expected), _len) != 0) { \
        printf("\n    " COLOR_RED "[FAIL]" COLOR_RESET " %s:%d: Memory content mismatch (%zu bytes)\n", \
               __FILE__, __LINE__, _len); \
        g_current_test_failed = 1; \
        return; \
    } \
} while (0)

#define TEST_RUN(fn) do { \
    printf("  RUN  %-46s", #fn); \
    fflush(stdout); \
    g_tests_run++; \
    g_current_test_failed = 0; \
    fn(); \
    if (g_current_test_failed) { \
        g_tests_failed++; \
    } else { \
        g_tests_passed++; \
        printf(" " COLOR_GREEN "[PASS]" COLOR_RESET "\n"); \
    } \
} while (0)

#define TEST_REPORT() do { \
    printf("\n==================================================\n"); \
    if (g_tests_failed == 0) { \
        printf(" " COLOR_BOLD COLOR_GREEN "ALL TESTS PASSED" COLOR_RESET ": %d total tests\n", g_tests_run); \
    } else { \
        printf(" " COLOR_BOLD COLOR_RED "TESTS FAILED" COLOR_RESET ": %d passed, %d failed out of %d\n", \
               g_tests_passed, g_tests_failed, g_tests_run); \
    } \
    printf("==================================================\n"); \
} while (0)

#endif /* TEST_COMMON_H */
