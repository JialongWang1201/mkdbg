# TODOS

## Phase 3 — wire-host --dump + mkdbg crash attach

- [x] **Phase 3a — wire-host --dump 模式** — wire repo 新增 `wire_rsp_client.c`（RSP client 收发 + NAK/retransmit）、`wire_crash.c`（CFSR decode + heuristic backtrace + JSON 输出）、`wire_host.c` 新增 `--dump` flag。MicroKernel-MPU 新增 `tools/mkdbg_wire.c`（`WireCrashReport` struct + `wire_probe_dump()`），`mkdbg_launcher.c` `cmd_attach()` 增加 wire path（`--port/--baud` 触发），`mkdbg_dashboard.c` PROBE 面板集成非阻塞子进程轮询。同时删除 `tools/build_mkdbg_native.sh`（Phase 2 清理）。**Completed: Phase 3a (2026-03-21)**
  - **Why:** 实现"零依赖 crash 诊断"——crash 发生后不需要 GDB/OpenOCD，wire-host --dump 直接输出 JSON，mkdbg attach 输出人类可读报告，Dashboard PROBE 面板实时显示。
  - **Pros:** wire-host 可独立使用；mkdbg attach 不再依赖 GDB；Dashboard PROBE 面板有真实数据。
  - **Cons:** 需要协调两个 repo（wire + MicroKernel-MPU）；wire 子模块指针需更新；QEMU CI 测试需要会 fault 的测试固件。
  - **Context:** 设计文档已 APPROVED（~/.gstack/projects/.../wangjialong-feature-phase2-libgit2-design-20260321-201725.md）。Eng review 已完成（2026-03-21）。核心决策：AttachOptions 新增 --baud；JSON 解析放在 mkdbg_wire.c（DRY）；Dashboard 用 O_NONBLOCK pipe+fork 轮询；RSP client 实现 NAK/retransmit（max 3 retry）。
  - **Effort:** M human (~1 周) → S with CC (~45 分钟)
  - **Priority:** P1 — 阻塞 Dashboard PROBE 真实数据 + mkdbg "零依赖调试" 成功标准
  - **Depends on:** Phase 2 libgit2（✓）、Phase 4 stub（✓）
- [x] **Phase 3b — wire 嵌入 mkdbg（迁移）** — 把 `wire_rsp_client.c` + `wire_crash.c` + `wire_serial.c` 直接编译进 `mkdbg_native_host`（`wire_crash_lib` OBJECT library），去掉 wire-host 子进程依赖。wire-host 变为可选的 GDB bridge 工具。**Completed: Phase 3b (2026-03-22)**
  - **Why:** 最终目标：mkdbg attach 不需要 wire-host 二进制在 PATH 中；真正自包含。
  - **Pros:** 消除 subprocess 开销；更健壮（不依赖外部 binary）；符合"单一二进制"设计目标。
  - **Cons:** 需要 wire_rsp_client.c/wire_crash.c 先在 Phase 3a 中稳定；host/firmware 分离变模糊（wire 代码进 mkdbg）。
  - **Context:** Phase 3a 的代码直接迁移过来——不会白费。迁移路径：把 wire/host/ 中的 .c 文件改为 OBJECT library，CMakeLists.txt 类似 seam_lib 写法。wire_crash.c 新增 wire_dump_crash_to_fp() + wire_dump_crash_to_buf() 供 mkdbg 直接调用。dashboard PROBE 面板用 fork-without-exec 保持非阻塞。
  - **Effort:** S human (~3 天) → XS with CC (~20 分钟)
  - **Priority:** P2 — 依赖 Phase 3a 稳定
  - **Depends on:** Phase 3a（✓）

## Phase 2 后清理

- [x] **移除 `tools/build_mkdbg_native.sh`** — **Bundle into Phase 3a PR.** Phase 2 (libgit2 集成) 已稳定，删除旧 shell 构建脚本，完全迁移到 CMake。**Completed: Phase 3a (2026-03-21)**

## Phase 2 — libgit2 集成

- [x] **libgit2 原生集成** — 把 `mkdbg git status/rev/branch/worktree` 的本地操作切换到 libgit2（静态链接），移除对外部 `git` CLI 的依赖。网络操作（push/fetch）保留 git CLI fallback（避免 TLS/SSH 依赖）。**Completed: Phase 2 (2026-03-21)**
  - **Why:** 这是"零依赖"成功标准的核心——用户不需要安装 git 即可使用 mkdbg git 子命令。
  - **Pros:** 真正零依赖；libgit2 API 稳定、MIT 许可；本地操作速度更快。
  - **Cons:** macOS 静态链接需要 `find_library(IOKit/CoreFoundation/Security)`（非平凡 CMake 配置）；需要测试 fallback 路径。
  - **Context:** Phase 1a（seam）+ Phase 1b（wire）已完成，CMake 构建系统已搭好，libgit2 集成前置条件满足。下一步：添加 libgit2 git submodule，在 CMakeLists.txt 中静态链接，替换 `mkdbg_git.c` 中的 subprocess 调用为 libgit2 API 调用。
  - **Effort:** L human (~2 周) → M with CC (~3 小时)
  - **Priority:** P1 — 阻塞"零依赖"成功标准
  - **Depends on:** Phase 1a + Phase 1b（均已完成 ✓）

## Phase 4 — Dashboard（可并行开发）

- [x] **Dashboard stub — `mkdbg dashboard` 子命令骨架** — 添加 `mkdbg dashboard` 子命令，显示串口输出 + 探针状态 stub（"no probe connected"）+ git 状态（Phase 2 后真实数据）+ 构建状态。使用 termbox2（单文件 MIT C 库，~2000 行）作为终端 TUI 底层。**Completed: Phase 4 stub (2026-03-21)**
  - **Why:** Dashboard 是 mkdbg 的差异化 UX 核心；通过 stub 接口可以在 Phase 2/3 完成前先行开发，不被探针协议阻塞。
  - **Pros:** 提前建立 Dashboard 架构；serial 面板可立即真实显示数据；用户体验差异化关键。
  - **Cons:** termbox2 需要 vendor；终端 raw mode + SIGWINCH + atexit 恢复需要仔细处理。
  - **Context:** 设计文档明确指出 Phase 4 可通过 stub 接口与 Phase 2/3 并行开发。serial 模块需新增环形缓冲区 API + 线程安全读回调。并发模型：单线程 select/poll 主循环，不使用多线程。
  - **Effort:** M human (~1 周) → S with CC (~1 小时)
  - **Priority:** P2 — 差异化 UX，但不阻塞核心功能
  - **Depends on:** Phase 1a + Phase 1b（均已完成 ✓）；Phase 2 libgit2（git 面板真实数据可推迟）

## Completed

- [x] **Phase 3b — wire 嵌入 mkdbg（迁移）** — wire_crash_lib OBJECT library (wire_rsp_client.c + wire_crash.c + wire_serial.c) embedded into mkdbg_native_host. wire_dump_crash_to_buf() added for in-process JSON capture. wire-host subprocess eliminated; dashboard PROBE uses fork-without-exec. **Completed: Phase 3b (2026-03-22)**
- [x] **Phase 3a — wire-host --dump 模式** — wire_rsp_client.c (RSP client + NAK/retransmit), wire_crash.c (CFSR decode + heuristic backtrace + JSON output), wire-host --dump flag, mkdbg_wire.c (WireCrashReport + wire_probe_dump/start/poll), mkdbg attach --port/--baud wire path, Dashboard PROBE panel (non-blocking subprocess polling). tools/build_mkdbg_native.sh removed. **Completed: Phase 3a (2026-03-21)**
- [x] **移除 tools/build_mkdbg_native.sh** — Deleted in Phase 3a PR. **Completed: Phase 3a (2026-03-21)**
- [x] **Phase 4 stub — Dashboard TUI** — `mkdbg dashboard` 子命令，termbox2 TUI，串口环形缓冲区，libgit2 git 状态面板，探针 stub，构建 stub，单线程 poll 主循环。**Completed: Phase 4 stub (2026-03-21)**
- [x] **Phase 1a — seam 集成** — seam causal fault analysis 作为 git submodule 集成到 MicroKernel-MPU，`mkdbg seam analyze` 子命令，firmware instrumentation，triage bridge。**Completed:** Phase 1a (2026-03-20)
- [x] **Phase 1b — wire 集成** — wire GDB RSP stub 作为 git submodule 集成，bsp/wire_port.c，wire_init() 调用，wire-host 构建/安装集成，QEMU CI 测试。**Completed:** Phase 1b (2026-03-21)
