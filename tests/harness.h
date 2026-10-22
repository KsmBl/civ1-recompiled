#pragma once
struct test_fn { void (*fn)(void); const char *desc; int len; const char *code; };
extern const struct test_fn test_fns[];
extern const int test_fn_count;
extern int TAKEN;
