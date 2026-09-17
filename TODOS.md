# TODOS

## mkdbg TUI / CLI 设计系统文档

**What:** 新增顶层 `DESIGN.md`，定义 mkdbg 的 TUI / CLI 设计系统：pane 层级、颜色 token、无颜色模式、状态栏、hint bar、错误文案、窄终端降级、`--dump`/`--json` 非交互输出约定。

**Why:** `/plan-design-review` 已为 `mkdbg timeline` 补了轻量 TUI design system，但项目根目录没有统一设计源。后续 dashboard、debug TUI、timeline TUI、replay 文本输出如果各自决定视觉规则，会导致终端体验不一致。

**Pros:** 后续实现 `mkdbg timeline`、dashboard 改版、Windows 文本降级时有统一规范；减少颜色滥用、空白 pane、不可访问状态和不一致快捷键。

**Cons:** 需要一次 `/design-consultation` 或手写整理，短期不直接增加功能。

**Context:** 设计评审文档已批准：`~/.gstack/projects/JialongWang1201-mkdbg/wangjialong-fix-dashboard-port-lifetime-design-20260608-212436.md`。其中 `Lightweight TUI Design System` 可作为 `DESIGN.md` 的起点。

**Depends on:** `mkdbg timeline` PR-1 前后均可；不阻塞 host timeline model，但建议在 TUI PR 前完成。
