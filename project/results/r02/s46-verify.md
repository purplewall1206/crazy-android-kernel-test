# M3b.S4-S7 验证报告 (r02) — 收口 (全绿)

- **VERDICT: PASS** — 验证矩阵 a-e 全部通过; 前夜断点 (corten=on fill_upper
  "stack segment" Oops) 已定位根因并修复, corten_fault 真链路 10/10 全绿。
- 对象: worktree `/home/ppw/linux-6.18-m3b46`, HEAD=`e911b31adb9c` + S1-S3
  (`patches/r02-m3b-s123.diff`) + S4-S7 增量 (`patches/r02-m3b-s46-increment.diff`,
  19 文件 +3161/-14); 全量 (`patches/r02-m3b-s46-full.diff`, 25 文件 +4918)
- 时间: 2026-09-14 04:03 断点 → 2026-09-15 23:13–00:0x 收口; 未 commit; 未碰主树
  / m3b 树 / tmux vm 会话

## 验证矩阵 (本轮全跑)

| 步骤 | 结果 | 证据 |
|---|---|---|
| a. =y 构建 | exit 0; 警告仅 2 条上游既有 (objtool cpuidle_enter_state / modpost memblock_end_OF_DRAM, 均已用 touch 定向复现确证), 零新增; =n 往返后全量重建同样干净 | s46-build.log (前夜) + 本轮 make 输出 |
| b. KUnit corten=off ×2 | 两跑一致: `corten: pass:21 fail:0 skip:4` (2 条 WARNING 为基线故意的 WARN 路径用例, 与前夜日志逐条一致) + `corten_arena: pass:8 fail:0 skip:1 total:9` (新 overlap 锚 case 在 off 下按设计 skip) + `corten_fault: pass:3 fail:0 skip:7`; 零 Oops | results/r02/s46-kunit-off-run1.log, -run2.log |
| c. KUnit corten=on | `corten: pass:20 fail:0 skip:5` (M3a 设计内 skip) + `corten_arena: pass:9 fail:0 skip:0` (含新 overlap 锚) + **`corten_fault: pass:10 fail:0 skip:0`** (真链路 fill_upper_race/map_race/chunk_unmap 全过); 双跑一致, 且 =n 往返重建后的最终 bzImage 再认证一次全绿; `page descriptors enabled` 在位 | results/r02/s46-kunit-on.log, s46-kunit-on-final.log |
| d. =n 链接回归 | scripts/config -d CORTEN_MM -d CORTEN_MM_ARENA -d CORTEN_MM_KUNIT_TEST → olddefconfig → 重编 kernel/sys.o mm/mmap.o mm/memory.o **mm/migrate.o mm/mempolicy.o** (后两个为本轮新钩子): nm 五对象 **corten 符号=0**, 零错误零警告; config 已恢复 (=y 三项核回) | 本轮 make + nm 输出 |
| e. checkpatch --strict | 全 diff (`git diff | checkpatch --strict -`): **0 errors, 2 warnings** — 两条均为已记录豁免 (新文件 MAINTAINERS 例行 + 设计强制 in_atomic); 逐文件 patch 模式 0E0W | 本轮输出 |
| 补丁 | patches/r02-m3b-s46-full.diff (5319 行, 25 文件, e911b31+s123+S4-S7) / r02-m3b-s46-increment.diff (3648 行, 19 文件, 相对 r02-m3b-s123 补丁后状态, --index 基线重建) | patches/ |

## 断点根因与修复 (前夜遗留, 本轮闭合)

- **现象**: corten=on 下 fill_upper fresh-fill 触发 `Oops: stack segment` (#SS),
  探针见 pud entry=`0xf000ff53f000ff53` 且 pud_none()=0。
- **根因** (探针值本身即铁证): 旧 fill_upper 在父项尚未填充时**预计算**
  `pud_offset(p4dp, addr)`。x86 的 `pud_offset()` 会解引用父项
  (`p4d_pgtable()`) 取下一级表指针: fresh mm 的 pgd/p4d 项还是零 →
  `p4d_pgtable(0)=__va(phys 0)` → 指向**物理页 0** (BIOS 中断向量表)。IVT 表项
  是 segment:offset 对 (如 `f000:ff53`), 小端成对读出正是
  `0xf000ff53f000ff53` — 重复模式不是"未清零页", 是 IVT 内容; 垃圾 present 位
  使 pud_none()=0, 后续 pmd_alloc() 被骗成 no-op, pmd_offset() 得非规范地址,
  leaf 检查读取即 #SS。
- **修复** (上一班完成, 本班核验+收尾): fill_upper 重写为标准 alloc-chain
  (`p4d_alloc → pud_alloc → pmd_alloc → pte_alloc`), 每级指针取自 pX_alloc()
  返回值而非预计算 offset, 逐级 NULL 检查; 4 级折叠 (p4d_alloc no-op,
  pud_alloc 装 pgd 项) 与 5 级均正确, 与 `__handle_mm_fault` 规范一致。
  `corten_arena_pmd()` 辅助走查保留 top-down presence 门。**该 bug 只存在于
  CortenMM 自身的合成走查, 真实内核路径 (pud_alloc 必然清零/填充) 从未受影响**
  — 即非内核级 bug; KUnit 用 `mm_alloc()` 真地址空间 (pgd 正常清零), 主攻假设
  ①"测试合成树未清零" 不成立。本班收尾: 删除 corten_arena_pmd() 处遗留的
  过时注释块 ("Untracked walk ... Safe because fill_upper() has made the whole
  chain present", 与修复后语义矛盾); 探针残留已无 (grep 全 diff 仅合法注释)。
- **验证**: corten_fault 10/10 (含 fill_upper_race 200 轮双线程、map_race) 于
  corten=on 全绿, 双跑 + 重建后终跑共 3 次无 flake。

## 本轮新增: move_pages / migrate_pages 入口拒绝钩子 (M4T0_SPEC 审计缺口 #9)

- 缺口: M4T0_SPEC.md 入口审计 #9 — rmap 迁移会无事务、无覆盖 desc 写锁地改写
  arena PTE (违 sec 6.1 R2), S4-S7 原无钩子。
- 实现 (沿用既有 reject-hook 惯例: `#ifdef CONFIG_CORTEN_MM_ARENA` +
  `corten_arena_range_overlaps()` + 设计注释, mlockall 式整 mm 门):
  1. `mm/migrate.c kernel_move_pages()`: `nodes!=NULL` (move 腿) 且
     `range_overlaps(mm, 0, TASK_SIZE)` → `mmput + -EOPNOTSUPP`; 只读的
     do_pages_stat() 腿保持可达。含 `#include "corten_arena.h"`。
  2. `mm/mempolicy.c kernel_migrate_pages()`: `get_task_mm()` 后、
     `do_migrate_pages()` 前同判 → `mmput + -EOPNOTSUPP + goto out`
     (scratch 正确释放)。
- KUnit 锚: 决策点 `corten_arena_range_overlaps()` 原无测试覆盖, 新增
  `corten_arena_test_range_overlaps` (mm/corten_arena_test.c, 挂在 declare_query
  之后): 无 arena 时含整 mm 扫全 false; DECLARE 后内部两端/跨块/整 mm 扫 true,
  前一页/后一页/不相交/零长 false。corten=off 下按既有降级口径 kunit_skip
  (决策函数首行即 corten_enabled_static() 门) — off 矩阵的 skip:0→1 与 on 的
  pass:8→9 即此。
- 精度说明: 入口级整 mm 拒绝 (与 mlockall 钩子同型) 是 M3 的 fail-closed 口径;
  逐页 status[] 精细拒绝留给 M4T0 路由化, 与 M4T0_SPEC "T0 新增入口重叠拒绝"
  定位一致。

## 未执行 (移交主 agent 派后续班次)

- guest 冒烟 (smoke-plan.md 全清单) 与 review/maintainer 流程。
- 双跑覆盖: off ×2、on ×3 (双跑 + 终认证), flake 检查充分。

## 纪律

未 commit / 未 push / 未动主树与 m3b 树 / 未碰 tmux vm 会话 / 探针零残留 /
补丁与日志已落盘 patches/ 与 results/r02/ / 密码未落盘。
