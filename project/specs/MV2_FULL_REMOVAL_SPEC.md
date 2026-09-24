# MV2 · 字面完全移除 VMA（MODE 进程 maple 树为空）
授权: STATE D28（2026-09-24 用户直接指令, 推翻 D20-a 两处保留）。
基线: 主树 037bfbaed020（M-V 完成, corten-mv-complete）。
终态判据: MODE 进程 **maple 树条目数 == 0**（live 断言）∧ J1-J4 全绿维持 ∧ 零改动回归集
∧ vm_area_struct 分配数对 MODE mm 恒 0（含 carrier——detached 也算）。

## 切片表（依赖线性; W-1 是拱心石）

| 片 | 内容 | 规模估 | 风险 |
|---|---|---|---|
| **W-1** | corten 原生 rmap：替代窗口页的 anon_vma 与 file i_mmap 锚。folio→mapper 索引（per-folio 反向指针或 frame 表派生）+ rmap_walk/ttu/hwpoison/migrate 消费者改道（M6 守卫已在, 升级为真路由或结构性排除） | 大, 先设计片后实施片 | 全系列最高——内核 rmap 消费面广 |
| W-2 | carrier 消灭：fork_copy_ptes 纯 metadata 化（W-1 的原生 rmap 承接 file 页 dup 与 COW 锚）/ GUP-slow 摘 carrier（check_vma_flags 仿真已在, folio 操作改 metadata） | 中 | fork 对拍锚全量复用 |
| W-3 | 委托域迁移四件：a) brk region 化（V-E.2 复活, ~400 行）b) 栈（主栈 GROWSDOWN 的 region 增长臂 + 线程栈普通 region）c) exec 镜像（file region, V-B 机制复用 + 显式地址路由）d) vdso/vvar（arch 安装钩子改道 region） | 大 | exec/信号 ABI 细节 |
| W-4 | MODE 入场扫入：进场时存量 VMA 活体转 region（一次性事务, ~40 条） | 中 | 转换窗口的并发正确性 |
| W-5 | 植入片消灭：显式 MAP_FIXED 全路由进 punch+region（遗留登记臂删除） | 小 | — |
| W-6 | 终判据：tree==0 live 断言 + J1-J4 扩展版电池 + 全回归 + REPORT 终稿 | 小 | — |
| ADV-1 | 事务体残留读锁折返删除（<0.5%, 随手片）; **W-1 后按 ADV 设计文档"条件翻转点"条款重评完整 _adv（先量测后动码）** | 微 | — |

## 风险登记
R1: 匿名页 folio->mapping 为 NULL 后的内核侧假设（page_dump/proc/调试器）——逐点钩或结构性排除。
R2: 迁移/compaction 对窗口页维持拒绝（M6 现状, W-1 后可选择性重开）。
R3: 入场扫入的一次性代价（~40 VMA, 事务化; 预期毫秒级一次性）。
R4: 每片走完整协议链（复审/三套件/=n/guest 门/严格门维持）——M-V 的 12 片节奏实证可行。
