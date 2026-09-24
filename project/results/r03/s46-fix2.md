# M3b.S4-S7 fix2 报告 (r03) — A/B 已修, churn 残留 = 新定性缺陷 C

- **VERDICT: PARTIAL** — 缺陷 A（新鲜页 MAPERR）与缺陷 B（RELEASE/exit drain 挂死）
  根因修复并有通过证据; **churn/mixed 仍 FAIL（errors>0）, 根因定性为第三缺陷 C
  （预先存在, 非本轮改动引入）, 未修, 移交下一班**。
- 对象: worktree `/home/ppw/linux-6.18-m3b46`（树内容 = tag `7bba3b9f7390` + 本轮
  增量, `git diff 7bba3b9f7390` 仅 5 文件 +256/-28, 已核对内容基座）。
  增量补丁: `patches/r02-m3b-s46-increment.diff`（基座=7bba3b9f7390, 460 行）;
  full: `patches/r02-m3b-s46-full.diff`（基座=d040b61051af）。bzImage 快照:
  `bzimg/r02-m3b-s46-fix2`。
- 时间: 2026-09-16 01:59–03:3x; 未 commit / 未 push / 未动主树、m3b、m9 / 密码未落盘。

## 缺陷 A（核心语义缺失）: 已修复 ✅

- **根因**: `corten_arena_fault_once()` 对 `corten_query()` 返回 CORTEN_INVALID 的
  页直接 dispatch default → MAPERR。缺了论文 Fig.8 L26-38 的"缺页主体": arena 内
  从未记录的页的首次访问 = PrivateAnon 虚拟分配的合成。旧 KUnit 全绿是用例自播种
  metadata 的盲区; churn 的 mark 路径（mmap MAP_FIXED → mmap_route 显式
  corten_mark）绕过它, 所以旧内核 churn 线程本体不 segfault——与 r03 现场自洽
  （touch/mixed 必死、churn 死于缺陷 B）。
- **修复**（mm/corten_arena.c + mm/corten_arena.h + mm/corten_fault_test.c）:
  1. 纯分类器 `corten_arena_dispatch()`: CORTEN_INVALID → 新增 `CORTEN_DISP_FRESH`
     （注释指明 Fig.8 语义; 权限门需要 arena prot, 纯函数看不到, 由调用方补足）。
  2. `corten_arena_fault_once()`: 查询后若 INVALID → 以 `ctx->ar->prot`（DECLARE
     时从 VMA 固化的 arena 契约）做 `corten_arena_perm_ok()` 门: 通过 →
     `corten_mark()` 合成 PRIVATE_ANON → 后续与 mmap-mark 路径完全同一条
     （读→共享零页, 写→map_anon, 零页写升级复用既有机制, map_anon 的
     corten_map 落 MAPPED）; 不通过 → ACCERR（SIGSEGV 口径保持）。
     mark 的 -ENOMEM/-EAGAIN 分别映射 OOM/RETRY。
  3. **新盲区用例** `corten_fault_test_fresh_fault`（不预播种 metadata 的纯新鲜
     fault）: 写故障 → PTE present+writable+folio anon/exclusive/off-LRU + 内容
     kmap 写读回 + metadata=MAPPED+arena perm; 读故障 → 共享零页 + metadata
     保持 PRIVATE_ANON + 零 anon 记账; 其写升级复用主链。sigsegv 用例改写:
     "never marked → SIGSEGV" 旧预期废止, 保留 RO 页写 ACCERR + 新增"未播种页
     instruction fault（RW arena 无 EXEC）→ ACCERR"。dispatch 表 2 个 INVALID
     case 改期望 FRESH。**用例注释写明教训: 每条真链路用例必须至少覆盖一个未
     播种页（review+KUnit 双双漏过的根因）**。
- **证据**: guest 真冒烟 `--mode touch 4线程 30s 64MB`: **exit 0, errors:0,
  op_errors:0, checksum 产出, "prctl RELEASE ok", pages_touched 12.4 亿**
  （旧内核同参 100% 首页 SIGSEGV exit 139, 见 results/r03/smoke-on/stress-touch.err）。
  host KUnit corten=on: fault 11/0/0（新增 1 case）; off×2: 3/0/8 一致。

## 缺陷 B（RELEASE/exit drain 挂死）: 已修复 ✅

- **根因（配对表 #3, 双 get 单 put）**: `corten_arena_munmap_route()` 为判定跨界
  做了 `lookup_get(start)` + `lookup_get(end-1)` 两次 tryget; 当两端落入同一
  arena（in-arena munmap 的常态）两个指针相等, 而 put 侧写的是
  `if (ar_end && ar_end != ar_start) put(ar_end)` —— **每次调用净泄漏 1 个
  active 引用**（EXACT 分支与公共出口两处同病）。churn 每个 chunk munmap 泄漏
  1 → 收尾 prctl RELEASE 的 drain 永等 → D 状态不可杀。
- **修复**:
  1. 配对修复: 两处 put 改为**每个 lookup 指针各 put 一次**（同一 arena 两指针
     = 两次 get, 两次 put 正确配对）。
  2. drain 降级硬化: `corten_arena_drain()` 改 `wait_for_completion_timeout(10s)`
     + 超时 `WARN_ONCE` + 计数 `CORTEN_ARENA_STAT_DRAIN_TIMEOUTS`（per-mm 统计,
     S8 debugfs 接线后可见）; 超时时**故意不 percpu_ref_exit/free**（straggler
     put 会 UAF）, descriptor 泄漏 ~150B 换进程可退出/exit_mmap 可走完
     ——挂死的内核比泄漏的内核糟。RELEASE 与 mm_exit 两条 drain 路径都接住。
     RELEASE 上下文核对: drain 时只持 ctl_lock, **不持 mmap_write**（mmap 写锁在
     drain 之后才取, sec 6.3）, prctl 上下文可睡眠, 超时等待合法。
- **percpu_ref get/put 配对总表（review 红线）**:

| # | get 点 (mm/corten_arena.c) | put 点 | 状态 |
|---|---|---|---|
| 1 | `corten_arena_user_fault` 入口 | `out:` 唯一出口 put | ✓ |
| 2 | `corten_arena_handle_mm_fault` 入口 | epilogue put | ✓ |
| 3 | `corten_arena_munmap_route` ar_start+ar_end 双 get | EXACT: 双 put→release; 其余: 出口双 put | **✓ 本次修复** |
| 4 | `corten_arena_munmap_guard` 1 get | 出口 1 put | ✓ |
| 5 | `corten_arena_mmap_route` 1 get | 全部 5 出口（class 拒/无 vma/fill 失败/out）各 put | ✓ |
| 6 | `corten_arena_dontneed_route` 1 get | 出口 1 put | ✓ |
| 7 | `corten_arena_release`/`mm_exit` | 无 get（ctl_lock + xa_load）; drain kill 基准引用 | ✓ |
| 8 | 测试 (corten_arena_test.c ft/conc, corten_fault_test.c ft_setup/ft_race) | 均 get/put 配对 | ✓ |

- **证据**: guest churn 4线程30s 跑满后 `prctl RELEASE ok` 正常返回（旧内核在此
  D 状态永挂, SIGKILL 不可终止）; touch 同证。drain 超时路径本轮零触发
  （配对修好后无需降级）。

## 缺陷 C（新定性, 未修）: churn/mixed 内容错误 — untracked PT 页 legacy 漂移

- **现象**: churn --fixed errors>0（4线程: 54599/15.6M 触页 ≈0.35%; --verify 时
  post-unmap 零检 6% 页失败）。100% 复现, 单线程也在（非跨线程竞态）。
- **取证**（`results/r03/smoke-fix2/probe3.c`, guest 复现）:
  - `ZFAIL got=5a5a000100000320`（= 上一 cycle 的 magic）: **munmap 后重映射读出
    陈旧内容 —— zap 信任 metadata, 跳过了"PTE 在而 metadata INVALID"的页**。
  - `INPLACE got=0`（写 magic 后立刻读回零页）: 写故障安装的 PTE/页与后续读不一致。
- **根因链**: PT 页描述符在 `pte_alloc_one()`（M2a 钩子）安装失败 → 该 2M 窗口
  **永久 untracked**（钩子只在新建 PT 页时触发一次, 不会对既有页补挂）→ 该窗口
  fault 在 `corten_lock_range()` -ENOENT → `CORTEN_F_FALLBACK` → **legacy body 在
  shadow-VMA 上写 PTE 而无任何 metadata**（协议不变量 INV"PTE⇔metadata"被打破）
  → munmap 路由时 `unmap_chunk` 对该窗口 -ENOENT 直接 `ret=0`（注释假设"无
  tracked 页 ⇒ 必无映射"——被 fallback 打破）, `zap_window` 又按 metadata INVALID
  跳过 → **陈旧 PTE/内容穿越 munmap**; 后续读命中陈旧翻译 → 错误内容。
- **预存证据**: r03 判定表 debugfs 数字里 `free_untracked=5` **恒定存在**（"恒定,
  无递增"）——untracked 窗口在修复前就有, 当时不可见只因 fresh fault 先 MAPERR。
  本轮改动未触碰该路径。
- **修复方向（移交, 估 ~1-2h）**: ①`fill_upper()` 对已存在 PT 页补装 descriptor
  （`corten_ptdesc_install` 幂等化/re-arm）, 让 fallback 漂移窗口在下次空间操作前
  重新被跟踪; ②`unmap_chunk` 的 -ENOENT 分支不能再假定空 —— 至少对 untracked
  窗口走 legacy zap（或拒绝 -EOPNOTSUPP）; ③漂移计数（debugfs）以便观测。
  KUnit 锚: 在 fail_alloc 注入下断言 untracked 窗口行为。

## 验证矩阵（本轮实跑）

| 项 | 结果 | 证据 |
|---|---|---|
| KUnit host corten=on | corten 20/0/5 + arena 9/0/0 + **fault 11/0/0**（含新 fresh_fault） | results/r03/kunit-on-host1.log |
| KUnit host corten=off ×2 | 两跑一致: 21/0/4 + 8/0/1 + fault 3/0/8; 零 Oops | kunit-off-host1.log, -host2.log |
| KUnit guest（8 vCPU, 开机自跑, 修复后内核） | corten 24/0/1（多核用例实跑全绿） | 串口 02:19 boot |
| guest touch 4t/30s/64MB | **PASS: exit 0, errors:0, RELEASE ok, checksum 一致** | smoke-fix2/guest-artifacts.txt |
| guest churn | FAIL（缺陷 C）: RELEASE 正常返回（B 已修）但 errors>0 | 同上 + probe3 输出 |
| guest mixed | 未跑通判据（与 churn 同缺陷 C 波及, mixed=touch+churn 交替） | — |
| maps_check | 本轮未重跑（上轮 PASS 的 shadow-VMA 面未被 A/B 触及; 复跑班次执行） | — |
| =n 链接回归 | sys/mmap/memory/migrate/mempolicy 五对象 corten 符号=0, 零错零警; config 已恢复 | 本轮 make+nm |
| checkpatch --strict | 增量 diff（vs 7bba3b9）: **0 errors, 0 warnings**（2 checks 为既有豁免类） | 本轮输出 |
| =y 全量重建 | exit 0（config 往返后; 后台日志 /tmp/fix2-rebuild.log） | — |

## 移交下一班（按优先级）

1. **缺陷 C 修复**（上述 ①②③ + KUnit 锚）→ 复跑 churn/mixed 判据。
2. 复跑全套 guest 冒烟（touch/churn/mixed 30s + maps_check + checksum）出 DoD 判定。
3. VM 现状: tmux `vm` 跑 nokaslr 修复后内核（6.18.32-ge911b31adb9c-dirty, corten=on,
   无僵尸——D 状态僵尸已随 reboot 清场）; 复跑可沿用或按 `bin/env.sh` 重启。
4. probe 工具: `results/r03/smoke-fix2/probe3.c`（内容漂移复现器, host gcc -static,
   9p /mnt 直跑）; `bzimg/r02-m3b-s46-fix2` 为本轮内核快照。
5. interlock 时钟源统一（M3a §5, 非阻塞遗留）。

## 纪律

未 commit / 未 push / 未动主树与 m3b、m9 worktree / 补丁与日志落盘 patches/、
results/r03/ / 密码未落盘。
