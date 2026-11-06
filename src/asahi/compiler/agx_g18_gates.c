/*
 * Copyright 2026 Aurora Silicon
 * SPDX-License-Identifier: MIT
 */

/*
 * The G18G (M6) A/B gates: each run-numbered G18 path stays selectable for
 * comparison and is on by default. They live apart from agx_compile.c so the
 * packer, tilebuffer and graphics-program tests that link the code reading
 * them without the whole compiler still link.
 */

#include "util/u_debug.h"
#include "agx_debug.h"

/* G18 multisample admission is on unless AGX_G18_MSAA4=0; cache-keyed. */
DEBUG_GET_ONCE_BOOL_OPTION(agx_g18_msaa4, "AGX_G18_MSAA4", true)

bool
agx_g18_msaa4_enabled(void)
{
   return debug_get_option_agx_g18_msaa4();
}

/* G18 encodings grounded on the verified native capture 1094 (run 1109) are
 * on unless AGX_G18_NATIVE1109=0, which restores the 1104 behaviour for A/B.
 * The persistent shader cache stays off while the refusal diagnostics are
 * forced on (agx_disk_cache_init), so no cached binary crosses settings. */
DEBUG_GET_ONCE_BOOL_OPTION(agx_g18_native1109, "AGX_G18_NATIVE1109", true)

bool
agx_g18_native1109_enabled(void)
{
   return debug_get_option_agx_g18_native1109();
}

/* G18 changes grounded on the verified native capture 1094 by run 1121 are
 * on unless AGX_G18_NATIVE1121=0, which restores the 1109 behaviour for A/B.
 * AGX_G18_NATIVE1109 keeps its meaning. */
DEBUG_GET_ONCE_BOOL_OPTION(agx_g18_native1121, "AGX_G18_NATIVE1121", true)

bool
agx_g18_native1121_enabled(void)
{
   return debug_get_option_agx_g18_native1121();
}

/* G18 changes grounded on the verified native capture 1121 by run 1128 are
 * on unless AGX_G18_NATIVE1128=0, which restores the 1121 behaviour for A/B.
 * AGX_G18_NATIVE1109 and AGX_G18_NATIVE1121 keep their meanings. */
DEBUG_GET_ONCE_BOOL_OPTION(agx_g18_native1128, "AGX_G18_NATIVE1128", true)

bool
agx_g18_native1128_enabled(void)
{
   return debug_get_option_agx_g18_native1128();
}

/* The G18 mixed-format tile path grounded on the verified native capture 1121
 * by run 1130 is on unless AGX_G18_NATIVE1130=0, which restores the 1128
 * behaviour for A/B. AGX_G18_NATIVE1109, AGX_G18_NATIVE1121 and
 * AGX_G18_NATIVE1128 keep their meanings. */
DEBUG_GET_ONCE_BOOL_OPTION(agx_g18_native1130, "AGX_G18_NATIVE1130", true)

bool
agx_g18_native1130_enabled(void)
{
   return debug_get_option_agx_g18_native1130();
}

/* The general G18 single-sample mixed-format tile path grounded on the
 * verified native capture 1130 by run 1139 is on unless AGX_G18_NATIVE1139=0,
 * which restores the run-1134 behaviour for A/B. Whether the kernel admits a
 * 40-, 56- or 72-byte 32x32 tile is the uAPI feature bit
 * DRM_ASAHI_FEATURE_G18_NATIVE_TILE_SAMPLES, never an environment variable. */
DEBUG_GET_ONCE_BOOL_OPTION(agx_g18_native1139, "AGX_G18_NATIVE1139", true)

bool
agx_g18_native1139_enabled(void)
{
   return debug_get_option_agx_g18_native1139();
}
