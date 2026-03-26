/* tools/seam_main.c — standalone shim: seam-analyze binary → mkdbg_cmd_seam
 *
 * Provides main() for the seam_analyze_host CMake target.
 * All logic lives in mkdbg_seam.c; this is just the entry point.
 *
 * SPDX-License-Identifier: MIT
 */

int mkdbg_cmd_seam(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    return mkdbg_cmd_seam(argc, argv);
}
