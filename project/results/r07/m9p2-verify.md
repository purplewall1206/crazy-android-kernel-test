# M9-P2: arm64 生命周期钩子补全 — 验证报告 (m9p2-verify.md)

- 班次: r07 m9p2 (2026-09-21 上午 CST)
- 树: /home/ppw/linux-6.18-m9, 分支 m9-arm64, **已同步基座**:
  `git rebase --autostash 2639d3294b9d`（M9-P1 的未提交改动与已入树的
  025756094542 内容一致，autostash 三方合并后零残差，工作树 = 2639d3294b9d +
  本班补丁）。注意: 基座推进含 M3b 全量 + M4.T0 + M5 + M6（M9-P1 提交
  025756094542 之后树上又落了 11 个 CortenMM 提交），**本班是该 M6 基座上
  arm64 首次构建/运行验证**。
- 交付: 工作树补丁（未 commit）= patches/r07-m9p2.diff；
  ARM64_PORTING.md §6/§8/§10-OQ4/§11.4 已更新（r3）。
- 主树 / 其它 worktree 未触碰; 未 push。

## 1. 覆盖判定（钩子落点的代码级核实）

按任务要求"先核实 asm-generic 是否已天然覆盖"，逐漏斗读码结论:

| 漏斗 | arm64 路径（6.18 实码） | M9-P2 前 | 判定 |
|---|---|---|---|
| alloc（用户 PT 页） | `pte_alloc_one` → asm-generic `__pte_alloc_one_noprof`（pgalloc.h:75；arm64 pgalloc.h 未定义 `__HAVE_ARCH_PTE_ALLOC_ONE`，直接继承） | **无钩子** | 需加码: 按 OQ4 定案把 `corten_on_pte_alloc()` 放进 `__pte_alloc_one_noprof()`（:94） |
| free，同步 | `pte_free()`（asm-generic pgalloc.h） | **已有钩子**（M2a，现 :136） | 天然覆盖，零改动 |
| free，TLB-batch | `__pte_free_tlb`（arch/arm64/include/asm/tlb.h，无 `___pte_free_tlb` 间接层）→ `tlb_remove_ptdesc` → `tlb_remove_table` → 通用 `__tlb_remove_table()` → `pagetable_dtor_free()`（asm-generic/tlb.h:217-222） | **无钩子** | 需加码: batch 路径**不经过** `pte_free()`，与 x86 需要 `___pte_free_tlb` 同理，一行补上（asm/tlb.h:93） |

补丁 3 文件 +29/-13（checkpatch --strict 0E/0W/0C）:

1. `include/asm-generic/pgalloc.h`: `__pte_alloc_one_noprof()` 成功路径加
   `corten_on_pte_alloc(mm, ptdesc_page(ptdesc))`（两个失败返回点不加钩子，
   与 x86 语义一致: 页未出生不装描述符）。
2. `arch/x86/mm/pgtable.c`: **删除** x86 `pte_alloc_one()` 里的 M2a 钩子调用
   （连带注释）。这是 OQ4-(a) 落地的必要配对改动，r2 文本未预见:
   x86 的 `pte_alloc_one()` 通过 `__pte_alloc_one()` 包装进
   `__pte_alloc_one_noprof`，钩子放进 asm-generic 后若保留 x86 侧调用则
   每次用户 PTE 分配**双重 install**——而 `corten_ptdesc_install()` 对已
   tracked pfn 的二次 install 会走 replace + `WARN_ON_ONCE`
   （mm/corten.c:356），x86 从此每次分配告警。删除后 x86 与 arm64 共用
   同一落点，install 恰一次；x86 侧净 diff 为纯删除。
3. `arch/arm64/include/asm/tlb.h`: `__pte_free_tlb()` 在 `tlb_remove_ptdesc`
   前加 `corton_on_pte_free(pte)`（注释对齐 x86 `___pte_free_tlb` 的顺序
   论证）+ `#include <linux/corten.h>`（:12，显式依赖，不靠 include 链
   巧合）。

覆盖闭环（M9-P2 后）: arm64 每个 PT 页的生/死必经一钩——生:
`pte_alloc_one*` → :94；死: 同步路径 `pte_free()` → :136，批量路径
`__pte_free_tlb` → asm/tlb.h:93。与 x86 双漏斗结构同构。

## 2. 验证矩阵

| Gate | 结果 |
|---|---|
| `make ARCH=arm64 CROSS_COMPILE=$TC olddefconfig` | PASS（基座已含 P1 门控: `CORTEN_MM=y`、`CORTEN_MM_KUNIT_TEST=y`、`KUNIT=y`、`ARM64_4K_PAGES=y`、`PAGE_SHIFT=12`、`PGTABLE_LEVELS=5`; `CORTEN_MM_ARENA` 在 arm64 正确缺席[depends X86_64]） |
| `make -j6 mm/corten.o mm/corten_test.o` | PASS，RC=0，零错误零警告（含新钩子头后重编） |
| `make -j6 Image`（增量，rebase 后近全量重编） | PASS，RC=0; 日志 4,729 行 0E/0W（4 处 "error" 命中均为文件名: uterror/utxferror/9p error/scsi_error.o） |
| Image 产物 | 42,232,320 B，sha256 `1aa6b167337c5010f4d51824f130bdc5a49a33b95e5d694e16040d1c6d946562`（与 M9-P1 Image 同字节数、较 Round A 基线 +200,704 B——M4-M6 共享层增量与 arena 代码在 arm64 被裁相抵后与 P1 口径持平） |
| `qemu-system-aarch64 -M virt -cpu max -smp 4 -m 1024 … kunit.filter_glob=corten*` | **7 boot: 4× 25/0/0 全绿**（run3/4/5/6，见 m9p2-kunit-smp4.log 与 run2/green6/green7 各 log; run2=24/1、green6=25/0/0 文件名见 §3）+ 3× 24/1 = interlock TCG 时钟伪影（§3，非协议/非本补丁回归） |
| 绿臂 WARNING 剖面 | 恰 2 条 WARNING（mm/corten.c:865/:812，`corten_txn_begin`，`txn_path_overflow` KUnit 注入设计路径）= 与 r03/M9-P1 口径一致; 零 Oops/零 BUG; 末段 `VFS: Unable to mount root` panic = 无根 fs 预期终止 |
| x86 回归（同补丁触碰 x86 文件 + 共享头） | 见 §4 |

对照 x86 的 25/25: arm64 绿臂 **25/0/0 与 x86 corten 套件同量全绿**（arm64
无 arena 三套件，`CORTEN_MM_ARENA depends on CORTEN_MM && X86_64` 按设计裁剪）。

## 3. interlock 测试 arm64 TCG 伪影（诚实记录，非本补丁回归）

- 现象: 7 boot 中 3 次 `corten_test_txn_uninstall_interlock` 24/1（run1/2、
  green7），其余 4 次 25/0/0。同一二进制同命令，仅宿主负载时序不同。
- 两种失败签名，均为**测试的时间脚手架被 TCG 时钟异常打破**，而非 interlock
  协议被破坏:
  - run1/2（runtime 20.25s，恰 worker A 的 20s deadline）: `a_err==1`、
    `a_locked==0`、`violations==1`——A 的 20s 轮询 deadline 先到期、正常
    放锁（a_err++），主线程标称 200ms 的检查窗口被拉长到 ~20s，之后才读到
    `b_done==1`。B 的 uninstall 是在 A **合法放锁之后**才完成的——协议
    （uninstall 在 A 持锁期阻塞）工作正常，"violations" 读数是检查窗口塌缩
    的伪读数。
  - green7（`worker A never acquired (phase=5 begin_ret=0)`，30.0s）:
    `begin_ret=0` 证明锁真实拿到过，`phase=5`=A 已走完整个生命周期;
    guest ktime 跳变使 A 的 20s deadline 起跑即过期、毫秒级穿场
    （a_locked=1→0 快于主线程 10s 轮询间隔），主线程"没看见"持锁窗口。
- 归属论证: ① 失败路径全部是 wall-clock 逻辑（20s deadline / 10s / 200ms
  窗口），interlock 机制本身（desc write_lock_bh 阻塞 uninstall）零证据
  异常; ② e911b31..2639d3294b9d 之间 `corten_ptdesc_uninstall`/
  `corten_txn_begin` 持锁路径与该测试代码**字节不变**（git diff 核实，
  变更仅 debugfs 重构/rearm 计数器/unmap 加 flags 参数）; ③ 本班补丁的
  钩子是 static-branch 关闭态（boot 不带 corten=on）且**不在测试路径上**
  （测试直接调 `corten_ptdesc_uninstall`，绕过漏斗）; ④ x86 KVM 同基座
  同测试绿（r07 历史 + 本班 §4 复跑）。
- **既有登记（M7）**: 该 flake 在 x86 侧已有登记——run/inflight.txt
  "宿主过载 interlock flake 2 例=M7 登记"、"遗留口径采信: interlock 3/6
  vs 基线 1/4 (维持 M7 登记)"。本班 arm64 TCG 3/7 与该登记率同量级，
  属**同一已知项的跨架构再现**，非新回归、非 arm64 特有。
- 定性: interlock 测试对宿主负载/时钟敏感的固有脆弱性（M7 已登记项），
  M9-P1 的单次 25/0/0 未采样到。建议（后续班次，不属本班最小补丁）:
  测试加固（go-信号替代 deadline 轮询）或判定口径改为 "TCG 3 取 2 多数决
  + KVM 复核"——与 M7 登记的"时间窗加宽"方向一致。

## 4. x86 回归（补丁触碰 arch/x86/mm/pgtable.c + 共享 asm-generic 头）

- 过程发现（基座遗留，非本补丁引入，不属本班修复范围）: 首次以
  `x86_64 defconfig` 构建在 `mm/corten_arena.c` 两处编译失败——:3323
  `struct shrinker has no member named 'id'`（缺 `CONFIG_SHRINKER_DEBUG`）
  与 :8530 `implicit declaration of 'mem_cgroup_is_descendant'`（缺
  `CONFIG_MEMCG`）。即 M6.T3/T4 的 memcg-shrinker 代码存在未加 Kconfig
  依赖守护的配置面，历史 x86 口径（gki 系配置 MEMCG=y）从未暴露。
  处置: 验证配置补开 `-e MEMCG -e SHRINKER_DEBUG`（不动 arena 代码/
  Kconfig，留 maintainer 评估是否补 `depends on` 或 `#ifdef` 守护）。
- `make ARCH=x86_64 -j6 bzImage` → PASS（RC=0, 0 error; 仅 2 条既有基线
  警告: objtool cpuidle + modpost memblock, 与 r05-m4t0a 登记口径一致,
  零新增）→ KVM qemu `-enable-kvm -m 2048 -smp 4 … kunit.filter_glob=corten*`
  （m9p2-x86-kunit.log）。
- **结果: corten 套件 25/0/0 全绿**，含 `ok 19 corten_test_txn_uninstall_interlock`
  （同基座同测试在 KVM 下绿，反证 §3 的 TCG 时钟定性）; WARNING 恰 2 条
  （设计注入路径）。x86 侧 alloc 钩子迁移（纯删除 + asm-generic 共用落点）
  端到端验证成立——这就是"对照 x86 的 25/25"。
- 口径说明: 本班 x86 用 defconfig 系配置，未开 arena KUnit 测试套
  （`CORTEN_MM_ARENA_*_KUNIT_TEST` default KUNIT_ALL_TESTS, defconfig=n），
  故只有 corten 核心套件 25 条; 历史 24+19+13 多套件口径属 gki 配置班次。

## 5. 判定

**M9-P2: PASS（含一项环境级遗留）。** OQ4 按定案落地且发现并处理了 r2
未预见的 x86 双重-install 配对问题; arm64 侧恰为一行钩子 + 一行 include;
双对象 0E/0W、CORTEN_MM=y Image 0E/0W、KUnit 绿臂 25/0/0 与 x86 同量;
interlock 的 TCG 时钟伪影已定性（§3）并记录到 ARM64_PORTING §11.5。
P3（contpte 主体）不在本班范围、风险未动。

## 6. 移交 / 遗留

- 未 commit（review/maintainer 后续）; 未 push; 主树/其它 worktree 未触碰。
- 遗留 1: debugfs 计数器 sanity（P2 exit criterion 后半）需 corten=on
  用户态冒烟 → P4。
- 遗留 2: interlock 测试 TCG 脆弱性加固（§3 建议）→ 与测试 owner 另班处理。
- 遗留 3: 16K/64K 编译矩阵（§7/R5）不变，仍按需排期。
- 本班证据文件（本目录）: m9p2-worktree.diff（=patches/r07-m9p2.diff）、
  m9p2-image-build.log、m9p2-kunit-smp4.log（run1, 24/1 伪影）、
  m9p2-kunit-smp4-run2.log（24/1 伪影）、m9p2-kunit-smp4-run3/4/5.log
  （25/0/0）、m9p2-kunit-smp4-green6.log（25/0/0）、
  m9p2-kunit-smp4-green7.log（24/1 伪影, phase=5 签名）、
  m9p2-x86-build.log、m9p2-x86-kunit.log。
