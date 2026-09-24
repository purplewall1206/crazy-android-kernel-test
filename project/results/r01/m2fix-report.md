# m2fix report — corten=on 启动挂死修复 + M2b review 5 项
日期: 2026-09-13 08:1x CST · agent: hotfix (m2c-fix1) · 时间盒内完成 (08:12 全部收口)
HEAD: 2fd4070e745c → **1284a235f751** (commit `mm: CortenMM: defer static key enable to initcall; apply review fixes`)
tag `corten-r01-m2c-fix1` · patch `patches/0001-mm-CortenMM-defer-static-key-enable-to-initcall-appl.patch` (checkpatch 0/0)
bzImage → `bzimg/r01-m2c-fix1`, sha256 `6871103f680487d82a121f16868c0a72edd066d8398f92dcf68a648de40652c3`

## VERDICT: PASS

## 1. 根因
`corten=on` 的 `__setup("corten=")` 回调在参数解析期直接 `static_branch_enable(&corten_enabled_key)`。
时序证据（init/main.c，本树行号）：
- `:1057 jump_label_init()`（表构建，早于参数解析——所以"jump label 未初始化"这一首要假设**不成立**）；
- `:1071 parse_args("Booting kernel")` → `unknown_bootoption()` `:568 obsolete_checksetup()` ——
  `__setup` 回调在此触发，此时 **早于** `:1094 mm_core_init()`、`:1096 poking_init()`、SMP bring-up 与
  `local_irq_enable()`；
- `static_branch_enable()` 走的是**运行期** jump-label patching 路径（text poking / 跨核同步），
  该路径在早期 boot 未就绪（poking_init 未跑、IRQ 关、SMP 未起）→ 每次都在解析期进入小环挂死，
  串口零输出（console 也尚未初始化），与 r01/m2-on-panic.log 的 2/2 复现完全吻合。
- 次要假设（翻转过早导致 pte_free 钩子对 early boot 生效）同为真实风险，同一修法覆盖。

## 2. 修法 (mm/corten.c)
- 回调只置普通变量 `static bool corten_param_on`；
- 新增 `corten_late_init()`（`early_initcall`；initcalls 经 `do_basic_setup()`/`do_initcalls()`
  init/main.c:1471-1505 运行，严格晚于 jump_label_init 与分配器就绪）：才执行 `static_branch_enable()`；
- 注释写明为何不能在 early param 里翻转（引用行号），debugfs `late_initcall` 注明安全
  （debugfs_init 为 core_initcall, fs/debugfs/inode.c:945），无同类隐患；
- `corten_enabled_static()` 语义未动；4 文件外零改动（git diff --stat 确认）。

## 3. Review 5 项（M2b）
1. corten.c L7/L8 与 path 释放处"冻结"注释重写：real view path 长 ≤1，层级稳定来自调用方
   mmap_lock(read) 契约而非 path 锁；path 锁冻结构造变化仅对多级树视图成立且依赖视图自身契约。✓
2. include/linux/corten.h COW 措辞更正：write fault on (shared && !writable) 走 COW 复制/FOLL_FORCE
   分支（fork 前本就只读），非"re-enables read-only"；corten.c mark() 处注释同步对齐。✓
3. `corten_unmap()` 归零 `__resv`（memset），防脏载荷复活。✓
4. mm/Kconfig CORTEN_MM help 更新为 M2b（协议+事务 API 在，M3 前无真实调用方）。✓
5. corten_test.c 并发加固：in_crit 窗口覆盖整个事务体。**实现偏离 review 原文的字面位置**——
   dec 放 `corten_unlock()` 之后会与前驱/后继竞态（后继在 write_unlock 释放瞬间即可 begin+inc，
   overlap 用例立即 EXPECTATION FAILED ×2 实测复现）；改为 dec 在 ops 之后、unlock 之前（仍持锁），
   窗口语义满足 review 意图。kthread 亲和性：返回值检查 + <3 online CPU 或 set_cpus_allowed_ptr
   失败即 kthread_stop 后 `kunit_skip`（helper 改返回 bool，两个调用方处理）。✓

## 4. 验证证据（顺序执行）
1. `make -j12 mm/corten.o mm/corten_test.o` 零警告（m2fix-build-obj.log）；全量 make 80s，
   仅 2 条既有无关警告（cpuidle objtool / memblock EXPORT_SYMBOL，见 m2fix-build-full.log）。✓
2. KUnit 无盘直启（timeout 150 兜底）：`corten: pass:16 fail:0 skip:0 total:16`（m2fix-kunit-boot.log）。
   第一轮 overlap 失败暴露上述 dec 竞态 → 修正 → 16/16。✓
3. corten=off 冒烟：重启 VM → 登录 → uname `6.18.32-g2fd4070e745c-dirty` → 9p 挂载 →
   `mmbench mmap-pf low 2 1 42` 8235 ops 正常输出。✓
4. corten=on 冒烟：relaunch append `... corten=on` → **到达登录提示**（此前 2/2 挂死）；
   dmesg: `0.346s corten: requested on, activating at initcall time` + `1.890s corten: page descriptors
   enabled`（两阶段生效可见）；debugfs: enabled=1, ptdescs=341, meta_arrays=342, txn active_max=2；
   `/mnt/mmbench pf high 4 2 7` 跑通零 panic（137737 ops，m2fix-on-console.log 留档）。✓
5. VERDICT=PASS（未触发挂死回退分支）。

## 5. 时间线（CST）
- 07:55 开工：读 STATE/corten.c/corten.h/corten_test.c/Kconfig，核对 init/main.c 行号
- 07:58 任务1+2 编辑完成 → 单文件零警告 → 全量 80s bzImage
- 07:59 KUnit #1：15/16（overlap 失败暴露 dec-after-unlock 竞态）
- 08:00 修正 dec 位置 → KUnit #2：**16/16**
- 08:05 corten=off 冒烟 PASS → 08:07 corten=on 重启 → **08:08 到登录** → debugfs+mmbench 全过
- 08:10 commit 1284a235f751 + patch + tag + bzimg 归档 + checkpatch 0/0
- 08:12 报告落盘；VM 留在 corten=on 最后验证成功状态运行

## 6. 备注
- 全程未使用 root 密码、未 push、未动 4 个 corten 文件之外的代码。
- corten=on 下 KUnit layout 用例按设计 SKIP（"corten=on boot"），属预期。
- 遗留观察（不阻塞）：debugfs `desc_alloc_fail=1 / meta_alloc_fail=1 / free_untracked=3` 为
  corten=on 启动初期（KUnit fail_alloc 注入 + init 期 untracked 页）的正常计数，M3 接入真实
  调用方时再复核。
