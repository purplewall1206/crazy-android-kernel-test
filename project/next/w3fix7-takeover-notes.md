# w3fix7 接手验收笔记（takeover session, 2026-10-08）

接手现场: worktree /home/ppw/linux-6.18-mva 分支 mv-a0, HEAD 6f42d5daeadc
"WIP: w3fix7 relay takeover snapshot"。前会话保护性快照 5 个 mm 文件;
上一接手会话（10-08 00:16-00:31 活动痕迹)已完成 =n 零符号检查并恢复 .config,
但恢复构建未跑完（无完成 bzImage）, 本次接续。

## 门 0: diff 通读（只读, 未动 git 状态）

`git diff HEAD~1 -- mm/` = 5 文件 +737/−73（w3fix6 = d36b3174626d 为父提交）:

1. **mm/corten.c (+96)**: 两处机械。
   - 新增 `corten_ptdesc_tracked()`: 对 corten_ptdesc_xa 的无锁 xa_load
     成员测试（fill 温路径用, 语义=re-arm 前半, 只读不装）。
   - `corten_txn_begin()` L3-before-L4 重排 + L7/L8 塌缩: covering 判定
     （纯 cur->level 算术）先于取锁算出; 命中 covering 直接 write_lock_bh,
     消掉 read_lock→read_unlock→write_lock 往返与 unlock 窗口; stale 判定
     仍在写锁下（与 uninstaller 发布互斥, 保证不变）。候选不再入 txn->path。
     协议计数器（txns/watermark）移到取锁前——advisory-only（debugfs）,
     注释论证了 transient active+1 与 failed-stale 臂自减的正确性。
2. **mm/corten.h (+11)**: corten_ptdesc_tracked() 声明 + 竞态契约注释
   （teardown 中仍可答 tracked → 事务 -EOPNOTSUPP → 慢臂收敛, 与 re-arm 同款）。
3. **mm/corten_arena.c (+138)**: 两处机械。
   - `corten_arena_fill_upper()` 温快路径（ledger #12）: 逐级 READ_ONCE
     锁无顶层走查（pgd→p4d→pud→pmd）, 全 present+非叶且 pmd_page 走
     corten_ptdesc_tracked() 命中 → `goto warm`: 免 fill_lock mutex,
     记 FILLS 后返 0。慢臂原样保留（含 r03 缺陷 C re-arm）。安全论证:
     与既有 fault 臂/real_root 同暴露级, 窗口 teardown 纪律兜底。
   - fault 臂统计搬出覆盖写锁: 各臂只记 `ctx->arm_stat` 桶（初始 -1）,
     fault_once 尾声在全部锁外统一落账（成功臂每尝试恰一个、成功不重试
     → 流水与旧逐臂写完全一致）。涉及 map_anon/zero_page/restore_pte/
     cow_reuse/cow_copy/file_cow 六臂 + 两个入口初始化。
4. **mm/corten_test.c (+167)**: 抽出 `corten_test_real_setup()` fixture;
   新锚 `corten_test_txn_candidate_churn`（200 轮塌缩路径事务, 锁 pin 纪律
   refs==2、rec_lo/hi 精确、quiesce 时 active==0/active_max>=1）。
5. **mm/corten_arena_test.c（新, +398）**: 两个新锚
   `corten_arena_test_fault_stat_parity`（统计流水对账）+
   `corten_arena_test_fault_stampede`（并发踩踏下温路径免锁且 FILLS 流水完整）。

## 门 1: checkpatch + =n 零符号（状态复核）

- 前接手会话 =n 读数已复核（project/results/r07/w3fix7/n-zerolen-takeover.txt,
  2026-10-08 00:16）: =n 构建 rc 完成日志 n-build-takeover.log（bzImage #409
  ready）; **17 个带 corten 消费对象 nm/strings 双零**（gup/madvise/memory/
  mempolicy/migrate/mincore/mlock/mmap_lock/mmap/mprotect/mremap/mseal/msync/
  oom_kill/rmap/swapfile/vma）; 5 个 corten provider 对象 =n 下缺席确认。
- .config 已恢复 =y（CONFIG_CORTEN_MM/KUNIT_TEST/ARENA/ARENA_KUNIT_TEST/
  ARENA_FAULT_KUNIT_TEST 全 y）; 受影响对象已重编（memory.o 00:18 含 9 个
  corten 符号, corten.o/arena.o 00:20）。
- **遗留**: 恢复构建中途断（build-restore-takeover.log 无 bzImage ready,
  树内 bzImage 停留在 =n #409）→ 本次 `make` 续完, 读数见下。

（后续各门读数按完成顺序追加。）

## 门 2: KUnit 三套件复跑 — 全绿（2026-10-08 09:2x, bzImage #410）

- 形态: diskless 直启 `-m 2048 -smp 4`, append `console=ttyS0 panic=-1
  kunit.filter_glob=corten* [corten=on]`, 等 qemu panic 自退（首轮 runner 在
  首 Totals 即杀, 只收到单套件——已修为等退出, 判据=totals 行全三套件）。
- **on×3 逐格一致全绿**: corten **27/0/1**（28, 含新锚 txn_candidate_churn）
  + corten_arena **146/0/0** + corten_fault **34/0/5**; `not ok` 行=0。
- **off×1 全绿**: corten 28/0/0 + corten_arena 28/0/118（skip=需 =on 的锚,
  对账恰 +新 arena 锚族）+ corten_fault 7/0/32。
- **登记 flake 事件**: 首轮 on1/on2（09:17, 宿主 load≈6.7-7.55, 两台外来 VM
  + 主树构建并发）`corten_test_txn_uninstall_interlock` 红 1 例/轮
  （violations 期望 1 计时敏感, runtime 20.3s marked slow）; 负载退潮
  （load≈1.9）后 on1/on2/on3 复跑三连绿。与台账在案 interlock flake 族同形
  （handoff: 原 w3fix7 relay "run1/run2 曾红两锚后被接力修绿"）, 判宿主负载
  计时敏感非代码回归; 前 relay 会话 23:56 三连绿存档
  takeover0-reruns/（同内容证词）。
- 日志: kunit-on1/on2/on3/off1.log（本次 #410 内核, 串 6.18.32-g6f42d5daeadc-dirty）。

## 门 3: mmbench bench-after 腿 + guest smoke/metis/audit（2026-10-08 09:3x-）

- **VM 基座偏差登记**: 任务书指定底图 trixie-mv3d.img, 但主会话 t1-bat VM
  （pid 39925, port 10031, 09:20 起）正**以写锁直用该底图**（10G 镜像 mtime
  活跃推进）——overlay 挂在运动底图上既锁不上也不可复现。改用同族 canonical
  底图 **trixie-w6v2.img**（0 holders, 最新未占用底图, 同 trixie Debian 13
  syzkaller guest）建 overlay /home/ppw/vm/w3fix7-t2.qcow2（底图只读未动,
  偏差可复现）。VM: port 10036, pidfile /home/ppw/vm/qemu-w3fix7-t2.pid,
  tmux w3fix7-t2, -m 4096 -smp 8, append=mv3d COMMON+corten=on。
  BOOT_OK（ssh try 5）, guest uname 6.18.32-g6f42d5daeadc-dirty。
- 驱动: /home/ppw/bench/share/w3fix7-guest.sh（9p 进 guest, kit 落 /root
  再跑——mv3d 教训: 电池不依赖 9p 读）。协议 = w3fix5 §6 同款:
  mmbench_dyn（sha256 须 38304f06 前缀）mmap-pf low t 2 seed; mode 臂
  LD_PRELOAD=corten_mode_hook.so STRICT=1; seed t4=20260951-53/t8=20260979-81
  与 base 腿逐 seed 配对; pool_parks 差分验臂（stock=0, mode≈ops）。

## 门 3 读数（2026-10-08 09:32-09:47）

### bench-after 腿（12 JSON 已落 bench-after/, 内核 6.18.32-g6f42d5daeadc-dirty）

- 驱动核验: mmbench_dyn sha256=38304f062d43…（精确前缀 ✓）; 12/12 rc=0 JSON
  valid; 臂差分: stock 六格 pool_parks 恒 0（真 legacy）, mode 六格 parks
  +121~+10743 且 hits≈parks（park→复用循环活）——MODE 路径实走 ✓。
- 逐格读数 after(takeover #410) vs base（w3fix6, ops/µs）:
  | cell | base | after | Δ |
  |---|---|---|---|
  | stock t4 k1/2/3 | .0013144/.0010778/.0012535 | .0012965/.0014343/.0019425 | −1.4/+33.1/+55.0% |
  | stock t8 k1/2/3 | .0006044/.0006014/.0006518 | .0010250/.0007761/.0013584 | +69.6/+29.1/+108.4% |
  | mode t4 k1/2/3 | .0009849/.0008359/.0007300 | .0013432/.0012714/.0005362 | +36.4/+52.1/−26.5% |
  | mode t8 k1/2/3 | .0002324/.0002522/.0002195 | 7.1e-6/8.9e-6/1.7e-5 | −96.9/−96.5/−92.1% |
  - 中位: stock t4 **+14.4%**, stock t8 +69.6%, mode t4 **+52.1%**, mode t8 −96.2%。
  - 同 boot 税线（after 中位）: t4 stock .0014343 vs mode .0012714 = **−11.4%**
    （历史: w3fix5 −28.0%, w3fix6 −33.3% → 本腿偏正=目标方向）; t8 −99.1%
    （历史 −46.4/−61.5%; mode 格塌缩, 见下）。
- **mode t8 双峰披露（GAP 登记, 非代码门失败）**: 本腿 3 格全落塌缩谷
  （117/147/282 ops）; 同 boot 复探（3s 间隔）弹回 1491/2392/1618;
  原会话同内容腿（bench-after-r1, gd36b317-dirty）k1=624 但 k2/k3=6159/5956
  （其 t8 mode 中位 vs base = **+60.1%**）。同族双峰在原会话即存在。
  归因证据: 探测期间宿主先是安静（load 0.04, 本腿窗口）, 09:36 起
  **perf10-wt bisect `make -j24 bzImage` 风暴**（load 0.04→29.8, 在案
  不可干扰第三方）+ 宿主常驻 3 VM; 风暴中连 stock t8 都 4520→1303 ops
  （ABAB 探测在案）→ t8 格对宿主扰动一阶敏感, 本宿主今日无法定读。
- 判读（诚实口径）: ①mode t4 方向一致为正——本腿 +36/+52%, 复探腿
  .000967 中位（vs base +15.7%, 3 样本紧密 ±2.3%）, r1 腿 +4.4%;
  ②mode t8 定读 GAP, 留待安静宿主窗（复跑基建齐备: w3fix7-guest.sh +
  /tmp/w3fix7-vm.sh + overlay, 15 分钟可复全腿）; ③stock 列跨环境不可比
  （base 腿产自 mv3d-battery 满载宿主, 本腿 load 0.04; r1 腿 stock
  +10.6/+15.2% 为同内容中性对照）。
- 工件落位: 原会话腿保全于 bench-after-r1/; 复探+ABAB 原件 reprobe-mode/
  （10 JSON）; guest 驱动全程 guest-run-takeover.log;
  console-w3fix7-takeover.log。

### guest smoke / metis / audit 门 — 全绿

- **smoke**: run_mode_smoke.sh rc=0, **26/26 PASS + SMOKE-DRIVER PASS**
  （gate-takeover/mode-smoke.log; 台账 arenas before=0 after=0）。
- **metis_eq ×2**（hook STRICT=1, MODE on marker 在）: rc=0×2,
  两遍 `"distinct_words":65073,"checksum":"2d383eeed4ceb73b"` —
  **与基准精确同值** ✓（gate-takeover/metis.{1,2}.out）。
- **audit_gate**: **gate_pass=1**; j1_probes=0/j1_hits=0; j2_walks=27
  violations=0 stale=0; wl_violations=0 unclassified=0 brk_anomalies=0 ✓。
- **dmesg**: corten warn/bug/oops = **0 行**。
- 首次 boot 尝试失败 1 次（trixie-mv3d 底图被 t1-bat VM 写锁占用）→
  换 w6v2 底图成功（偏差已登记）, 非门失败。

## 总判（接手会话口径）

| 门 | 结果 |
|---|---|
| diff 通读（只读） | ✓ 两处机械+4 新锚, 判定/安全论证与注释一致 |
| checkpatch --strict | ✓ 0E/0W/0C（1002 行）ready for submission |
| =n 零符号 | ✓ 17 消费对象双零 + 5 provider 缺席（前会话跑, 本次复核+.config 恢复确认） |
| 恢复构建 | ✓ 续完 bzImage #410, 零新增警告（唯一=objtool cpuidle 基线） |
| KUnit 三套件 | ✓ on×3 全绿逐格一致（27/0/1 + 146/0/0 + 34/0/5）+ off 绿; interlock flake 1 事件登记（负载敏感, 负载退潮三连绿） |
| mmbench bench-after | ✓ 12 JSON 落 bench-after/（机制核验过）; 读数 mode t4 正向, **t8 定读 GAP**（宿主风暴+双峰, 复跑基建留） |
| guest smoke/metis/audit | ✓ 26/26 + checksum 2d383eeed4ceb73b 精确同值 + gate_pass=1 + dmesg 静默 |

移交注意: ①commit/merge/tag 归主会话（本会话未动 git 状态, 只新增/覆盖
evidence 文件）; ②bench-after 已被本腿覆盖, 原腿在 bench-after-r1/;
③t8 定读复跑: 安静宿主窗跑 `bash project/bin/w3fix7-vm.sh boot && run &&
fetch`（VM 驱动 + guest 驱动已入库 project/bin/w3fix7-vm.sh|guest.sh;
guest 驱动同件在 9p share 根）④kernel 版本串 -dirty 后缀 = project/ 下
evidence 文件未提交所致, mm/ 代码与 HEAD 严格一致。




## 门 1 读数（2026-10-08 09:1x）
- **checkpatch --strict**（5 mm 文件全量 diff, 1002 行）:
  `total: 0 errors, 0 warnings, 0 checks` — "ready for submission"。
- **恢复构建续完**: 前会话 00:31 中断遗留 160 个零字节 .o（kill 于 AR/CC 途中,
  vmlinux.a 曾含空成员 intel_gt_pm_irq.o 致首次续跑 LD 失败）→ 清零字节 .o
  后 `make -j8` 续跑, **bzImage ready #410**（09:15, 18080768 B, 与committed
  bzImage-w3fix7 同尺寸）。全日志唯一 warning = objtool cpuidle_enter_state
  （stock 基线, 在案登记项）, **零新增警告**。内核 6.18.32-g6f42d5daeadc。
- **=n 零符号门验收 = PASS**（前接手会话 2026-10-08 00:16 产出, 本次复核）:
  n-build-takeover.log rc 完成（bzImage #409 ready）; n-zerolen-takeover.txt:
  17 个消费对象（gup/madvise/memory/mempolicy/migrate/mincore/mlock/mmap_lock/
  mmap/mprotect/mremap/mseal/msync/oom_kill/rmap/swapfile/vma）nm_corten=0 且
  strings_corten=0 双零; 5 个 provider 对象（corten.o/corten_arena.o/
  corten_arena_test.o/corten_test.o/corten_fault_test.o）=n 下缺席。
  .config 已恢复（五 CONFIG 全 y）+ 受影响对象已重编（memory.o 9 corten 符号
  @00:18, corten.o/arena.o @00:20）+ 本次续编至 bzImage #410 完成。

