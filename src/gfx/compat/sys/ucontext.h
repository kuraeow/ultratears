// Stand-in for the macOS header <sys/ucontext.h> (MacNativeGames code on Windows): definitions live in gfx_compat.h.
// On Linux the system's own header (this folder comes first on the include path).
#pragma once
#ifdef __linux__
#include_next <sys/ucontext.h>
#endif
#include "gfx_compat.h"
