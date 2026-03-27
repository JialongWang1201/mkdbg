/* termbox2_impl.c — single translation unit that compiles the termbox2 implementation.
 *
 * Both dashboard.c and debug_tui.c use termbox2 but must not each define TB_IMPL,
 * as that would produce duplicate symbols at link time.  All other files just
 * #include "termbox2.h" (declarations only, no TB_IMPL).
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunused-function"
#  pragma clang diagnostic ignored "-Wextra"
#  pragma clang diagnostic ignored "-Wsign-conversion"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#define TB_IMPL
#include "termbox2.h"

#ifdef __clang__
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
