# M3b.S1-S3 夜间验证报告 (r02) — 复验通过版

- **VERDICT: PASS**（首次验证 FAIL 的两个问题已修复并全量复验; 标记翻转为 `S123_VERIFY_DONE`）
- 复验人: S1-S3 dev agent 自验（dev agent 按验证 agent 移交的两个问题修复后重跑完整验证）
- 复验时间: 2026-09-14 02:20-04:35 CST（免费窗口内, timegate 放行）
- 对象: worktree `/home/ppw/linux-6.18-m3b`, 分支 `m3b-s123`,
  基座 = `1284a235f751` + M3a 未提交 diff（与已提交的 e911b31adb9c 一致, 未 rebase）
- 未 commit; bzImage 仅存于 worktree（maintainer 提交后另行归档）; 未碰主树/VM/tmux

## 修复（对应首次验证 FAIL 的两个问题）

1. **RCU_LOCKDEP_WARN 三参 → 两参**（构建失败 P0）: 本树宏为固定 `(c, s)`
   （include/linux/rcupdate.h:395/:483）, 修为
   `RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "arena lookup without rcu_read_lock() protection")`
   （措辞避开精确函数名以同时满足 checkpatch __func__ 规则, strict 0W）。
2. **CONFIG_CORTEN_MM_ARENA_KUNIT_TEST 未开**（8 个新 case 此前未跑）: 种子 KUNIT_ALL_TESTS=n,
   worktree .config 已显式 `-e CORTEN_MM_ARENA_KUNIT_TEST`（另 `-e ANON_VMA_NAME` 以覆盖
   shadow-VMA 命名路径; `-e CORTEN_MM_ARENA` 确认 default CORTEN_MM 生效）。**后续 guest/验证
   配置必须包含该两项**。

## 复验中额外发现并修复的 4 个内核/测试问题（诚实记录）

3. **refcount_t 饱和语义**: `state->nr` 是合法 0..N 计数（回零后再 DECLARE 属常态）,
   `refcount_add/inc` 自 0 起加触发 "refcount_t: saturated; leaking memory"
   （lib/refcount.c:22, 崩溃面 exit_mmap）。改为 ctl_lock 下 set 语义
   （`refcount_set(nr, refcount_read(nr) ± 1)`, 读写者互斥由 ctl_lock 保证）, 注释已说明。
4. **本树 vma cache 为 SLAB_TYPESAFE_BY_RCU**（3104138517fc）: 新 VMA 带 `vma_dummy_vm_ops`,
   须显式 `vma_set_anonymous()` 才是匿名 VMA。测试 mkvm 已对齐 mmap 路径; 内核侧
   DECLARE 校验顺势加 `vma_is_anonymous(vma)`（比 vm_file 判定更精确地表达设计 §2.1-4
   "私有匿名", 特殊映射 vm_ops 非空无文件也能拒）。
5. **KUnit 双线程模型**: case 体跑在 kunit_try_catch 线程、cleanup 在父线程,
   案例线程不能 `kthread_use_mm()`（无法跨线程安全解除）; 而本树
   `vms_complete_munmap_vmas()` 用 `current->mm`（mm/vma.c:1338）, KUnit 线程 mm=NULL
   会在 munmap 收尾崩溃。方案: RELEASE/测试 unmap 一律走自管理 attached op 线程
   （use_mm→op→unuse→complete, case join 后断言）; 合成坏-flag VMA（伪 HUGETLB/PKEY 等）
   改用树级摘除 helper（vma_iter_prealloc/clear + vma_mark_detached + vm_area_free）,
   不喂给真实 munmap 语义。
6. **C-7 重排后的次序语义**: overlap 帧检查先于 VMA 校验（ctl_lock 段内）,
   子区/跨界 DECLARE 现返回 -EEXIST（先于 -EINVAL）, 测试预期已对齐（注释写明理由）。

另: `kunit.filter_glob=corten`（无通配）不匹配套件名 `corten_arena`
（lib/kunit/executor.c glob_match 精确匹配）, 统一用 `kunit.filter_glob=corten*` 同时覆盖
M3a `corten` 套件与本切片 `corten_arena` 套件。

## 复验证据（全部原文见 results/r02/s123-*.log）

| 步骤 | 命令/配置 | 结果 |
|---|---|---|
| =y 全量构建 | `make -j6`（real 18m09s, exit 0） | PASS; warning 仅 2 条上游既有
  （objtool cpuidle_enter_state / modpost memblock_end_of_DRAM）, 与 corten 无关 |
| KUnit 第 1 跑 | corten=off, 无盘 qemu -smp 2, `kunit.filter_glob=corten*` | `# corten: pass:21 fail:0 skip:4 total:25` + `# corten_arena: pass:8 fail:0 skip:0 total:8`, 零 BUG/Oops/not-ok（s123-kunit1.log） |
| KUnit 第 2 跑 | 同上（flake 检查） | 同结果全绿（s123-kunit2.log） |
| =n 链接回归 | `-d CORTEN_MM -d CORTEN_MM_ARENA -d CORTEN_MM_ARENA_KUNIT_TEST` → olddefconfig → `make -j6`（17m10s, exit 0） | **P0/F-1 直接回归 PASS**: exit_mmap 的 corten_arena_mm_exit 调用折叠为空桩、sys.c case 折叠为单条 -EOPNOTUPP, 全内核链接通过; =n 内核无盘冒烟零 BUG/Oops（s123-off-boot.log） |
| =y 恢复重建 | 恢复 .config → olddefconfig → `make -j6`（exit 0） | PASS, 配置四项确认: CORTEN_MM=y / CORTEN_MM_ARENA=y / CORTEN_MM_ARENA_KUNIT_TEST=y / ANON_VMA_NAME=y |
| corten=on 冒烟 | 无盘 qemu `corten=on kunit.filter_glob=corten*` | `corten: page descriptors enabled`（D9 initcall 翻转日志在位）+ `# corten: pass:20 fail:0 skip:5 total:25`（corten=on 下 M3a 套件 1 例按其设计转 skip）+ `# corten_arena: pass:8 fail:0 skip:0`（s123-corton-on-kunit.log） |
| checkpatch --strict | 3 个新文件 + 改动 hunk + 全 diff | 0E0W0C（全 diff 仅 1 条新文件 MAINTAINERS 提示, 与 M2/M3a 先例一致） |

KUnit 原文摘要（run 1; run 2 同）:

```
[    4.293604] # corten: pass:21 fail:0 skip:4 total:25
[    4.308926]     # Subtest: corten_arena
[    4.559714] # corten_arena: pass:8 fail:0 skip:0 total:8
```

corten_arena 8 case: declare_reject / declare_reject_flags（负例表 12-14 组 + uffd-ctx +
SOFTDIRTY 正例）/ declare_query（xa 索引+lookup 内容+overlap）/ release（精确匹配+排空+
双重释放+往返）/ shadow_vma（flags+anon_vma+命名）/ exit（直呼钩子+真 exit_mmap 路径）/
prctl（arg5 门+关闭态负值）/ concurrent（DECLARE/RELEASE × QUERY/tryget_live 双 kthread）。

## 历史记录（首次验证 FAIL 报告, 保留）

# M3b.S1-S3 夜间验证报告 (r02)

- **VERDICT: FAIL**（步骤 1 =y 全量构建失败 → 无 bzImage → 步骤 2 KUnit 无法执行）
- 标记: `S123_VERIFY_FAILED`（未 touch DONE）
- 验证人: qemu-exec agent (M3b.S1-S3 夜班)；时间: 2026-09-14 00:40–01:05 CST
- 对象: worktree `/home/ppw/linux-6.18-m3b`, 分支 `m3b-s123`,
  HEAD=`1284a235f7510df4bdfdad166655dcf5f7a99bba` + S1-S3 未提交改动(17 个路径,
  含新文件 mm/corten_arena.c / mm/corten_arena_test.c / include/linux/corten_arena.h)
- 工具链: gcc (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0; 节流 -j6（与主树 M3a 验证并行）
- 未 commit、未动任何代码、未碰主树与 m3b46 worktree、bzImage 未归档（本就无产物）

---

## 步骤 1: =y 全量构建 — **FAIL**

命令: `make -j6 olddefconfig && make -j6`，日志 `results/r02/s123-build.log`。

olddefconfig 后配置确认（.config 原无 ARENA 条目，因其 `default CORTEN_MM` 自动为 y）:

```
CONFIG_CORTEN_MM=y
CONFIG_CORTEN_MM_KUNIT_TEST=y
CONFIG_CORTEN_MM_ARENA=y
```

失败位置: `mm/corten_arena.c`（S1-S3 新增文件，In function ‘corten_arena_lookup’）。
日志原文（s123-build.log 行 916-933）:

```
  CC      mm/corten_arena.o
  CC      kernel/trace/trace_events.o
  CC      kernel/events/core.o
  CC      block/disk-events.o
mm/corten_arena.c: In function ‘corten_arena_lookup’:
mm/corten_arena.c:554:65: error: macro "RCU_LOCKDEP_WARN" passed 3 arguments, but takes just 2
  554 |                          "%s() without RCU protection", __func__);
      |                                                                 ^
In file included from ./include/linux/corten.h:243,
                 from mm/corten_arena.c:42:
./include/linux/rcupdate.h:483: note: macro "RCU_LOCKDEP_WARN" defined here
  483 | #define RCU_LOCKDEP_WARN(c, s) do { } while (0 && (c))
      |
mm/corten_arena.c:553:9: error: ‘RCU_LOCKDEP_WARN’ undeclared (first use in this function)
  553 |         RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
      |         ^~~~~~~~~~~~~~~~
mm/corten_arena.c:553:9: note: each undeclared identifier is reported only once for each function it appears in
make[3]: *** [scripts/Makefile.build:291: mm/corten_arena.o] Error 1
make[2]: *** [scripts/Makefile.build:552: mm] Error 2
```

- make 退出码 **2**；`arch/x86/boot/bzImage` **不存在**。
- 失败前全日志 **0 条 warning**（除本错误外其余对象干净，含 mm/mmap.o、kernel/sys.o 等）。

### 根因定性

- 本树 `include/linux/rcupdate.h` 两个分支的宏均为**固定 2 参** `(c, s)`:
  - PROVE_RCU=y 分支（行 395 起，commit 3066820034b5dd 语义）: `#define RCU_LOCKDEP_WARN(c, s)`
  - PROVE_RCU=n 分支（行 483）: `#define RCU_LOCKDEP_WARN(c, s) do { } while (0 && (c))`
- 调用点 `mm/corten_arena.c:553-554` 传了 **3 个实参**（多了 `__func__`）:
  ```c
  RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
                   "%s() without RCU protection", __func__);
  ```
- 与配置无关: 两个分支都是 2 参，任何编译该文件的配置（即 CORTEN_MM_ARENA=y 的所有配置）必炸。
- S1-S3 改动集中 grep 全量: `RCU_LOCKDEP_WARN` 仅此一处调用（corten.c/corten.h/mmap.c/vma.c/
  fork.c/sys.c/userfaultfd 等均无），修一处即解除构建阻塞。
- 修法提示（供 dev agent 参考，验证 agent 未实施）: 改为 2 参形态，如
  `RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "corten_arena_lookup() without RCU protection")`；
  建议修后全树 grep 一遍新增代码里类似 vararg 习惯用法。

## 步骤 2: 无盘 qemu KUnit ×2 — **BLOCKED（未执行）**

- 原因: 步骤 1 失败无 bzImage，无法启动 qemu。未跑任何 KUnit，无证据可采。
- **给主 agent 的补充情报**: 项目 config 里 `CONFIG_CORTEN_MM_ARENA_KUNIT_TEST` **=n**
  （其 `default KUNIT_ALL_TESTS`，本 config `# CONFIG_KUNIT_ALL_TESTS is not set`，
  `CONFIG_KUNIT=y`）。即使修好构建，按现状 `filter_glob=corten` 只会出 corten 套件 25 例，
  **8 个 corten_arena 新 case 不会运行**。KUnit 取证前需
  `scripts/config -e CORTEN_MM_ARENA_KUNIT_TEST && make olddefconfig`。

## 步骤 3: =n 链接回归 — **PASS（有保留意见，见注）**

### 3a. 按"禁令清单"配置（CORTEN_MM/ARENA/KUNIT_TEST 全 -d；项目 config 本身 `# CONFIG_USERFAULTFD is not set`）

命令: `cp .config /tmp/s123-y.config; scripts/config -d CORTEN_MM -d CORTEN_MM_ARENA
-d CORTEN_MM_KUNIT_TEST && make olddefconfig && make -j6 kernel/sys.o mm/mmap.o
mm/memory.o mm/userfaultfd.o`，日志 `results/r02/s123-nbuild.log`。

- `kernel/sys.o`、`mm/mmap.o`、`mm/memory.o`: **全部编译成功，零 warning**（唯一 error
  均来自 userfaultfd，见下）。
- `mm/userfaultfd.o`: 编译失败——但**与 S1-S3 无关，属基线树既有状态**，证据链:
  1. `mm/userfaultfd.c` 与 `include/linux/userfaultfd_k.h` 相对 HEAD=1284a235 **零改动**
     （git status 干净，不在 S1-S3 diff 的 17 个路径中）；
  2. 该文件**没有任何 `CONFIG_USERFAULTFD` 守卫**（grep 零命中），文件头即裸用
     `vm_userfaultfd_ctx.ctx`，仅在 USERFAULTFD=y 下可编译；
  3. 项目 config USERFAULTFD=n → `obj-$(CONFIG_USERFAULTFD)` 把它排除在一切正常构建外
     （=y 全量构建日志中 userfaultfd 出现次数=0）；直接点名编译该对象属构建面之外。
  首个错误样本（s123-nbuild.log）:
  ```
  mm/userfaultfd.c:35:41: error: ‘struct vm_userfaultfd_ctx’ has no member named ‘ctx’
  mm/userfaultfd.c:171:52: error: unknown type name ‘uffd_flags_t’; did you mean ‘fop_flags_t’?
  ```

### 3b. 对照组: USERFAULTFD=y + CORTEN_MM=n（纯 config 操作，补钉 userfaultfd 维度）

命令: `scripts/config -e USERFAULTFD && make olddefconfig && make -j6 kernel/sys.o
mm/mmap.o mm/memory.o mm/userfaultfd.o fs/userfaultfd.o`（fs/userfaultfd.c 是 S1-S3
实际改动的 userfaultfd 路径），日志 `results/r02/s123-nbuild-uffd-y.log`:

```
exit=0
warning/error 计数: 0
  CC      fs/userfaultfd.o
  CC      kernel/sys.o
  CC      mm/mmap.o
  CC      mm/memory.o
  CC      mm/userfaultfd.o
```

**结论**: 在 userfaultfd 真实参与编译的配置下，=n 回归对全部 diff 涉及对象（含
fs/userfaultfd.o 与 mm/userfaultfd.o）**零警告通过** → review P0（=n 桩/链接面）的实际
回归信号为 PASS。

### 3c. 配置恢复

`cp /tmp/s123-y.config .config && make olddefconfig`，恢复后与验证前一致:

```
CONFIG_CORTEN_MM=y
CONFIG_CORTEN_MM_KUNIT_TEST=y
CONFIG_CORTEN_MM_ARENA=y
# CONFIG_CORTEN_MM_ARENA_KUNIT_TEST is not set
# CONFIG_USERFAULTFD is not set
```

---

## 汇总

| 项 | 结果 |
|---|---|
| S1. =y 全量构建 + ARENA=y 确认 | **FAIL**（corten_arena.c:553 宏 3 参 vs 2 参；确定性，与配置无关） |
| S2. 无盘 qemu KUnit ×2 | BLOCKED（无 bzImage；另发现 ARENA_KUNIT_TEST=n 会让新 case 不跑） |
| S3. =n 链接回归 | PASS（sys/mmap/memory 零警告；userfaultfd 维度经 USERFAULTFD=y 对照零警告；基线 USERFAULTFD=n 下 mm/userfaultfd.c 不可直编属基线既有，非 S1-S3 引入） |
| 纪律 | 未 commit / 未动代码 / 未碰主树与 m3b46 / config 已恢复 / 无密码落盘 |

**下一步（主 agent 决策）**: 将失败单发回 dev agent（单点: mm/corten_arena.c:553-554
改为 2 参调用；修后重建跑 S1-S3 全序列，KUnit 前记得 -e CORTEN_MM_ARENA_KUNIT_TEST）。
S46 agent 请以 `S123_VERIFY_FAILED` 为准，不要消费本切片的 DONE 信号。
