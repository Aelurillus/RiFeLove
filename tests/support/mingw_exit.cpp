// The GCC 15.1 MSVCRT toolchain supplies an out-of-line _Exit wrapper
// that references UCRT-only imports. GoogleTest death-test support uses
// _Exit in Debug builds. Retain C99 immediate-exit semantics through
// MSVCRT's _exit, without linking a second CRT or changing GoogleTest.
// Product binaries do not include this test-only compatibility definition.
#define __CRT__NO_INLINE
#include <cstdlib>
#if defined(__MINGW32__) && !defined(_UCRT)
extern "C" void __cdecl _Exit(int status) { _exit(status); }
#endif
