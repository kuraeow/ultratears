// Stand-in for the macOS header <sys/resource.h> (MacNativeGames code on Windows): definitions live in gfx_compat.h.
// On Linux the system's own header (this folder comes first on the include path).
#pragma once
#ifdef __linux__
#include_next <sys/resource.h>
#endif
#include "gfx_compat.h"
