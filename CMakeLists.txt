cmake_minimum_required(VERSION 3.20)
project(mkdbg C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── libgit2: minimal static build (no HTTPS, no SSH, no TLS) ─────────────────
# Used by mkdbg-native for local git operations without requiring a git CLI.
# Network operations (push/fetch) fall back to the git CLI subprocess path.
set(BUILD_SHARED_LIBS    OFF CACHE BOOL   "" FORCE)
set(BUILD_TESTS          OFF CACHE BOOL   "" FORCE)
set(BUILD_CLI            OFF CACHE BOOL   "" FORCE)
set(USE_HTTPS            OFF CACHE BOOL   "" FORCE)
set(USE_SSH              OFF CACHE BOOL   "" FORCE)
set(USE_GSSAPI           OFF CACHE BOOL   "" FORCE)
set(USE_NTLMCLIENT       OFF CACHE BOOL   "" FORCE)
set(REGEX_BACKEND        builtin CACHE STRING "" FORCE)
set(SONAME               OFF CACHE BOOL   "" FORCE)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/libgit2/CMakeLists.txt")
  add_subdirectory(deps/libgit2 libgit2_build EXCLUDE_FROM_ALL)
  set(MKDBG_HAS_LG2 ON)
else()
  message(STATUS "deps/libgit2 submodule not initialised — building mkdbg without libgit2")
  set(MKDBG_HAS_LG2 OFF)
endif()

# ── seam_lib: host-side causal analysis ──────────────────────────────────────
add_library(seam_lib OBJECT
  deps/seam/src/seam_analyze.c
  deps/seam/src/seam_rules.c
  deps/seam/src/seam_print.c
)
target_include_directories(seam_lib PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/deps/seam/include
)
target_compile_options(seam_lib PRIVATE -Wall -Wextra -O2)

# ── wire_crash_lib: in-process crash diagnostics (embedded in mkdbg-native) ──
# Cortex-M crash dump over UART — no wire-host subprocess required at runtime.
add_library(wire_crash_lib OBJECT
  deps/wire/host/wire_rsp_client.c
  deps/wire/host/wire_crash.c
  deps/wire/host/wire_serial.c
)
target_include_directories(wire_crash_lib PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/deps/wire/include
  ${CMAKE_CURRENT_LIST_DIR}/deps/wire/host
)
target_compile_options(wire_crash_lib PRIVATE -Wall -Wextra -O2)

# ── mkdbg-native: main CLI ────────────────────────────────────────────────────
add_executable(mkdbg-native
  src/main.c
  src/util.c
  src/process.c
  src/config.c
  src/incident.c
  src/parse.c
  src/core.c
  src/launcher.c
  src/git.c
  src/probe.c
  src/action.c
  src/serial.c
  src/seam.c
  src/dashboard.c
  src/wire.c
  src/debug_session.c
  src/debug_cli.c
  src/debug_tui.c
  src/dwarf.c
  src/termbox2_impl.c
  arch/cortex_m.c
  arch/arch_registry.c
  $<TARGET_OBJECTS:seam_lib>
  $<TARGET_OBJECTS:wire_crash_lib>
)
target_compile_options(mkdbg-native PRIVATE -Wall -Wextra -Werror -O2)
target_include_directories(mkdbg-native PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/arch
  ${CMAKE_CURRENT_LIST_DIR}/deps/seam/include
  ${CMAKE_CURRENT_LIST_DIR}/deps/wire/include
  ${CMAKE_CURRENT_LIST_DIR}/deps/wire/host
  ${CMAKE_CURRENT_LIST_DIR}/deps/termbox2
)
if(MKDBG_HAS_LG2)
  target_compile_definitions(mkdbg-native PRIVATE MKDBG_USE_LG2=1)
  target_include_directories(mkdbg-native PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/deps/libgit2/include
  )
  target_link_libraries(mkdbg-native PRIVATE libgit2package)
endif()
set_target_properties(mkdbg-native PROPERTIES
  OUTPUT_NAME "mkdbg-native"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ── seam-analyze: standalone causal-chain analysis tool ──────────────────────
add_executable(seam-analyze
  src/seam_main.c
  src/seam.c
  $<TARGET_OBJECTS:seam_lib>
)
target_compile_options(seam-analyze PRIVATE -Wall -Wextra -Werror -O2)
target_include_directories(seam-analyze PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/host
  ${CMAKE_CURRENT_LIST_DIR}/deps/seam/include
)
set_target_properties(seam-analyze PROPERTIES
  OUTPUT_NAME "seam-analyze"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ── wire-host: TCP↔UART GDB bridge (optional standalone tool) ────────────────
add_executable(wire-host
  deps/wire/host/wire_host.c
  $<TARGET_OBJECTS:wire_crash_lib>
)
target_compile_options(wire-host PRIVATE -Wall -Wextra -Werror -O2)
target_include_directories(wire-host PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/deps/wire/include
  ${CMAKE_CURRENT_LIST_DIR}/deps/wire/host
)
set_target_properties(wire-host PROPERTIES
  OUTPUT_NAME "wire-host"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ── ctest: seam-analyze smoke ─────────────────────────────────────────────────
enable_testing()
add_test(
  NAME seam_analyze_kdi_cascade
  COMMAND $<TARGET_FILE:seam-analyze>
          ${CMAKE_CURRENT_SOURCE_DIR}/deps/seam/test/fixtures/normal_kdi_cascade.cfl
)
set_tests_properties(seam_analyze_kdi_cascade PROPERTIES
  PASS_REGULAR_EXPRESSION "VERDICT:"
)

# ── ctest: arch plugin registry ───────────────────────────────────────────────
add_executable(arch_test
  tests/arch_test.c
  arch/cortex_m.c
  arch/arch_registry.c
)
target_compile_options(arch_test PRIVATE -Wall -Wextra -Werror -O2)
target_include_directories(arch_test PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/arch
)
add_test(NAME arch_registry COMMAND arch_test)
