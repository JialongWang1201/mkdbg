/* debug_tui.h — 4-panel TUI debugger (termbox2)
 *
 * Called from debug_cli.c when the user types 'tui'.
 * Returns when the user presses 'q' (quit) or 't' (back to CLI).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DEBUG_TUI_H
#define DEBUG_TUI_H

#include "debug_session.h"
#include "dwarf.h"

#define TUI_QUIT 0   /* user pressed 'q' — caller should detach and exit */
#define TUI_CLI  1   /* user pressed 't' — caller returns to CLI REPL    */

/* Enter TUI mode.  Initialises termbox, runs the event loop, shuts down
 * termbox, and returns TUI_QUIT or TUI_CLI. */
int debug_tui_run(DebugSession *s, DwarfDBI *dbi);

#endif /* DEBUG_TUI_H */
