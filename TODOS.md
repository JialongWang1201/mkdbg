# TODOS

## Phase 1b 后清理

- [ ] **移除 `tools/build_mkdbg_native.sh`** — Phase 1b (libgit2 集成) 稳定后删除旧 shell 构建脚本，完全迁移到 CMake。保留两套构建系统增加维护负担。依赖：Phase 1a CMake 切换 + Phase 1b libgit2 集成完成。

## Phase 1b — libgit2 集成

- [ ] **libgit2 原生集成** — 把 `mkdbg git status/rev/branch/worktree` 的本地操作切换到 libgit2（静态链接），移除对外部 `git` CLI 的依赖。网络操作（push/fetch）保留 git CLI fallback（避免 TLS/SSH 依赖）。
  - **Why:** 这是"零依赖"成功标准的核心——用户不需要安装 git 即可使用 mkdbg git 子命令。
  - **Pros:** 真正零依赖；libgit2 API 稳定、MIT 许可；本地操作速度更快。
  - **Cons:** macOS 静态链接需要 `find_library(IOKit/CoreFoundation/Security)`（非平凡 CMake 配置）；需要测试 fallback 路径。
  - **Context:** Phase 1a（本分支）已经搭好 CMake 构建系统，libgit2 集成的前置条件已满足。下一步：添加 libgit2 git submodule，在 CMakeLists.txt 中静态链接，替换 `mkdbg_git.c` 中的 subprocess 调用为 libgit2 API 调用。参考设计文档的 Reviewer Concerns 中关于 macOS 静态链接配置的说明。
  - **Effort:** L human (~2 周) → M with CC (~3 小时)
  - **Priority:** P1 — 阻塞"零依赖"成功标准
  - **Depends on:** Phase 1a CMake 切换（本分支，已完成）

## Phase 4 — Dashboard（可并行开发）

- [ ] **Dashboard stub — `mkdbg dashboard` 子命令骨架** — 添加 `mkdbg dashboard` 子命令，显示串口输出 + 探针状态 stub（"no probe connected"）+ git 状态（Phase 1b 后真实数据）+ 构建状态。使用 termbox2（单文件 MIT C 库，~2000 行）作为终端 TUI 底层。
  - **Why:** Dashboard 是 mkdbg 的差异化 UX 核心；通过 stub 接口可以在 Phase 2/3 完成前先行开发，不被探针协议阻塞。
  - **Pros:** 提前建立 Dashboard 架构；serial 面板可立即真实显示数据；用户体验差异化关键。
  - **Cons:** termbox2 需要 vendor；终端 raw mode + SIGWINCH + atexit 恢复需要仔细处理。
  - **Context:** 设计文档明确指出 Phase 4 可通过 stub 接口与 Phase 2/3 并行开发。serial 模块需新增环形缓冲区 API + 线程安全读回调。并发模型：单线程 select/poll 主循环，不使用多线程。
  - **Effort:** M human (~1 周) → S with CC (~1 小时)
  - **Priority:** P2 — 差异化 UX，但不阻塞核心功能
  - **Depends on:** Phase 1a（本分支）；Phase 1b libgit2（git 面板真实数据可推迟）
