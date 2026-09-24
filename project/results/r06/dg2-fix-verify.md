# D-G'' 修复（file-MAP_FIXED 打穿 shadow-VMA）· 代码班报告

- 日期: 2026-09-17（r06 日窗 ~08:20-10:0x CST + 预 review 修复）
- worktree: `/home/ppw/linux-6.18-m4fix`，branch `m4-dg2` @ 9d74b22a1348（T0a+T0b 之上）
- diff: `/home/ppw/cortenmm/patches/r06-m4dg2.diff`（1080 行, 6 文件, +838/-25）
- 状态: **代码完成, 未 commit**（review/maintainer 另派）。夜间验证归验证班。
- 根因依据: `results/r05/dg2-analysis.md`（D1 守卫缺口 + D2 地址键控误路由）

## 1. 修复内容

### F-A（fault 侧所有权自检, D2）
- `corten_arena_user_fault`（快钩子, xa_load 命中后）: 两级自检。
  - tier-1: 缓存 shadow-VMA 边界（`corten_arena_vma_spans`）, 常态零走树;
  - tier-2（罕见: 打洞拆分/RELEASE 半途）: rcu_read_lock 下 `find_vma_intersection`
    + VM_CORTEN 位测（lock_vma_under_rcu 同款读模式; 误判双向有界, 注释论证）。
  - 不归 arena → FALLBACKS 计数 + put + legacy（file VMA 自然服务）。
- **偏离分析草案**: 不用纯缓存指针边界判 fallback——打洞后尾片 fault 会误降
  legacy, 而尾片 mprotect 是路由的（VMA 旗标陈旧）→ 回归; VM_CORTEN 探测保住尾片
  路由语义。
- 慢钩子无对应检查: memory.c 门按 vma 键控（仅 VM_CORTEN 进入）, 洞天然不达。
- **F-B seal**: `corten_arena_fault_once` 在 desc 写锁下 `corten_arena_txn_owned()`
  （xa_load frame == ar）复核后才提交——关 punch 与 in-flight fault 的提交窗竞态
  （否则往即将装上的 file VMA 下塞匿名页=静默损坏）。

### F-B（mmap 侧路由, D1）——选"前置 punch 路由"而非纯拒绝
- 纯拒绝（-EOPNOTSUPP）会让 JVM CDS map_archive 的 MAP_FIXED 直接失败,
  java 腿 F→P 不成立; 处方按任务"让 overlap 拆除经过 arena 路径"落:
  - `corten_arena_mmap_punch_route`（do_mmap 门, mmap.c:469 既有 gate 内, 持
    mmap_write）: CHUNK → punch; EXACT（含 release_classify 尾规则）→
    `corten_arena_release_locked`（DEV-13: mmap_write 已是最外层, 只取 ctl_lock）;
    PARTIAL/跨 arena → -EOPNOTSUPP（与 munmap 路由同判, 计数 mmap_punch_rejects）。
  - `corten_arena_mmap_punch`: ①先擦洞内 frames（RELEASE 顺序; 已 NULL=重 punch,
    合法）; ②`corten_arena_unmap_chunk_retry` 事务 zap（PTE 内容驱动, r03 缺陷 C）;
    ③**VMA 手术**（见 B1）。
  - backstop: `__mmap_prepare` gather 前用 `corten_arena_range_overlaps`（frame 表
    探针）拒绝一切仍持有活 arena 状态的 MAP_FIXED 形状——**故意不用 VM_CORTEN 旗标
    走查**, 否则会否决刚 punch 的洞（shadow-VMA 由 gather 拆）。覆盖: 中段 arena
    跨越（路由两端点 lookup 的盲区）、一切未路由形状。
- `corten_arena_punch_classify`（纯）= unmap_classify ∘ release_classify, 表测。

### B1（预 review BLOCKER, 潜伏 UAF）——已修
- 问题: gather 的拆分归属（vma.c:1439 start 拆 new_below=1 → 原始对象留在 doomed
  侧; :1460 end 拆同理; :1371 remove_vma 释放）使 `ar->vma` 在 punch 后悬垂——
  下一个头/尾片写缺页 tier-1 裸解引用。
- 修法（评审处方, `corten_arena_punch_split`）: punch 返回前、同一 mmap_write 下,
  对 range 交叠的每个 shadow piece:
  - ps > piece->vm_start: `__split_vma(..., ps, new_below=0)`（new=上半, 原始对象
    成为头片留在树内 → ar->vma 有效）;
  - pe < 上片 vm_end: 再 `__split_vma(..., pe, 0)` 产出尾片（new）, 中段恰为
    [ps,pe) 留给 gather;
  - ar->vma == doomed → 重指尾片; 无尾片 → NULL（fault 侧已有 !ctx.vma→FALLBACK、
    unmap_chunk/mprotect 已有 !vma→-EOPNOTSUPP, F-A tier-2 仍能找到存活片）。
  - `__split_vma` 去 static + 原型入 mm/vma.h（mm/corten_arena_test.c 已有 "vma.h"
    include 先例, include 面安全）。
  - 时序论证（注释内）: xa_erase 前进入的事务在 desc 写锁下先完成 → punch zap 串行
    其后（同锁）→ gather free 最后（mmap_write 全程持有）; 手术失败路径 mmap 即败、
    gather 不跑、无 free → 无悬垂。
- 关联适配: `corten_arena_release_locked` 擦 frame 循环容忍已 NULL（打洞后 RELEASE
  不再假 WARN）; fork_demote 从"只 unshadow ar->vma"改 piece-wise（多片 arena 不残留
  VM_CORTEN）; zap 两 helper 加 anon/file 页分支（重 punch 覆盖已住 file 页时
  MM_FILEPAGES 计数正确）。

### 注释更正（错误假设）
- mm/mmap.c:463 区、mm/vma.c:1611 区、mm/corten_arena.h guard 注释、
  corten_arena.c munmap_vma_guard 注释: 原"MAP_FIXED overlap-removal 由
  corten_arena_munmap_vma_guard 把守/到得了 do_vmi_align_munmap"均为假
  （正是 D1 缺口）, 已更正并指向 backstop/punch。

### 计数器（debugfs arena_stats）
- `mmap_punches`（punch+release 路由成功）、`mmap_punch_rejects`（不可拥有形状）。

## 2. KUnit 锚（mm/corten_fault_test.c, 套件已挂）
1. `punch_classify`（纯, corten=off 可跑）: 内部→CHUNK / 头 punch→CHUNK / 跨双
   arena→PARTIAL / 整 arena+页整→EXACT / 尾规则→EXACT / 出界→OUTSIDE;
   fault_covered 决策表（洞=file VMA→不归、洞缘 tier-1 失守→由 covering 决定、
   尾片 VM_CORTEN→仍归、cached 内→归）。
2. `punch_hole`（实链, corten=on; 中段 punch）: 洞 frame 擦除+lookup 落空、尾 frame
   保留、内容 zap（PTE none/INVALID/账目闭合）、ar->vma==存活头片（B1 不变量）、
   尾片经 tv（gather 拆出的那片）续 fault FRESH→MAPPED 可写。guest-only 半段
   （file 数据真实读回, E1）已标注。
3. `punch_head`（实链, corten=on; **B1 回归锚**）: 头 punch → ar->vma 必须
   PTR_NE doomed 且 PTR_EQ 尾片; 随后真 free（unshadow 中段 + do_munmap 走常规
   funnel）再 fault 尾片——修复后无任何对已释放对象的解引用（KASAN 干净;
   指针恒等断言在任何构建抓 B1）。
4. 既有 `mmap_classify` 等不受影响（file/MAP_FIXED→LEGACY 行为未变）。

## 3. 白天已做验证（无 make/qemu）
- `scripts/checkpatch.pl --strict`: **0 errors, 0 warnings, 0 checks**。
- 六文件花括号平衡、无 >80 列新增行、无尾随空白、无注释 */ 同行违规。
- 代码级推演: 拆分后 ar->vma 指向/引用计数/xa 一致性（B1 各 case:
  中段/头 punch/重 punch 跨片/手术失败路径全部落在存活对象或 NULL）。

## 4. 登记条件（不修, 随 commit note 上报）
- (a) punch 发生在 do_mmap 的 file_mmap_ok/MAP_LOCKED/can_do_mlock 校验**之前**
  （gate 位于 mmap.c:469 既有路由点）: 校验失败的 MAP_FIXED 会留下"已 zap+拆分"的
  洞（内容有损; MAP_FIXED 语义本为丢弃, 且 Linux 自身对失败 fixed mmap 也可留空洞）。
  低概率; 勘误登记 M4T0_SPEC。
- (b) 混合状态（洞+头尾片）下 mprotect/munmap 路由的端点 lookup（start/end-1 两次
  xa_load）存在口径缺口: 纯洞范围→legacy（自洽）; 跨洞+片范围→按 CHUNK 路由, 洞内
  窗口由事务层跳过（metadata 前瞻写仅在片内生效）——不崩溃, 但"整段语义"与 legacy
  有偏差。后续: 逐帧覆盖检查或 M4T0_SPEC 勘误（留规划者裁决）。
- (c)（自记）EXACT 路由走 release_locked: do_munmap 失败（内存压力）时 arena 已
  退场而 mmap 也失败——与 RELEASE 失败路径同款, 已有约定（计数泄露优于卡死）。

## 5. 夜间验证清单（23:00 后, 验证班）
1. `make -j6` 全量: 零新增警告（重点: mm/corten_arena.c、mm/corten_fault_test.c、
   mm/vma.c __split_vma 去 static 的 warn 扫描）。
2. KUnit: `corten_fault` 套件 on×2/off×1（off 跑纯测 punch_classify; on 跑
   punch_hole/punch_head 及全量）全绿; `corten_arena`、`corten` 套件回归。
3. corten =n: 七对象编译 + 启动冒烟（backstop/route 全部折出）。
4. guest corten=on: **run_t0_dod.sh 完整跑**——判据: java 腿 F→P
   （-version rc=0 输出与 off 逐字一致; strace 等价: 不出现 off 没有的错误返回;
   dmesg 零 corten WARN/BUG）; metis_eq 照旧（fork 边界 ACCERR=已登记 OQ-D 不计败）;
   `arena_stats`: mmap_punches ≥ 2（JVM 两段 map_archive）, accerr 不再增长,
   auto_mmaps/mprotect_routes 照常移动。
5. `dg_probe2`（E1 判定实验）: MODE on 读回 0x7f 不再 ACCERR; E2 匿名照旧 PASS;
   E5: MAP_FIXED_NOREPLACE→EEXIST、MAP_FIXED→成功且读得内容（F-B 后语义）。
6. run_mode_smoke 回归 + =n + checkpatch 复核; 失败按
   `results/r05/dg2-analysis.md` §4 实验清单定位。
