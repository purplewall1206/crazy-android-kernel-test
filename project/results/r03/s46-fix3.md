# M3b.S4-S7 fix3 报告 (r03) — 缺陷 C 修复, 三模式全绿（含 review 增量 fix3r）

- **VERDICT: PASS (fix 范围内)** — 缺陷 C（churn/mixed 内容错误）根因定位并修复;
  guest 真冒烟 touch/churn/mixed 各 30s **全部 exit 0 + errors:0 + RELEASE ok**,
  probe3 复现器转绿（fails=0 zfails=0）, maps_check PASS, dmesg 零 panic/Oops。
- 对象: worktree `/home/ppw/linux-6.18-m3b46`（树内容 = tag `7bba3b9f7390` + fix2
  增量 + 本轮 C 修复）。补丁: `patches/r02-m3b-s46-increment.diff`（基座=
  7bba3b9f7390, 889 行, **0E0W**）; `patches/r02-m3b-s46-full.diff`（基座=d040b61）。
- 时间: 2026-09-16 03:45–05:1x; 未 commit / 未 push / 未动主树、m3b、m9 / 密码未落盘。

## 缺陷 C 真根因（fix2 报告的"untracked 漂移"假说被修正）

- **真相: zap 的 TLB flush 缺失, 不是（只是）metadata 漂移。**
  `corten_arena_zap_window()` 用 `tlb_gather_mmu(&tlb, mm)`（**双参 = 非全 mm**,
  mm/mmu_gather.c:443 `__tlb_gather_mmu(tlb, mm, false)`）开后, 手工
  `ptep_get_and_clear()` + `tlb_remove_page()`, **完全绕过了 mmu_gather 的 flush
  range 跟踪**（从不调 `tlb_start_vma()`/`tlb_remove_tlb_entry()`）→
  `tlb_finish_mmu()` 的 flush range 为空 → **PTE 清了, TLB 没刷**。
- **后果链**（guest probe3 取证, `results/r03/smoke-fix2/guest-artifacts.txt`）:
  ZFAIL=`got=上一 cycle 同页 magic`——munmap 后重映射读不打故障（PTE none 但
  TLB 陈旧）→ 读到孤儿旧页; 后续 cycle 的 user store 同样打 进 陈旧翻译 →
  INPLACE=`got=0`（store 落孤儿页, load 走新翻译读到新装零页）。
- **为什么 fix2 的 KUnit 没抓到**: churn_repro 在纯内核侧驱动, KUnit kthread 无
  用户态 store、无真 TLB → stale=0（PTE 层 zap 本来就完整）; inplace 失败是
  测试自己没做 store 的伪影（已修正, 见下）。TLB 类缺陷只能 guest 真冒烟暴露
  ——这也是 r03 DoD 冒烟存在的意义。

## 修复（两防御一根修, 按 briefing 顺序）

| # | 修复 | 位置 | 内容 |
|---|---|---|---|
| ① | **根因: 显式 TLB flush** | `corten_arena_zap_window()` (mm/corten_arena.c) | walk 改 **PTE 内容驱动**: 每页先 `ptep_get_and_clear`, 非 none 即扩展记录 flush range; present 非 special 才释放 rmap/计数/tlb_remove_page（对 legacy fallback 写入的 PTE 同样正确——其生产者都挂 rmap 的私匿名页）; metadata 只决定是否需要 `corten_unmap()` 复位（recorded 才调）。循环后 `flush_tlb_range(vma, flush_start, flush_end)`。注释写明"为什么 mmu_gather range 为空"。 |
| ② | **防御: -ENOENT 不再假定空** | 新增 `corten_arena_zap_untracked_window()` + `corten_arena_unmap_chunk()` -ENOENT 分支 | 无 descriptor 的窗口（M2a 安装失败/legacy fallback 写入过）按 PTE 实际内容 zap（同一 present 非 special 释放逻辑 + 尾部 flush + `legacy_drift` 计数）; PMD 不存在→0（真空）, THP leaf→-EOPNOTSUPP。munmap_guard/dontneed_route 走 unmap_chunk 自动受益。 |
| ③ | **根修: 幂等补装** | `corten_ptdesc_rearm()` (mm/corten.c) + `fill_upper()` pte_alloc 后调用 | PT 页已 tracked → 直接返回（**不触碰, `install()` 的 WARN_ON(old) 契约绝不触发**）; 未 tracked → 补装 descriptor + `reinstalled` 计数。首次安装失败（GFP_NOWAIT）的窗口在下次 fill 恢复 transactional。 |
| ④ | **可观测** | mm/corten.c debugfs stats + `corten_legacy_drift_inc()` | `free_untracked` 旁新增 `reinstalled`（补装次数）与 `legacy_drift`（untracked 窗口实际清出过内容 / fault -ENOENT 中途 bail 次数）。本轮 guest 两计数均 0——防线未再被触发, 与"根因是 TLB 而非漂移"的定性互证。 |

- `fault_once` 的 lock_range -ENOENT fallback 分支补 `corten_legacy_drift_inc()`
  + 注释指向修复③。

## KUnit: churn_repro 用例修正并转正

- fix2 夜的 `corten_fault_test_churn_repro` 的 inplace 失败是测试伪影（内核侧
  驱动故障链后没有 store, kmap 读回恒 0）。修正: 写故障后 **kmap 侧写 magic 再
  读回**（kernel store 替身）, 断言同翻译读回一致 + PTE present/writable/非
  special; 保留 munmap→重映射后的 **陈旧 PTE 扫描（stale=0）** 与
  mmap_route→touch→verify→unmap 的 150 轮随机 chunk 循环。
- 结果: `corten_fault` **12/12**（on）; off 3/0/9（新用例按降级口径 skip）。

## 验证矩阵（本轮实跑）

| 项 | 结果 | 证据 |
|---|---|---|
| KUnit host corten=on | corten 20/0/5 + arena 9/0/0 + **fault 12/0/0** | results/r03/fix3-kunit-on1.log |
| KUnit host corten=off ×2 | 两跑一致: 21/0/4 + 8/0/1 + fault 3/0/9; 零 Oops | fix3-kunit-off1.log, -off2.log |
| guest probe3 | **fails=0 zfails=0 + RELEASE ok**（修复前 96/31625） | smoke-fix3/guest-fix3-evidence.txt |
| guest touch 4t/30s | exit 0, **errors:0**, op_errors:0, RELEASE ok | 同上 |
| guest churn 4t/30s | exit 0, **errors:0**, op_errors:0, RELEASE ok（修复前 54599） | 同上 |
| guest mixed 2t/30s | exit 0, **errors:0**, op_errors:0, RELEASE ok | 同上 |
| maps_check（mixed 40s 运行中快照） | **PASS**: parse/overlap/arena-exactly-one/name `[anon:corten_arena]` 全过 | 同上（首次 FAIL 为取证脚本 pgrep -f 误中 bash 包装壳, 非内核问题, 已注明） |
| dmesg（全程） | panic/BUG/Oops 计数 0 | 同上 |
| debugfs stats | `reinstalled 0 / legacy_drift 0` 行在位; 本轮零触发 | 同上 |
| =n 链接回归 | sys/mmap/memory/migrate/mempolicy 五对象 corten 符号=0; config 已恢复 | 本轮 make+nm |
| checkpatch --strict | 增量 diff（vs 7bba3b9）: **0 errors, 0 warnings** | 本轮输出 |
| =y 全量重建 | exit 0（config 往返后） | /tmp/fix3-rebuild.log |

## 与 M3 DoD 判定的关系

- DoD#1 的三个失败模式 A/B/C 至此全部有修复与通过证据; 本轮三模式 30s 冒烟 +
  maps_check 构成完整复核。终判（含 kselftests 复跑、perf 符号复测、多参数扫）按
  流程留给主 agent 派冒烟/review/maintainer 班次。
- VM 现状: tmux `vm` = 修复后内核（kaslr, corten=on）, 无僵尸。

## 纪律

未 commit / 未 push / 未动主树与 m3b、m9 worktree / 补丁与日志落盘 patches/、
results/r03/ / 密码未落盘。

## 增量 review 三发现与处置（fix3r, 2026-09-16 ~04:3x–05:1x）

| # | 发现 | 处置 | 结果 |
|---|---|---|---|
| 1 | mmu_gather range 跟踪: 两 zap 函数的 `ptep_get_and_clear` 绕过 `tlb_remove_tlb_entry()`——(a) DEBUG_VM 内核 mid-batch `tlb_flush_mmu` 会踩 `VM_BUG_ON(!tlb->end)`, (b) 页批满 9200 页中途 flush 空 range 放页的窄 stale-TLB 窗; 且早退路径不对已清 span 补 flush | ①两 zap 函数在 `!pte_none` 清除后（pte_unlock 前）补 `tlb_remove_tlb_entry(tlb, ptep, addr)`（x86 走通用层 `tlb_flush_pte_range()`, 把 span 喂回 gather）; ②`zap_window` 的 -EAGAIN/`corten_unmap` WARN 与 `zap_untracked_window` 的 -EAGAIN 早退统一改为 `goto out_flush`（对已清 span 补 `flush_tlb_range` 再返回）。与显式 flush 双覆盖无害 | 编译干净; 全套 KUnit 绿 |
| 2 | mm/corten.h:182 `child` 成员多一个 tab（早前 C-8 同款 whitespace 污染随工作树带入增量） | 还原 | 增量 diff 中该行消失 |
| 3 | fix2 移交承诺的注入用例缺位 | 新增 `corten_fault_test_untracked_drift`（#ifdef CONFIG_CORTEN_MM_KUNIT_TEST）: `corten_test_inject_alloc_fail(2)` 使 pte_alloc 的安装与同轮 re-arm 均失败 → 窗口存在 PT 页但 untracked → 断言 (a) `corten_lock_range` = **-EOPNOTSUPP**（修正认知: untracked PT 页是 -EOPNOTSUPP, 纯洞才是 -ENOENT）; (b) 手工模拟 legacy fallback 写入的普通匿名 PTE 被 munmap 漏斗按内容释放（PTE 归 none + MM_ANONPAGES 闭合 + `legacy_drift` 递增）; (c) 下次 fill_upper 幂等补装成功（`reinstalled` 递增）且窗口恢复事务能力（ft_mark+写故障成功）。配套: `corten_ptdesc_rearm` 修为 install 失败不计数不报 tracked; unmap_chunk 把 **-EOPNOTSUPP 与 -ENOENT 同路由**到内容 zap（前者才是 untracked 主形态）; fault_once 两码都计 drift | 用例 13/13 绿; Bad rss-counter 告警（错误路径计数不对称的产物）随之消失 |

- **fix3r 验证**: KUnit on: 20/0/5 + 9/0/0 + **fault 13/0/0**（`untracked_drift` 新增）;
  off×1: 21/0/4 + 8/0/1 + fault 3/0/10; guest 快冒烟（bzimg/r02-m3b-s46-fix3）:
  probe3 fails=0/0 + RELEASE ok, touch 4t/15s exit0 errors:0, churn 4t/15s exit0
  errors:0; =n 五对象零 corten 符号; checkpatch **0E0W**; =y 往返重建 exit 0。
  证据: results/r03/fix3r-kunit-{on,off}.log, smoke-fix3/guest-fix3-evidence.txt
  （REVIEW 班次复验节）。
- 补丁: `patches/r02-m3b-s46-increment.diff` 已重导出（基座 7bba3b9f7390, 含
  fix3r; 0E0W）。VM = tmux `vm`（bzimg/r02-m3b-s46-fix3, corten=on, 无僵尸）。
