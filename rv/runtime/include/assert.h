// A failed assert prints "file:line: function: Assertion `expr' failed." on stderr and aborts.
// No include guard: the meaning of assert follows NDEBUG at each inclusion.
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
__attribute__((noreturn)) void __assert_fail(const char* expr, const char* file, int line,
                                             const char* func);
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#if !defined(static_assert) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define static_assert _Static_assert
#endif
