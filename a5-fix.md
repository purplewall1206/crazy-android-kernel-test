# A5 修复: arena registry 惰性化（G5 门根因修复, M4 最后缺口）—— r07 通宵班收口

- 班次: 2026-09-21 03:56–07:00 CST（timegate 全程放行; 宿主邻 VM SIGSTOP 降噪, 收班恢复）。
- 内核: 主树 **commit `b9541335a554`（HEAD, tag `corten-r07-a5`）**, 构建 #91
  `6.18.32-g2639d3294b9d-dirty`, bzimg 归档 `bzimg/r07-a5/`（sha256 `ad9040f6…c038a`）。
- 前序: `results/r07/g5-gate/g5-gate.md`（根因已定案）。VM: 复用 vm-t5final（port 10026,
  trixie-m4t12.img, 8 vCPU/4G/KVM）换 A5 内核重启; `corten=on mitigations=off kunit.enable=0`。

## 1. 修法（4 文件, +122/-45; commit b9541335a554）

根因链 = ENTER 急切建 registry（state_create: kzalloc+双 percpu+registry join）→ fork_begin
急切复制子 registry → **exit_mmap 每 MODE mm 遥拆付一次 list_del_rcu + synchronize_rcu**。
T0a 已把 mode 位放在 `mm_struct`（`corten_mode`, mm_types.h 注释本来就预言了 registry-free
ENTER）, 因此 state 可以为 NULL——惰性化不需要空壳:

1. **惰性 ENTER**（corten_arena.c `corten_arena_mode_enter`）: 只置位, 不再 get_state/-ENOMEM。
   registry 由第一个 arena 工作按需建（auto_mmap_route 的 get_state / DECLARE / fork 镜像）。
   配套: route 建 registry 失败 → `corten_arena_auto_fallback` 降级 legacy（计数）, 不再把
   -ENOMEM 抛给 mmap; `corten_arena_auto_attach` 也走 get_state 自给自足。
2. **惰性 fork**（`corten_arena_fork_begin`）: 仅当父有活 arena（`refcount_read(&old_state->nr)`)
   才建子 registry 并拷贝 cursor; parked 池 arena 不算（fork_begin 池冲洗先行, 无遗留到子）。
   无 arena 的 MODE 子只继承 mode 位, 与父一样 registry-free。
3. **空 state 廉价拆卸**（`corten_arena_mm_exit`）: 入口采样 `had_arenas = nr || nr_pool`
   （池簿记在 drain 前被复位, 故先采样）。registry 空（从未有 arena/池）→ unlink 后
   **call_rcu(corten_arena_state_free_rcu) 异步释放**（新增 `corten_mm_state.rcu`）, 濒死进程
   不再阻塞在 synchronize_rcu; 有 arena 的 exit 保持同步路径不变。宽限期语义不变（in-flight
   shrinker reader 安全）, 变化的只是"谁站着等"。
4. **KUnit 锚更新到惰性契约**: test_mode（ENTER 后 state==NULL）、auto_route（ENTER 建空
   registry→改为拒绝臂不建/命中臂懒建）、auto_attach_release 注释、pool_fork（池冲洗后子
   query==-ENOENT）。INV7/fork_swapped/pool 计数锚全部原样保持并通过。

## 2. 验证（host）

| 项 | 结果 |
|---|---|
| =y 全量构建（#91） | RC=0, **零新增警告**（仅基线 objtool cpuidle; 增量构建不触发 modpost memblock 基线条） |
| KUnit corten* on×2 | **全绿 ×2**: corten 24/0/1 + corten_arena 48/0/0 + corten_fault 30/0/2（a5fix/kunit-on{1,2}-full.log） |
| KUnit corten* off×1 | **全绿**: 25/0/0 + 18/0/30(skip 设计性) + 6/0/26（kunit-off1-full-r2.log, 与 m6t34 off 轮廓一致） |
| =n 八对象 | memory/mmap/migrate/rmap/swapfile/gup/oom_kill/x86-fault RC=0 零警告零 corten 符号（build-n.log）; =y 配置已恢复 |
| checkpatch --strict | file 模式 4 文件: **0 新增 E/W**（corten_arena.c 余 1W=基线既有 braces 条@4027; mm_types.h 全部既有 typedef/注释条）; commit patch 模式 2W=标题 76 字符（任务规定标题, 不可改）+ git show 缩进伪影 |
| KUnit 干扰说明 | 当日 3 次 `txn_uninstall_interlock` fail 均为**已登记 harness 时序 flake**（两种签名: "a_locked/violations/a_err"=on7-fix2 同款; "worker A never acquired (begin_ret=0, phase=5)"=m7-preheat 同款, 该用例写死 10s/20s 窗; r06 gupfix/r07 kunit-off1 等前 A5 日志同址同签名）, 静音宿主后复跑全绿 |

## 3. Guest 判定实验（同 boot BASE vs MODE）

### 3.1 隔离实验 /bin/true ×20（修复目标: hook 臂回到 plain 同级）

| 臂 | r1 | r2 | r3 | 中位 |
|---|---|---|---|---|
| plain | 168ms | 192ms | 163ms | **168ms** |
| MODE hook | 178ms | 208ms | 170ms | **178ms（+6.0%）** |

修复前同口径: plain 135ms vs hook **1135ms（+741%, +50ms/生命周期）**。**每-MODE-进程退出
GP 已消失**（残余 +6% ≈ LD_PRELOAD 多载一库的固有成本）。

### 3.2 lat_proc 三件套 ×3 中位（G5 门: MODE 回退 ≤30%）—— **MET**

真 no-probe STRICT hook（逐腿 marker 断言 + 逐腿零 fork-probe 行校验, clean-gate-stderr/）,
3 rep 臂级交错, 单位 µs:

| op | BASE 3 rep | 中位 | MODE 3 rep | 中位 | **Δ** | 修复前（g5-gate run2） |
|---|---|---|---|---|---|---|
| fork | 1849.6 / 1629.5 / 1419.1 | **1629.5** | 1768.6 / 1624.8 / 1329.0 | **1624.8** | **-0.3%** | +516.0% |
| fork+exec | 5009.5 / 3724.3 / 3720.1 | **3724.3** | 3995.0 / 4265.3 / 4357.6 | **4265.3** | **+14.5%** | +171.7% |
| shell | 7928.1 / 8686.4 / 8404.5 | **8404.5** | 7011.1 / 10744.5 / 9476.8 | **9476.8** | **+12.8%** | +353.5% |

**三 op 全部 ≤30% → G5 门 MET**（fork op 与 BASE 同级; 原 +50ms/lifecycle 定价消失）。

过程披露: ①首轮 battery 的 g5/isol MODE 臂误用了 share 上**名不副实的
corten_mode_hook_noprobe.c**（实含 atexit fork-probe, 每 fork 子继承, 每腿 438 行
probe 行 = g5-gate run1 同款污染）, fork 臂读数 2868-3539µs 判废; 诊断后以真
no-probe hook 重跑（本节数字为重跑件; battery 的 exec +3.7%/shell +29.1% 即便带污染
也在门内, 交叉印证）。判废件保留在 guest/a5fix/raw/。②battery 里 exec/shell 首轮
（含污染）与本节干净重跑的中位数同向同量级, fork 的 -0.3% 与探针 P1-P4
（dummy.so 1304 ≈ base 1297; MODE-on 1540 vs base 1297=+19%）一致。

### 3.3 回归（同 boot, ×1/臂）

| 项 | BASE | MODE | 判定 |
|---|---|---|---|
| dedup_eq glibc（8 线程/6s） | 4.00M blocks/s | 4.18M（**+4.3%**） | rc=0, 池路径无回归 |
| dedup_eq tcmalloc | 4.14M blocks/s | 4.68M（**+13.1%**） | rc=0（T0 臂 preload hook+tcmalloc 双载） |
| JThreadBench 2000×3 | 3450.9ms | **2699.4ms（-21.8%）** | 双臂 rc=0, **0 ClassFormatError**, fork-probe OK |
| metis_eq 8 线程 | 0.313s, checksum `2d383eeed4ceb73b` | 0.216s, checksum **同值** | rc=0 双臂 + **fork-probe OK** + fork_faithful 3→4（忠实 fork 仍走） |
| run_mode_smoke | — | **24/26 + 同款栈粉碎中止 rc=134** | 与 m6t34-guest/smoke.log **逐行一致**（released-arena-unmapped/gone 2 例 + binary 自身 abort = 9p 上 T1c 前旧 binary 的既有伪影, 非 A5 回归）; T1c 契约件 26/26 复跑见 §3.4 |
| debugfs evict 对账 | arena_stress 4 线程 128MB 混合 | evict 20000 页 rc=0, arena_stress rc=0, errors=0/op_errors=0/verify_fail=0, swapped_out +20000 | 池/evict 路径无回归 |

§3.4 收尾补跑（guest 内直接执行, 数据在 battery-progress.log 尾段与本班 ssh 日志转录）:
T1c 契约 smoke binary（`bench/mode-smoke/corten_mode_smoke`, 9-19 构建, 静态件经 9p 入
guest）= **26/26 PASS + SMOKE PASS + SMOKE-DRIVER PASS（连跑两遍全同）**, debugfs arenas
0→0; registry 走查探针: arena_stress 4 线程 64MB 混合存活期间 `echo 3 > drop_caches` 触发
shrink_slab → **shrink_scans 3416→7207（+3791）, aging_passes=5032** = registry 遍历在 A5
后照常枚举带 arena 的 MODE state（evict 直写按 pid 定向不计数, 属既有口径, 本探针补齐
"shrinker 走查仍见 registry"的 guest 端证据; KUnit 侧 shrink_registry_count 锚同证）。

## 4. 登记

- `[已修复]` G5 fork/shell 回退根因（g5-gate.md §2 登记的修复方向 ①+②+③全落地）。
- `[环境]` share `g5gate/corten_mode_hook_noprobe.c` 名不副实（含 atexit probe）——后续
  班次以 `share/a5fix/corten_mode_hook_true_noprobe.c` 为准（干净版已留档）。
- `[既有]` interlock KUnit 时序 flake（两种签名, 写死窗口偏紧, m7-preheat 建议改 ktime 预算）。
- `[既有]` 9p smoke binary 过期（T1c 前契约, 栈粉碎 abort）; T1c 契约件 26/26 在档。
- `[观察]` battery g5 腿（boot 后 ~1-12min 窗口）base 臂读数整体偏高（fork 1545 vs 静置
  1297-1419）, 与污染无关的宿主/启动期效应; 判定以静置重跑件为准。

## 5. 证据索引

- 结果: `results/r07/a5fix/`（=y 构建日志、KUnit on×2/off、=n、checkpatch）、
  `results/r07/a5fix/guest/`（battery progress + raw + g5_summary + arena_stats 快照 +
  clean-gate-stderr/ 9 件零污染凭据）。
- 归档: `bzimg/r07-a5/{bzImage,SHA256SUMS}`（ad9040f6…）; green.txt 本条; tag `corten-r07-a5`。
- VM 终态: vm-t5final = A5 内核（#91, b9541335a554）corten=on 留运行。

—— A5 收工: **G5 门 MET（fork -0.3% / exec +14.5% / shell +12.8%, 修复前
+516/+172/+354）; 隔离实验 +741%→+6%; 全套 KUnit on×2/off×1 绿; =n 零瑕疵;
dedup/JTB/metis/smoke(T1c 契约 26/26) 无回归; 忠实 fork fork_faithful 仍前进。
主树 commit b9541335a554, tag corten-r07-a5。M4 最后缺口闭合。**
