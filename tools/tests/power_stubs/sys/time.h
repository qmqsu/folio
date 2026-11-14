#pragma once
#include_next <sys/time.h>

// Test-only redirection: never resolve settimeofday to the host libc symbol.
int folio_power_test_settimeofday(const struct timeval* value, const void* zone);
#undef settimeofday
#define settimeofday folio_power_test_settimeofday
#define FOLIO_POWER_TEST_TIME_MOCK 1
