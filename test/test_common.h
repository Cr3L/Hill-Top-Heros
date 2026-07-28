// Shared assertion machinery for the host test binaries.
//
// Both suites keep running after a failure and report the total at the end —
// with a lossy-channel sweep you want the whole picture, not the first seed
// that broke.
#pragma once
#include <stdio.h>

inline int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

// Same, but with a printf-style explanation — use it when the values involved
// (a seed, a state name) are what make the failure diagnosable.
#define CHECKM(cond, ...)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("  FAIL %s:%d  ", __FILE__, __LINE__);                        \
      printf(__VA_ARGS__);                                                 \
      printf("\n");                                                        \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

// Returns the process exit code, so main() can `return testSummary("...");`
inline int testSummary(const char* whatPassed) {
  if (g_failures) {
    printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
  }
  printf("\n%s\n", whatPassed);
  return 0;
}
