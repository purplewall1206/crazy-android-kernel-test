# CortenMM→Linux 移植 · 主控状态文件
(唯一权威状态源；任何 agent 接手前必读；每夜循环结束由主 agent 更新)
创建: 2026-09-12 23:2x CST (主 agent 环境预制轮)

## 使命
把 CortenMM (SOSP'25) 的核心思想移植进 Linux 6.18: 消除热路径上的软件层抽象(VMA 树),
改用「页表页锁协议 + per-PTE metadata array + 事务接口」, 在 QEMU x86_64 环境用论文同款
测试证明: 干掉 VMA 后内核照常运行, 且 mmap-PF/PF/unmap 等热路径取得可测的性能提升,
fork/lmbench 等全地址空间操作允许论文量级的回退。ARM64 侧完成移植性设计文档 + 交叉编译验证。

## 环境状态 (全部已验证, 详见 bin/env.sh)
- 内核树: /home/ppw/linux-6.18 — android17-6.18 **后台拉取中**(git fetch aosp, 日志
  /home/ppw/fetch_android17-6.18.log; googlesource 匿名限速, 预计数小时; 断了就重跑同命令, git 增量续传)
- 种子配置: /home/ppw/kernel/linux-6.18/.config (本 VM 实证可启动: MEMCG/PSI/SWAP/LRU_GEN/ZRAM=y)
- VM: ~/vm/trixie.img, `KERNEL=<bzImage> bash ~/bench/host/launch_vm.sh <img> "systemd.mask=sys-kernel-config.mount"`,
  KVM 加速 4G/8vCPU, 串口 tmux 会话 vm, guest ssh ~/vm/gssh (root, 10022), 9p share ~/bench/share
- 工具: perfetto v58.2 ✓(全链路已冒烟, 见 log/20260912-r00-smoke.md: guest tracebox 采集→9p→
  host trace_processor SQL, mmap_lock 三 tracepoint 归因验证通过), sashiko 67 文件 ✓, pdftotext 论文文本 ✓;
  **sudo 可用**(密码用户口头提供,不落盘); qemu-system-aarch64 6.2.0 已装(M9 可真启动); Go 待装(tarball)
- [冒烟发现] guest zram 不随机自启(bandit 项目靠 setup_zram.sh 手动跑) → M0 必须把 zram 初始化
  纳入 android 基线启动流程, 否则"默认开启 zram"不成立
- GitHub: purplewall1206/crazy-android-kernel-test 返回 404 — **需用户一次性动作**: 建仓库
  + 提供推送凭据(PAT 或把 ~/.ssh/id_ed25519.pub 加到账号)。未就绪前 publish/ 本地累积。

## 里程碑
状态唯一真源已迁至 **docs/ROADMAP.md**(任务 ID 级, 含证据链接/依赖图/风险登记册)。
速览: M0 ✓ M1 ✓ M2 ✓ | M3a ◐(1 blocking: desc->lock BH 对称) | M3b 设计完待实施 |
M4-M8 未开始 | M9 设计完(ARM64_PORTING.md), 交叉编译 gate 定在 M4.T4。

## 当前阶段
- **[2026-09-13 v3 规划者重规划备注(用户指出含论文误读)]** 新增 docs/PAPER_SPEC.md
  (论文唯一权威解读, PS-A..G 条款化含评测协议全细节); DESIGN 重写为"以 PAPER_SPEC
  为规范的移植决策+DEV 偏差表"。修正三处实质误读: ①废除 v2"PTE 真源原则"(倒置了
  论文 PS-B2 metadata 唯一真源; rmap/回收/换出改 arena PTE 必经事务=论文 §4.5 原文)
  ②新增 MODE-process 透明接管(此前真实应用根本进不了 arena, PS-F7/F8 复刻不成立)
  ③M5 fork 默认改论文忠实遍历(不走 copy_page_range, 避免双真源)。EVAL 补齐
  mitigations=off(PS-F2)/tcmalloc 双分配器维度(PS-F8)/JVM 精确口径/8vCPU 不可测
  清单; ROADMAP 同步(M4.T0 前移, R11/R12 新增)。v2 的 D10 中"chunk-VMA+PTE 真源"
  表述由 D11 取代; M3B_DESIGN.md 不变, 与 PAPER_SPEC 冲突处以后者为准。
- **[2026-09-13 12:4x 规划者会话重规划备注]** 应用户要求完成 v2 重规划(循环会话
  空闲期, 无锁冲突): 新增 docs/{ROADMAP,DESIGN,EVAL}.md + MASTER_PROMPT v2
  (瘦身驱动器+会话锁协议+日窗协议)。DESIGN 补齐了 v1 缺失的 M4-M6 深设计:
  mmap_lock write→read 降级(§4.1)、fork 复用标准 COW 只补 metadata(§5.1)、
  **chunk-VMA 分阶段 rmap 接入**(§6.2, Stage1 wired/Stage2 2M chunk)、
  "PTE 真源 + metadata 语义覆盖"原则(§6.3)、不变量 INV1-8(§7)。这些为规划者
  提案, 循环会话 review 时如与 M3B_DESIGN 冲突以先实现为准并记 OQ。
  v1 prompt 与旧微基准副本已入 attic; STATE 里程碑块改为 ROADMAP 指针。
- **r01 夜: M0 ✓ M1 ✓ M2 ✓**（证据 results/r01/, publish/baseline/; 摘要见 log/20260913-r01.md）。
  M2 三提交: b8386e4e(m2a)/2fd4070e(m2b)/1284a235(fix1), tag corten-r01-m2{a,b,c-fix1};
  KUnit 16/16 双验证; corten=on 启动 BUG 已修（D9）。
- **r02 日间（09:00-23:00 计费窗, 只做读写类工作）**:
  - M3a（协议加固: uninstall↔写锁互锁/hole ensure-alloc/8 新 KUnit/lockdep 脚本）代码完成,
    review **FAIL→一项 blocking**: khugepaged pte_free_now 在 RCU_SOFTIRQ 上下文调 uninstall,
    write_lock_irqsave 可与同 CPU task 的 plain read_lock 死锁 → 修法=desc->lock 全链 BH 对称。
    r2 agent(agent_72154da9) 白天改码, 23:00 后验证（KUnit×3/lockdep/corten=on）。
  - **M3B_DESIGN.md 完成**（publish/, 667 行, 函数签名级, PR_CORTEN_ARENA=79, per-mm xarray
    2M 框查找, VM_CORTEN=bit43(CONFIG_64BIT), percpu_ref drain 方案, S1-S8 切片表 ~1640 行）。
    最大风险 R1: 上层页表 fill 的封闭性论证（一切进 arena 路径必须被双钩子收口）。
  - **ARM64_PORTING.md 完成**（publish/, 504 行, 107 file:line 引用）。核心发现: contpte
    fold/unfold 整块 16 PTE 重写与单 PTE 写锁的真实冲突（OQ1/P3 阻塞, 推荐 PTL 嵌套）;
    PTRS_PER_PTE=512/2048/8192（4K/16K/64K）; 移植层 LoC 估算 450-1000。交叉编译验证待窗口。
- **[2026-09-14 08:30 r02 夜末·已提交两项]**
  - M3a.F1 ✓ commit e911b31adb9c tag corten-r02-m3a-f1（BH 对称; KUnit25/lockdep/BH探针/on/=n/真boot 全绿; 详见 log/20260914-r02.md）。
  - M3b.S1-S3 ✓ commit d040b61051af tag corten-r02-m3b-s123（prctl=79/xarray/shadow-VMA/KUnit 21+8; =n 回归/on 全过; review P0+C1-C9+复验追加 5 修全落）。
  - M3b.S4-S7 ◐ 断点保留: 基础面全绿（=y 构建/off 三套件/on 基础面）, 增量 3459 行已在新基座重放; **遗留 fill_upper pud 垃圾值 Oops（corten=on 真链路用例）**——探针与疑点在 results/r02/s46-verify.md; 过程修掉 3 真 bug（pmd presence gate/提前解引用/pX_alloc 指针误判）。worktree m3b46 现场未动; r03 先修它。
  - 基础设施: **mm/corten.o arm64 零错误编译**（Round B, 唯一阻塞=test 文件 x86 宏, OQ5 回填乐观）; syzkaller 构建完成+cfg 就绪（infra-report.md）。
- **[2026-09-16 07:40 r04 夜末: 🎉 M3 里程碑 PASS]**
  - 主树 9 个项目提交, 最新 tag: corten-r02-m3b-s46 / -fix1 / -s8。
  - M3 DoD 4/4 PASS（详 log/20260916-r04.md + results/r03/final-smoke/m3-verdict-final.md）:
    压测 5+1 配置零 panic 零错误 / maps 恰一条 shadow-VMA / kselftests fail-set 与基线一致 /
    **perf 367K 样本 find_vma·mmap_lock 零命中**（M3 头条主张实证）。guest KUnit fault 13/0/0。
  - M3b.S1-S8 全部落地。三轮 DoD 驱动缺陷修复（A 新鲜页分派/B ref 配对+drain 降级/C TLB range）
    全部带 KUnit 回归锚与 debugfs 计数（实测自愈各一次）。
  - M9-P1 双证据齐（Image 40MiB + corten.o 零错）; M4.T0 规格就绪（P0=锁序反转）。
  - **r05 计划**: M4.T0a 锁序重构 → T0b（含 mremap 路由=D12）→ 零改动回归集 → M4.T5 中期对比;
    M9 文档回填; lockdep+arena 压测预热（M7）。
  - 待用户: publish 手动 push; 规划者: OQ-A/OQ-B。- **r03 计划**: S4-S7 pud 修复→增量 review→commit; M3 DoD guest 冒烟（arena_stress/perf 符号/maps_check/ksmoke）; M3b.S8（debugfs arenas+ksmoke）; M9 Round A 续传; 余量则 M4.T0 设计评审。
- **待用户**: GitHub PAT（HTTPS push, SSH22 被封）——publish 本地已提交。
- **[2026-09-13 15:4x 循环会话 r02 更新·已按 v2 §2 持锁]**（锁 owner=sess_0a9112d9, 心跳 5min）:
  - M3b S1-S3 代码完成（worktree /home/ppw/linux-6.18-m3b）→ review **FAIL**(P0=缺
    corten_arena_mm_exit =n 桩 + C1..C9) → 修复单已发, agent 改码中;
  - M3b S4-S7 代码完成（worktree /home/ppw/linux-6.18-m3b46, 增量 ~2100 行）→ review
    **FAIL**(FALLBACK_BIT 无生产者 / 热路径 vma_lookup 破坏 0-maple-walk DoD / DECLARE 未拒
    驻留页) → 修复单已发;
  - 切片映射: 本会话 S46 切片 = ROADMAP M3b.S4-S6 + S7 的 fork 防线; S8(debugfs arenas/
    kselftests 冒烟收口) 留下一夜。提交顺序: M3a → S123 → S46（S4 与 S6+S7 同批, 无危险中间态）。
  - 夜间基础设施 agent 已挂后台（等 M3 make 收尾后跑 M9 交叉编译两轮 + syzkaller 构建, -j6/-j4, 01:00 硬上限）。
  - 夜间验收线（主 agent 决策）: M3a.F1 commit+tag; S123 commit+tag; S46 若验证+增量 review
    通过则 commit, 否则顺延; green.txt 登记; 08:30 硬停实验, 09:00 前关 VM/解锁/夜报。
- [基础设施] gmm 微基准 mmbench + apps 等价件在 bench/{mmbench,apps}/（checksum 跨机一致已验证）;
  guest 测试件齐（lmbench lat_proc=/usr/lib/lmbench/bin/x86_64-linux-gnu/lat_proc）;
  Go 1.23.4 / syzkaller clone / bootlin aarch64 工具链已备（M7/M9 用）。
  [坑] 9p 不自动挂载; pkill qemu 用 [q]emu 防自匹配; guest apt 慢(187kB/s)大件后台。

## 关键决策记录 (主 agent)
- D19 (2026-09-19, r07 m4t12 收口登记; **提案——待主 agent 决策实施**): **T1c
  常驻 arena 池** —— G1 翻盘的正确杠杆不是 VA 分配器（T1 杂志已做到 99% 回收
  +零 xarray 节点 churn, seg_claims 88 摊薄 26k 次发放）而是 **arena 生命周期
  成本**: 动态口径下 G1 四格 -22.6%~-88.1% 全部归因于"每 16KB mmap 建一个
  arena"的 T0 架构税（probe mpl 形态 ~12.6µs/op 全程在 mmap_write 内, 多线程
  排队放大）。候选实现: (a) arena 描述符缓存复用（percpu_ref/ctl_lock/obs/
  kalloc 池化）; (b) per-thread 常驻 multi-chunk arena（16KB op 退化为 chunk
  mark/unmark, 即 probe uv 形态, CHUNK 形态已实证 3×）。建议规划者立 OQ。
- **G1 口径更正** (2026-09-19, r06-m4t12 DIAGNOSIS D1, r07 maintainer 复核
  采信): bench mmbench 为**静态链接**, LD_PRELOAD MODE hook 从未进入其 mm
  （证据链: ldd / g1-consol .err 单 marker / auto_mmaps +160 vs 动态重编
  +19365 / 同源码动态重编对照）。因此 t5-run2 / g1-consol 的全部 mmbench
  "T0" 臂实为 **legacy-vs-legacy 慢漂移, G1 全部历史读数作废**;
  publish/ 的 t5-run2 报告中 mmbench 臂结论表述须按此更正读（apps 臂
  dedup/metis/psearchy/JVM 动态链接、hook 双 marker 实证, **不受影响,
  G3/G4 结论维持**）。后续 mmbench 臂必须用动态口径并在 meta 记 sha256。
- D17 (2026-09-19, M5.T1a maintainer 收口登记): **DEV-14** —— M5 fork 定型 =
  **copy_page_range 照常（PTE 层; 其 wrprotect 置位 = corten_glue_pte_write
  白名单第 1 处, ≤3 已用 1, DESIGN §3 已记）+ fork_begin/fork_commit 事务 meta
  镜像**（SHARED 置位+快照/子 arena 注册/meta 深拷贝/冻结窗单一收口, mmap.c 双钩子
  之间）。D11(d) 与 M5_FORK_SPEC §1.3 的"不走 copy_page_range"**作废**——自遍历
  重写与双钩子夹持构成第二实现, copy 层 PTE 语义本就经钩子收敛进事务。
- D18 (2026-09-19, M5.T1a maintainer 收口登记): **DEV-15** —— fork 冻结窗
  （arena frozen 位; fork_begin 冻结 / fork_commit 单一收口 unfreeze+
  percpu_ref_reinit / loop_out fork_abort unwind）为**移植自造语义**（论文无对应,
  换取 fork 窗与事务的互斥正确性）, REPORT 披露清单项。同批登记实现 DEVIATION
  两处（代码注释已声明, 此处编号引用）: ①**CORTEN_INVALID+KEEP_PERM 槽的子侧
  重表达**为 PRIVATE_ANON+同 perm（协议无法写 Invalid 槽的 perm, 按规格字母跳过会
  在子侧重演 r06 rogue ACCERR 形状）; ②**fork_commit 镜像循环不持父 ctl_lock**
  （register_child 需嵌套子 ctl_lock, 两 mutex 同 lockdep class 不可表达嵌套;
  注册表写方本就全在 oldmm mmap_write 之下, frozen 记账仍在锁内收口）。
  **DEV-11 废止生效**: fork_demote 全删无 fallback（fork_demotes 计数留历史遥测,
  本 boot 恒 0）。review 三条件 **F2（过度 SHARED 角落）/F3（fork drain 注入
  缺口）/F4（OQ-5 LATR 关联）→ 全部转 T1b/T2 跟踪项, 本轮不修**（commit 正文
  Known boundaries 同步披露）。
- D16 (2026-09-18 rogue 班使用, 2026-09-19 maintainer 补登记, 防编号重占):
  present-RO 缺陷族修法 = 路由提交 perm 跨"内容丢弃与 fork demote"携带
  （CORTEN_UNMAP_KEEP_PERM + FRESH 门 perm 优先 + demote 前 perm 物化 split）——
  commit **e219920b0923** 标题即 "(D16)", tag corten-r06-rogue; 本行补登记使
  编号账实相符（D16 不得再作他用）。
- **D20 (2026-09-21 用户直接指令·架构升级)**: **移除整个 VMA 层**——从 D1 的 opt-in shadow-VMA
  升级为论文完整形态: MODE 进程零 VMA 运行（maple tree 空，全部内存经 corten metadata/事务）。
  分阶段: V-A 零 VMA 匿名内存（parked 窗去 VMA 化+file 区记录非 VMA 化）→ V-B /proc/maps 从
  metadata 枚举 → V-C GUP 无 VMA 走查 → V-D exit teardown 无 VMA → V-E brk 策略 → 终态=MODE
  进程 VMA 层完全移除。4K 页必须支持（x86 原生 4K ✓; arm64 门控 ARM64_4K_PAGES 已落; 16K/64K
  明确不支持）。无时间限制持续工作直到完成，沿途 commit+push。
  注: VMA **层代码**仍保留服务非 MODE 进程（单内核双 MM 形态）；"移除"指 corten 管理的内存
  完全脱离 VMA 语义。
- D15 (2026-09-18, T5 开销诊断班授权落地): percpu_ref 改 **born-atomic（PERCPU_REF_INIT_ATOMIC）**——
  RELEASE drain 从"等一个 RCU GP 且持 mmap_write"改为同步到零只等在飞事务。实证: MODE munmap
  8554μs→227.8μs（base 278）; dedup tcmalloc -91.84%→**+19%**。commit ba77046c78fe。
  教训: born-percpu 的 GP 窗口在"每 munmap 一 RELEASE"的 MODE 形状下是 O(GP×次数) 放大器。
- D14 (2026-09-17 用户直接指令): **09-18 全天解除时间窗限制**——从早到晚持续工作直到完成计划。
  对 09-18 全天生效（含上午，实际自 09-18 00:00 起持续）: 构建/VM/压测等实验动作不再受 23:00-09:00 门禁约束（timegate 对 09-18 的班次跳过）,
  其余铁律（基线永存/诚实汇报/单写者锁）不变。09-17 日窗剩余时间维持原协议（只读写）。
- D1 (2026-09-12): 移植形态 = **opt-in "corten arena"**(prctl/boot param 划定的地址空间区段),
  非全 mm 替换。理由: Linux 的 gup/rmap/khugepaged//proc 全走 VMA, 一次性替换无法保证内核运行;
  论文自己也说 retrofit Linux "requires substantial engineering efforts"。shadow-VMA 保证其余子系统可用。
- D2 (2026-09-12): 语言 = C (融进 linux mm/), Rust/Verus 形式验证**不做**; 用「伪规范文档 + lockdep +
  KCSAN + KUnit 不变量测试 + syzkaller」逼近论文的 strong correctness。主 agent 决策, dev agent 不得翻案。
- D3 (2026-09-12): 先 CortenMM_rw (rwlock 版协议), 打通全链路后再评估 _adv (RCU+DFS) 作为 M4+ 的可选升级。
- D4 (2026-09-12): guest 规模 8 vCPU, 论文 384 核——目标为**方向性验证**(可扩展性曲线斜率与拐点),
  不承诺复现 26× 数字, REPORT.md 必须明说。
- D5 (2026-09-13): zram 压缩 = **lz4**(对齐论文与 M0 DoD)。android17 构建在种子配置上改:
  ZRAM_BACKEND_LZ4=y / ZRAM_DEF_COMP_LZ4=y / DEF_COMP="lz4"（FORCE_LZO 派生自动为 n）。
  种子配置本身不动；此修改记入 results/r01/config-android17-m0。
- D6 (2026-09-13): 微基准语义偏差接受（mmbench README 声明）: mmap=mmap+munmap 成对计 1 op;
  unmap 系 refill 不计时; pf=每页 1 次写。M1/M8 必须用同一二进制同一脚本同参。
- D7 (2026-09-13): metis/dedup/psearchy 获取失败或 2h 编不过时用**功能等价 workload**
  (metis_eq=多线程 map-reduce 词频, dedup_eq=多线程 malloc/munmap churn 流水线,
  psearchy_eq=多线程文本倒排索引), 等价件与真源码尝试均留档, M1/M8 同件同参。
  JVM 线程创建用真 openjdk-21 + 固定 Java 微程序。
- D8 (2026-09-13): M2 切片 >300 行授权（骨架 1254 行+协议 1862 行, review 全绿）;
  M2b review PASS 但其 5 个"必修"文档/测试项折入 fix1 提交(1284a235f751)而非阻塞合入。
- D9 (2026-09-13): corten=on 早期挂死根因 = early param 解析期翻 static key（run-time
  text patching 未就绪）; 修法 = early_initcall 延迟翻转（两阶段 dmesg 日志）。红线不变:
  boot 参数默认 off, 基线路径零扰动（=n 编译 + off 冒烟双验证）。
- D12 (2026-09-15, ponytail skill 载入后评估用户两点):
  ①"移除 VMA 减少代码"=**终态属性而非中间步骤**——理论上限成立(mm/mmap.c+vma.c+maple_tree≈12.5k 行,
  89 个 mm/*.c 文件依赖 vma), 但工程路径必先"加"后"减": 我们 opt-in 路线净增 ~11k 行(M2-M3b)。
  lazy 路线=shadow-VMA 复用整套 VMA 机制(fork/proc/gup)而非重写。**M8 报告增加 LoC 终态账**
  (若 arena 成默认、可删的 VMA 热路径代码量 vs 新增 corten 代码量, 论文口径对照)。
  ②"上层接口不可改、已编译程序必须照常运行"=内核第一原则, **升级为 T0 硬门**:
  (a) OQ-A(mremap-on-arena)从"提请规划者"升为 **T0 必须路由实现**(kernel-copy ~120 行, 否则
  glibc realloc 会失败=破坏已编译程序); (b) T0 DoD 增加**零改动回归集**: M1 基线真实应用
  (JVM/metis_eq/dedup_eq/psearchy_eq/lmbench)不经修改在 MODE-process 下跑通且 checksum 一致;
  (c) 非 opt-in 进程零感知已有保障(=n 折叠+static branch+等价测试)。

- D10 (2026-09-13, 规划者): 文档架构 v2 定型 — STATE(现场)/ROADMAP(计划)/DESIGN(机制)/
  EVAL(口径)/MASTER_PROMPT(驱动); 单写者会话锁($PROJ/run/lock)+日窗只读协议(§2)。
  技术提案三则待循环会话 review 采纳: ①M4 mmap_lock write→read 降级 ②fork 复用
  copy_page_range 只补 metadata ③M6 chunk-VMA + PTE 真源原则(详见 DESIGN §4-§6)。
- D11 (2026-09-13, 规划者, **修正 D10-③ 与 v2 DESIGN 两处误读**): 
  (a) **PAPER_SPEC.md 为论文唯一权威解读**, 偏离必须登记 DESIGN DEV 表+决策编号;
  (b) **arena 内 metadata=唯一真源(PS-B2), 一切 arena PTE 写必经事务**——rmap/回收/
  换出路径的 PTE 修改路由进事务(论文 §4.5 "rmap always goes through the transactional
  interface"), 胶水收敛 corten_glue_pte_write 白名单(≤3 处)+INV7/INV9 一致性 checker;
  (c) **新增 MODE-process 透明接管**(prctl 进程级开关, addr=0 匿名私有 mmap 四入口路由,
  brk/显式地址/文件映射留 legacy=DEV-6)——没有它论文真实应用实验(F7/F8)无法复刻;
  (d) **M5 fork 默认论文忠实遍历**(wrprotect+shared+meta 深拷贝), copy_page_range
  复用降为 OQ7 fallback; (e) EVAL 对齐 PS-F: 两臂 mitigations=off、dedup/psearchy
  双分配器档、JVM spawn→init 窗口口径、8vCPU 不可测清单入报告。
- D13 (2026-09-16, T0a review C1 登记): DEV-11（fork 过渡=arena 全退场+MODE 继承, M5 换忠实 fork 后废止）、
  DEV-12（per-mmap N auto-arena + NORESERVE 记账豁免 + OVERCOMMIT_NEVER 降级）、DEV-13（**锁序修订:
  mmap_write > ctl_lock > drain-wait > desc->lock > ptl**, 取代 DESIGN §7 旧 INV2 锁序——结构性消灭
  ctl_lock→mmap_write 反向边, 四调用点穷举见 T0a review §①）+ PR_CORTEN_MODE EXIT 语义偏离
  （SPEC §1.1 "-EBUSY" 草案 → 全退场, 与 DEV-11 自洽）。docs/DESIGN.md §7 的 INV2 文字更新留规划者
  （本条即权威登记, include/linux/corten_arena.h 头注释引 STATE D13）。
- **[2026-09-17 07:50 r05 夜末: M4.T0a/T0b 已提交, T0 判定=部分达成]**
  - 提交: 5aef23c4aee9 tag corten-r05-m4t0a（T0a）; 9d74b22a1348 tag corten-r05-m4t0b（T0b）。主树 12 个项目提交。
  - T0a review PASS-w-conditions（锁序反转"结构性消灭反向边"; C1 登记→D13; C2 实为注释错标已修; C3 计数已补）。
  - T0b review PASS-w-conditions（条件 C1 -EAGAIN 重试+计数/C2 回滚+计数/C3 runner soft-skip/C4 SPEC 勘误——全部收口）。
  - 五轮缺陷修复各带 KUnit 锚: B1/D-A/D-B(kthread_use_mm)/B2/D-C/D-D/D-E(clear 前置)/D-F(归一化)/D-G(fill_upper+pending perm)/D-G'(rearm 重试, guest recovered=16)。
  - **T0 零改动 DoD=7P/3F/2S 部分达成**; 遗留两条已登记 OQ: D-G''（JVM CDS abort, 独立根因, dg_probe2 可复现）+ fork 边界（DEV-11, 需规划者设计裁决——demote 翻译 VMA split vs 文档化重提交）。
  - M9 文档回填 ✓（ARM64_PORTING.md r2, 662 行）。

- **[2026-09-17 日间 r06 预备]**
  - **D-G'' 根因定案**（results/r05/dg2-analysis.md, 三方证据非推测）: JDK21 CDS map_archive 用 file-backed mmap(MAP_FIXED,RW) 落进 auto-arena → legacy mmap_region 的 vms_gather_munmap_vmas（vma.c:2469）**绕过只挂 do_vmi_align_munmap 的 arena 守卫** → shadow-VMA 无声打洞 → 洞内 fault 仍按 xa_load 地址路由归 arena → FRESH 门陈旧 PROT_NONE 拒读 ACCERR。**修法必须在 mmap 侧**（fault 侧放行会发匿名零页=静默损坏）。
  - **D-G'' 修复代码完成**（wt m4fix, +580/-24/5 文件: F-A 分级覆盖自检 tier1=缓存边界零成本/tier2=RCU find_vma+VM_CORTEN 位测; F-B 前置 punch 路由 do_mmap 门+__mmap_prepare frame 探针 backstop——纯拒绝会再断 CDS MAP_FIXED 故选 punch;连带 release/fork_demote 对多片 shadow-VMA 的适配; 2 KUnit）。diff=patches/r06-m4dg2.diff。
  - **M4.T5 runner 就绪**（bench/t5/: ABAB 交错+M1 参数审计 19/19 自测+strace multiset+G1/G3 预判定+QUICK 模式, 预计 guest 35-45 分钟全矩阵）。
  - **M9-P1 代码完成**（wt m9: corten_test 位宏 arm64 sect 构造三分+Kconfig 过渡门控 ARM64_4K_PAGES 限定+PGTABLE_LEVELS>3 守卫; diff=patches/r05-m9-p1.diff 67 行; 验证命令序列在班次报告）。
- **r06 夜验证链**: D-G'' verify（判据 java F→P）→ review → commit → T5 首跑（committed 树）→ M9-P1 arm64 构建+KUnit → M4.T0 判定更新（D-G'' 闭合后）→ M5.T4 fork gate 视 OQ-D 裁决。

- **[2026-09-18 上午 r06 收官: M9-P1 PASS + present-RO 族闭环 + T5 首跑]**
  - 提交: 452ad7b9d91e tag corten-r06-m4dg2（D-G'' 打洞+B1 悬垂修复+P1 vma_end_read 泄漏——exit 挂死根因, hung-task 栈实证）; ba77046c78fe（D15 born-atomic, MODE munmap 8554μs→227.8μs）; e219920b0923 tag corten-r06-rogue（present-RO 族: zap 擦 perm + demote 丢 perm 两同源缺陷一次闭环, 同时是 T5 三 app rc=139 与 OQ-D 主体）。主树 16 个项目提交。
  - **M9-P1 PASS**: arm64 Image（+196KiB）+ CortenMM KUnit 25/25 arm64 首次全绿（-smp 4 复跑）。P4 前半 gate 达成。
  - **MODE 兼容达成**: java -version rc=0、dedup/metis/psearchy 全 rc=0（此前 3/3 rc=139）。残余: JThreadBench ClassFormatError（既有, 下一片）。
  - **M4.T5 首跑（9d74b22 口径, 被 present-RO 遮蔽）**: G1/G3 NOT MET; 正面: mmap-pf low t8 +17.9%/unmap-virt low t4 +17.1%/unmap high t4 +18.9%; 真实开销: dedup tcmalloc -91.84%（已由 D15 修复→+19%）、JVM spawn >480s（前提 libc 族已修, 待重测）。**T5 重跑（e219920 内核）进行中**。
- **[2026-09-18 午后 r07 进行中]**（D14 unrestricted）
  - M9-P1 提交 ✓ 025756094542 tag corten-r06-m9p1（x86 回归零警告）。lockdep 预热三关全过（M7 首检: corten 锁 PROVE_LOCKING 零 splat; interlock 用例 lockdep 开销下 1 次时序 flake 复跑绿→M7 清单）。
  - **T5 重跑（rogue 内核 e219920）**: G3 MET（dedup tcmalloc +11.32% CV 紧）/G4 MET/G1 1/4（unmap-virt low t4/t8 +48.3†/+11.4†; psearchy -40% 方差未决列 M8 加测）; **present-RO 零复现**; JVM 2000 线程完整跑完（残余=已登记 ClassFormatError）。镜像版本串异常已符号级澄清（=e219920; 建议 clean 重编）。
  - 主树 HEAD=025756094542（15 项目提交+marker）。并行三线: JThreadBench ClassFormatError 排查（VM 协调: 先日志分析后 VM）/M5.T1a 忠实 fork 实施（新 worktree）/G1 加测（VM 排队）。- **r06 收尾→r07 计划**: T5 重跑结果 → M4.T5 正式判定/M4 收口评估 → JThreadBench ClassFormatError → M7（lockdep+arena 压测+syzkaller 挂机）→ M5.T1 忠实 fork（M5_FORK_SPEC.md 就绪; OQ-1 多片形状已在主线）→ metis_eq fork 后复测。
- **[2026-09-19 02:5x r06 夜末 m5t1a 收口: M5.T1a 已提交]**
  - 提交: **68697442097e** tag **corten-r07-m5t1a**（忠实 fork: frozen 冻结窗/copy_page_range 照常=glue 白名单第 1 处/fork_commit 事务 meta 镜像/COW 核心（fault_once epilogue folio 所有权=泄漏修法）/fork_demote 废止）。主树 17 个项目提交。
  - 终态补跑全绿（评审条件闭合; results/r06/m5t1a-final-verify.md + guest-final/）: KUnit on/off、fork_isolation 双臂、metis_eq MODE 全量 checksum 三方同值+fork-probe OK（OQ-D 闭环落终态代码）、fork_roundtrip 1000 轮（23:03 COW-reuse 泄漏修法闭合）、JThreadBench 2000、run_mode_smoke 26/26; lockdep 变体零 splat 证据由前班在档; checkpatch 0E0W0C; bzimg/r07-m5t1a。
  - 登记: D16（补）/D17/D18; DEV-11 废止生效; DEVIATION×2; **F2/F3/F4 → T1b/T2 跟踪**; DESIGN §3 白名单 "≤3 已用 1"。未 push。

- **[2026-09-19 r07 maintainer+reviewer M4.T1/T2 收口]**
  - 提交: **de8a685370bb** tag **corten-r07-m4t12** "mm: CortenMM arena: per-cpu
    VA magazine and munmap fast path (M4.T1/T2)" (基座 68697442097e, 主树第 18 个
    项目提交; 3 文件 +1122/-221); patches/0001-...per-cpu-VA-magazine-...patch;
    快照 diff=patches/r07-m4t12.diff; checkpatch --strict 0E/0W/0C (1661 行) →
    results/r07/checkpatch-m4t12.txt; 主树应用后与 worktree 3 文件 cmp 字节级全等。
  - 复审 PASS（焦点逐条）: ①杂志并发=全写方持本 mm mmap_write, percpu 仅局部性/
    子树不相交; claim 严格前进+回卷 unwind; sentinel 十处 xarray 访问点穷举
    语义一致; fork 子状态全新（仅复制 next_va, magazine/va_free/seg_list 不继承）;
    ②T2=同帧单查找 ar_end 仅作记账、分类用指针相等性; unmap_chunk 全出口
    tlb_finish_mmu（含 -EAGAIN 路径）; force_flush 重试自同一 addr 补 metadata
    复位（for 增量在 break 后不执行——逐字核对无漏）; ③4 修复逐个落实
    （recycle 整块丢弃含 VMA 块/va_nrfree 部分消费递减/punch 洞帧不回 marker/
    pinned_end 删除）; ④红线=路由白名单/分类器/migrate 未触碰, =n 守卫嵌套
    核对, KUnit 新增 mag_recycle+mag_marker+auto_route 扩展全绿×2;
    ⑤D1 口径更正论证扎实（见上 G1 口径更正条目）。
  - 判定: G1 NOT MET (0/4, 动态口径) + 机制级归因 + D19 T1c 提案; T1/T2 机制
    本身按设计生效（va_recycles 25805≈99% / 仪式税 49.5→17.4µs / CHUNK 3×）。
  - 登记: **D19**（T1c 常驻 arena 池提案, 待主 agent）+ **G1 口径更正**
    （mmbench 静态链接; t5-run2/g1-consol mmbench-T0 臂作废; apps 臂不受影响;
    publish/t5-run2 报告 mmbench 臂表述按此更正读）。
  - 已知备注（非阻塞）: include 头注释与 verify 报告称回收列表 "LIFO", 实现为
    FIFO+尾块扩展（纯顺序启发, 无正确性影响）; auto_route 测试 addrs[8] 以
    KUNIT_ASSERT_LE(ncpus,8) 封顶, >8 CPU 机器该用例会 ASSERT 失败（测试文件
    局部, KUnit 实际运行环境 ≤8 vCPU）。
  - 未 push; VM vm-m4t12 (port 10026) = worktree #25 终态件留运行。

- **[2026-09-19 夜 ~22:3x r07 maintainer+reviewer M5.T1b/T2' 收口]**
  - 提交: **1f8dfc78ae9f** tag **corten-r07-m5t1b** "mm: CortenMM arena: COW
    unshare, INV7 checker and fork test battery (M5.T1b/T2')" (基座
    802ff7551bd0, 主树第 21 个项目提交 = m5t1a#17→m4t12#18→T1c#19→perf1#20
    →本片; 5 文件 +1127/-13); patches/0001-mm-CortenMM-arena-COW-unshare-
    INV7-checker-and-fork-.patch; 快照 diff=patches/r07-m5t1b.diff(与 worktree
    diff 字节全等); 主树应用后与 worktree 5 文件 cmp 字节级全等。
  - 复审 **PASS**（焦点逐条, 只读）: ①F2 门=子侧 pmd presence 门属实
    (fork_mirror mm=child, dup_mmap 双写锁下; pmd_leaf 保守跳=安全方向),
    窗粒度残留论证核对成立（无第二 mapper+mapcount==1 首写自愈）; fork 性能
    无回归信号(fork_faithful=2202 正常走); ②INV7 walker=rcu_read_lock +
    corten_lock_range(write_lock_bh 原子安全) + ptl 嵌套合规, txn 出口无泄漏;
    真断言(SHARED+可写 PTE 即违例, checked>=1 防空转), 豁免=结构性（比声明
    更严）; ③force_write 与上游实拍对齐: can_follow_write_common(gup.c:598)
    在 VM_WRITE VMA 拒 FOLL_FORCE、!VM_WRITE+MAYWRITE+exclusive 放行——
    FORCE 转派门恰为其接受集, 复用/拷贝两分支均产 exclusive 页; 永不
    mkwrite/永不改 perm 红线守住(用例双形状锚); OQ-4 分派调用点核对=
    faultin_page(unshare) 唯一生产者, WRITE|UNSHARE 互斥 VM_WARN 映射全函数;
    ④OQ-5 实读核对: do_wp_page() 复用分支 wp_page_reuse() 前
    SetPageAnonExclusive(memory.c:4129-4136 属实, OQ-5 原文前提确有误),
    wp_can_reuse_anon_folio 拒 DMA-pinned→拷贝分支; ⑤KUnit on5 终件日志
    24/0/1+38/0/0+26/0/0 核实(尾部 VFS panic=无盘跑标准收场, Totals 之后),
    7 新用例+2 既有用例 USER 化加固; bench 件(逐页双侧 magic 对撞/诚实 skip)
    质量过关; ⑥残余登记口径与代码实况一致。
  - **D18 F2/F3/F4 闭环声明**: F2=fork_mirror 子侧 pmd 门(本片)/F3=
    fork_drain_leak 10*HZ 真超时注入(本片)/F4=OQ-5 复用必置 exclusive(本片)
    ——三项全部落地并有 KUnit 锚, **关闭**。**OQ-4 关闭**(UNSHARE→ctx.write
    走 COW 分派, unshare_pin 锚); **OQ-5 关闭**(按树内代码裁决, 覆盖 SPEC
    原文前提)。
  - **T2' 残余登记（非阻塞, 下片候选）**: ①F2 门窗粒度——同窗页粒度
    DONTCOPY 片边界过度 SHARED 残留（自愈, 该窗 INV7 checker 可见; 页粒度门
    +~10 行可闭）; ②FORCE 边界——perm RO+VMA VM_WRITE(routed-partial 降级
    片) 外部写=响亮 ACCERR/EIO, 与 legacy(无声 wp_page_copy) 分叉, 语义裁决
    "外部写不得无声废止进程契约", 闭环需 routed-partial 同步 VMA 位（T0 遗留
    形状另评）; ③fork_battery.sh ksmoke 步需显式 --kdir /root/ktree（预编译
    口径, 证据来自手动带参复跑 rc=0）。非代码备注: force_write 拷贝分支不置
    pte_mkdirty（上游 wp_page_copy 写故障形状置; 私有 anon 无 pte-dirty 消费
    方+与 UNSHARE 共体, 影响为零, 仅记备考）。
  - 维护者产物: checkpatch --strict **0E/0W/0C** (1275 行) →
    results/r07/checkpatch-m5t1b.txt（**已重生成**——原文件为 spinlock 注释
    补齐前的陈旧轮, 代码中注释已在, 复跑全绿）; bzImage 不重打（M7 syzkaller
    已停, worktree 验证构建为准: 普通件 sha256=313111b4…3a836e, 证据全出自
    该件+lockdep 变体）。
  - worktree m5t1b 未触碰（仍停在 802ff7551bd0+未提交增量, 留作者处置）;
    VM tmux m5t1b-vm (lockdep 件, hostfwd 10027) 留运行供复核。未 push;
    密码未落盘。

- **[r07 日班 reviewer+maintainer M5.T3 收口 (2026-09-19)]** 复审 **PASS** → 入库
  **commit 5c545359e856** tag **corten-r07-m5t3** "mm: CortenMM arena: GUP interop
  fixes and pin accounting (M5.T3)" (基座 1f8dfc78ae9f, 5 文件 +528/-285; 主树应用
  patches/r07-m5t3.diff 后与 worktree cmp 字节全等; format-patch → patches/
  0001-...GUP-interop-fixes-...patch; checkpatch --strict 0E/0W/0C →
  results/r07/checkpatch-m5t3.txt; bzimg/r07-m5t3 + sha256 + green.txt, 件=
  worktree 验证构建 #11)。复审要点: ①force 移除完备 (FORCE_COPY/handler/ctx.force/
  FORCE_WRITES 统计零残码, foll_force 重写双形状锚为真断言: RO 契约外部写 SIGSEGV
  + PTE/meta/folio 逐项不动 + VMA-writable 边界仍拒绝=perm 规则非 VMA 位; 与
  can_follow_write_common 的对齐=移除后不再有"仅 FOLL_FORCE 可 re-follow 的存活
  形状"); ②UNSHARE 落回: 调用点=memory.c 钩子 FALLBACK_BIT 消费 (VMA_LOCK/atomic
  已前置排除, mmap_lock 保持, txn 锁先释放, 无锁嵌套); wp_page_copy unshare 分支
  BUG_ON(pte_write) 硬门=永不 mkwrite, perm-RO 契约硬件层不可废止; 落回仅触发于
  F_ACCERR (MAPPED+perm 拒绝), 无 untracked 窗 legacy 安装旁路; meta SHARED 残留
  由 cow_write reuse 分支既有残码清除自愈 (INV7 检查 SHARED↔RO-PTE 形状仍闭合);
  ③zap_pinned: 命中= zap_window 清 PTE 时 folio_maybe_dma_pinned, 语义合法不拒绝
  (pin 引用 refcount-native), debugfs 渲染+named_counter 可读, state4 锚 +1,
  guest delta=5289 为 M6 skip-gate 频度证据; ④D12: 行为变更=外部 FOLL_FORCE 写
  routed-RO 页 T2' 成功→T3 EFAULT; ptrace=调试场景非"已编译程序运行"场景; 普通
  read/write pin 对 RW 契约不变 (state2/3+guest pread/io_uring 实测), VMA-writable
  形状 legacy 本就无声 wp_page_copy=被 T2' 残余②裁决禁止的分叉, D12 判定通过。
  **T2' 残余② 关闭** (语义裁决落地)。**遗留登记**: ①ptrace 分叉——upstream
  FOLL_FORCE 契约 (poke RO 页成功) vs arena 响亮 EFAULT, 属 upstream 级接口设计
  问题 (proc-poke/process_vm_writev 工具面), 需独立切片评估 (暴露口+文档+潜在
  follow_pfn_ratelimit 形状); ②T2' 残余① 同窗页粒度 DONTCOPY 边界 SHARED 残留
  (自愈, 原样保留); ③T2' 残余③ fork_battery.sh ksmoke --kdir 口径 (原样保留)。
  **非阻塞备注**: a) gup_state4_pin_zap 尾部 folio_put ×2 对 count==1 多放一次
  (本树 page _refcount=atomic_t + !DEBUG_VM 下静默, free 时 prep_new_page 重置,
  断言全真/零内核影响, 下片 1 行修); b) memory.c:6579 钩子注释 "the M5 FOLL_FORCE
  COW case" 例子已陈旧 (该形状已删); c) folio_maybe_dma_pinned 对 order-0 真
  FOLL_PIN (plain +1 ref) 探测不到 (count≥BIAS 启发式), zap_pinned 对 order-0
  低估——M6 设计输入时注意。未 push; 密码未落盘。

- **[r07 凌晨班 reviewer+maintainer M6.T1 收口 (2026-09-20 ~19:45 CST 起)]** 复审
  (r07-m6t1 增量 +461/-7 7 文件 585 行 diff 只读精读 + 上游锚逐一核对): **PASS**。
  - **①V1 (P0)**: skip 挂点=上游 `(VM_HUGETLB|VM_PFNMAP)` 掩码后一位, 同一
    VMA 循环同一 `mmap_read_trylock` 上下文 (oom_reap_task_mm 持读锁贯穿),
    VM_CORTEN 读对 mmap 写者稳定 = 与上游同型读取, 无新竞态; skip 不置
    ret=false → 与上游 hugetlb-only mm 的 skip-all 形状完全同构 (返回
    "reaped" 而零释放 = reaper best-effort 既有口径); 逐 VMA skip 严格优于
    mm 级 (同 mm 普通 mmap 照常 reap); arena 释放路径 = exit_mmap 事务序
    (corten_arena_mm_exit drain mmap.c:1407 先于 unmap_vmas) = 已验收 carve-out。
  - **②V2 (P1)**: 两 `_one()` 门在 mmu_notifier_range_init 之前, 拒绝臂零写
    零 notifier 流量 → 不放大 R6-2 zap 缺口; ttu 两入口均 void +
    `.done=folio_not_mapped` → 拒绝经 folio_mapped() 正常通道上报: vmscan
    nr_unmap_fail+keep (无重试风暴: 无 LRU 锚即无再隔离, 计数器=R6-3 绊线),
    migrate 恢复 src, hwpoison -EBUSY 后照走 kill; **复审新发现**: TTU_HWPOISON
    经 memory-failure.c 对 arena folio 今日即可达 (anon_vma 遍历 shadow-VMA),
    V2 非纯"潜伏"——守卫实际关闭了这扇在用的裸写门, KUnit 四形状锚恰含此形状。
    corten_rmap_unmap_one = SPEC D1 原名/原签名, true 臂=T2 落点 (须移入
    invalidate 窗——注释+zap R6-2 注记已登记)。
  - **③语义红线**: 拒绝+留驻 与 "不进 LRU" 对 kswapd 可观察行为等价 (页不被
    回收), 差别=审计口径从"结构够不着"升级为"够得着+明确拒绝+可观测"; D12/
    DEV-10 (压力下 OOM 而非 swap) 诚实保留给 T2, "不装 swap entry 的回收=数据
    损坏"论证成立 (commit 正文入库); 未动 publish 只读纪律 (thaw 发现待 T2
    回填 SPEC §1.2 P5)。
  - **④测试**: KUnit ok27=真 try_to_unmap 端到端 (rmap_walk→守卫) + D1 接口
    四形状直驱 + plain-VMA 负对照; ok28=谓词注入+无损 restore; off 臂设计性
    skip; =n 折叠=头内联桩+静态键 (与 gup.c/memory.c 先例同形, 结构复核过);
    checkpatch --strict 本班独立复跑 0E/0W/0C (快照与 format-patch 双跑一致)。
  - 入库: **commit 0e469cd9d055** tag **corten-r07-m6t1** "mm: CortenMM arena:
    reclaim guards for oom_reaper and rmap (M6.T1)" (基座 5c545359e856, 主树
    第 22 个项目提交; 主树应用 patches/r07-m6t1.diff 后与 worktree 7 文件 cmp
    字节全等; format-patch → patches/0001-mm-CortenMM-arena-reclaim-guards-
    for-oom_reaper-and-.patch; checkpatch → results/r07/checkpatch-m6t1.txt)。
  - bzImage: 归档 bzimg/r07-m6t1 sha256=e004f78f...76f092 (=worktree 验证构建
    6.18.32-g5c545359e856-dirty #4 = 全套证据最终二进制, 主树未重编, 等价性由
    cmp 全等锚定); green.txt (bzimg 内 + run/green.txt registry 行) 已登记。
  - 非阻塞移交: ①=n 对象集逐条 log 未归档 (verify §2 口径声明, 折叠结构本班
    代码级复核过); ②新 KUnit 用例 kunit_skip 后声明 (文件惯例, checkpatch 净);
    ③V1 压测复现条件需巨 mm/阻塞 victim (>2s 退出) 形态——T2 guest 判据设计
    输入; ④M6.T2 移交面: swap out/in 事务 (D1 步骤 3-6 完整化+D5 换入+D6
    编码+D7 消费面+INV7 扩展), true 臂上线时守卫移入 invalidate 窗, thaw/
    DELAY 事实回填 SPEC。
  - worktree m6t1 未触碰 (增量保持未提交态, 留作者处置)。未 push; 密码未落盘。
- **[r07 日班 reviewer+maintainer M6.T2 收口 (2026-09-20 ~07:40 CST 起)]** 复审
  (r07-m6t2 增量 +1817/−93 8 文件, 快照 patches/r07-m6t2.diff 与 worktree git
  diff cmp 字节一致; 只读精读 + 上游锚逐一对照): **PASS** → maintainer 入库。
  - **①swap-out 镜像保真**: corten_rmap_swap_out 与上游 rmap.c:2120-2201(含
    2061-2081 clear 段)逐位对照成立——entry=folio->swap(prefilter 已锚
    swapcache+folio lock, order-0 等价 page_swap_entry); flush_cache_range→
    get_and_clear→defer 分叉→dirty 标记位置同上游; swap_duplicate/
    arch_unmap_one/try_share_anon_pte 三异常臂 swap_free+set_pte_at 原样恢复+
    计数拒绝=上游 walk_abort 同型(defer 形态 clear+restore 同值无需 flush,
    论证成立); mmlist/hiwater/计数移动/exclusive+soft-dirty+uffd-wp 编码/
    set_pte_at/remove_rmap_ptes/folio_put_refs 全同序; 三处有意省略各有锚:
    uffd-wp marker(declare 拒 VM_UFFD_*)、mlock_drain_local(prefilter 拒
    VM_LOCKED)、swapbacked!=swapcache WARN(folio_alloc_swap 拒 !swapbacked
    →swapcache 蕴含 swapbacked, 传递成立)。notifier 窗=vma_address_end(&pvmw)
    与上游 init 同位(pvmw 于 DEFINE 宏即有效)。defer 登记后置=R6-1
    happens-after 强化, 采纳。glue 白名单 #2 INV7 纪律=metadata 同 ptl 提交;
    corten_swap_out WARN 臂在 txn 写锁下真不可达(query 已证数组在), 仅注释
    heal 措辞不精(metdata-MAPPED+swap-PTE 并非 swap-in heal 形状)——nit。
  - **②swap-in**: SWP_SYNCHRONOUS 直读形态与 do_swap_page 4686-4740 逐锚核对
    成立(alloc/charge/swapcache_prepare 双参/memcg1_swapin(1)/shadow-refault/
    folio->swap/swap_read_folio/private=NULL 全同); **拒绝 cache/readahead 的
    论证核实为真且承重**: swap_state.c:498 folio_add_lru + 上游 sync 分支自身
    memory.c:4726 也 add_lru——corten 有意双省略, 换入 folio 真不进 LRU(DEV-10),
    省略安全(PTE 引用消费, 无 LRU 依赖)。exclusive=true 恒置=上游 fresh-page
    规则(memory.c:4843)逐字一致(fork 共享 entry 双直读=各自私有拷贝, anon
    entry 单次消费语义正确); swap_free 持有越过 metadata 提交=严格强于上游
    ptl-内 free; swapcache_clear 在锁外=上游同位。legacy-fallback 拒绝论证
    成立(do_anonymous_page 分支零页盖内容=静默数据丢失; handle_mm_fault 中段
    新 glue 点不可接受)。锁窗外行全由 re-query 关闭(zap→meta 重置→EAGAIN;
    entry 释放→swapcache_prepare -ENOENT 有界重试; SWAP_HAS_CACHE 期间
    entry 不可复用)。**复审新发现(非阻塞)**: heal 臂判别式(pfn==本方回读页)
    在真实 unuse 竞态不可达(unuse 装自家 folio 且 sync 在其 ptl 前); 真实
    双重竞态(sync_meta -EAGAIN/ENOMEM 被调用方忽略+unuse 装他页)→ 有界重试
    4 次→MAPERR(假阳性 SIGSEGV, 无数据损坏, 保守失败)——登记 P12 深化项
    (swapoff 注入时补); swapin_heals guest=0 与此相容。nit: corten_map 不清
    __resv(MAPPED 槽残留旧编码, decode 有 state 门=惰性无害)。
  - **③entry 生命周期**: 编码 type-u8+le32-offset+byte5=0 有界单测; zap
    free_swap_and_cache 在 ptl 下=上游 zap_pte_range 同型, 非交换标记 WARN
    (守卫拒绝形状不可达); fork default 分支整槽拷 __resv 已代码级核实, 计数
    归上游 copy_nonpresent_pte(swap_duplicate), 元数据不含计数=正确分工;
    swapoff 钩在 ptl 前=DEV-13 同向, 中毒跳过=swap-in 重派保守应答。
  - **④驱动缺陷修复回归面**: frame 切片钳制(arena 起止)消除别名重拾(成环
    根因); 每帧短 RCU+cond_resched 消 GP 饥饿; folio->lru 复用合法(永不
    LRU 锚); __reclaim_pages private 契约核实(vmscan.c: private→不 putback,
    存活 folio 回 list, 驱动 folio_put 收尾, 已解锁); swap-in 多余 folio_put
    修复=do_swap_page 记账(folio_ref_add(nr-1)==0)核实。nit: 计数器指针作
    private cookie 传 RVH trace 钩(gki 缺省 no-op, 语义透明)。
  - **⑤测试/构建**: 5 新锚质量过(纯 roundtrip 边界/真链路 roundtrip+zap-free
    带设备门设计 skip+mprotect 合成形 LIFO action 清理/INV7 双向=完好 0+破坏
    恰 1); KUnit 终版 24/0/1+44/0/0+30/0/2 与 lockdep 全绿本班复核日志属实
    (on3 的 1 fail=既有 interlock flake, 披露口径一致); =n 八对象; checkpatch
    本班独立复跑 0E/0W/5C(快照与 format-patch 双跑一致, 5C=CHECK 级咨询)。
  - **⑥遗留口径**: interlock flake 3/6 vs 基线 1/4 已披露(10x 对照建议合理,
    本班裁决=维持 M7 登记); evict 逐页慢=T3 shrinker 正确杠杆; P12 swapoff
    真盘未跑=如实(本次新增 heal 判别式发现加重该移交面); run_mode_smoke 2 例
    基线复现归属 T1/工具面, 维护者裁决=维持。
  - 入库: **commit b51754002f2f** tag **corten-r07-m6t2** "mm: CortenMM arena:
    swap out/in transactions (M6.T2)" (基座 0e469cd9d055, 主树第 23 个项目
    提交; 主树 apply 快照后 8 文件 cmp 字节全等; format-patch →
    patches/0001-mm-CortenMM-arena-swap-out-in-transactions-M6.T2.patch;
    checkpatch → results/r07/checkpatch-m6t2.txt)。commit 正文登记
    **白名单额度满(3/3: ①fork wrprotect ②ttu 换出 ③unuse_pte)——后续任何
    shadow-VMA legacy PTE 写者=设计错误, 须规划者路由扩展先行**。
  - bzImage: 归档 bzimg/r07-m6t2 sha256=0125443a...bdf2f1d0 (=worktree #24
    终版重建 6.18.32-g0e469cd9d055-dirty, 主树未重编, 等价性由 cmp 全等锚定;
    guest/KUnit 终判跑于重建前同源件, lockdep 件单列, 口径见 green.txt)。
  - 遗留移交: T3 shrinker(mm registry/批量化/压力通道)+T4 观测+T5 矩阵
    (swapoff 真盘注入/swapin_retries-heals 并发激发/interlock 静默宿主 10x);
    OQ-M6-2(SHARED 不换出)与 OQ-M6-3(migration 拒绝)维持。
  - worktree m6t2 未触碰(增量保持未提交态, 留作者处置)。未 push; 密码未落盘。
- **[r07 晚班 M6.T3+T4 复审 NO-GO 3 阻断项处置 (2026-09-20 ~20:3x CST 起)]**
  通宵班 m6t34 增量(+1563/−126 5 文件, 详见 results/r07/m6t34-verify.md)复审
  NO-GO 3 阻断项, 本班在 worktree `/home/ppw/linux-6.18-m6t34`(分支 m6-t34,
  基座 b51754002f2f)未提交增量上逐条处置:
  - **B1(spin 锁内睡眠)处置**: `corten_arena_evict_mm()` 每 round 的 reclaim
    原在 `shrink_lock` 持有下跑——folio trylock/zram 同步 submit_bio_wait 均
    可睡, lockdep 下 evict 路径必炸(评审指名的实跑盲区), 且函数注释"all
    released before the reclaim runs"失实。修 = 与 `corten_shrink_mm()` 同形:
    每片 aging 后先 `spin_unlock` 再 reclaim, 有下一片才 `spin_trylock`
    (busy → `break` 记 evict_busy, 已收各片经 swap delta 照报; 首轮 busy
    维持 -EBUSY 语义); 注释改为如实描述锁丢弃形状; verify.md 红线 4 表述
    同步修正。
  - **B2(xarray 泄漏)处置**: `corten_arena_state_free()` 补
    `xa_destroy(&state->shrink_aged)`——凡跑过 pass-1 aging 的 mm, 退出即
    泄漏 shrink_aged 的 xarray 节点, 补 destroy 收口。
  - **顺带(评审①(a)附注)**: shrinker scan 路径补 `!(sc->gfp_mask &
    __GFP_IO) → return 0` 门(上游 superblock shrinker 的 __GFP_FS 先例同型;
    门在 shrink_scans 计数前, 无 I/O 预算的调用不再白扫)。
  - **B3 登记(评审要求的偏差/角案账, 全部非回归)**:
    ① **R6-6 偏差·scan 睡眠**: R6-6 字面"count/scan 不得睡眠"与实现冲突——
    scan 的 reclaim 睡眠(folio lock/zram 同步写/分配器)。内核语义合法:
    scan_objects 语境本可睡, 真纪律=自旋锁只护片内 aging/pick(有界自旋)+
    从不取 mmap_write; R6-6 字面按此解释落账。
    ② **R6-6 偏差·count 活计数**: R6-6 缓解项"count 用 per-mm 缓存值(T3)"
    未照做——实现为 per-desc 活计数(nr_mapped/nr_swapped, desc 写锁内维护)
    registry 走查求和, 构造精确无漂移, 严于缓存值口径(缓存值本为防 count
    虚高, 活计数同此目标且更强), 记偏差非回归。
    ③ **同扫描重 age 角**: age 片游标枯竭(arena 全部窗扫完即 cursor=0 返回)
    后, 同一扫描的第 2 片从 frame 0 起扫, 可重 age 首片已 age 过且 pass-1
    标记已被本扫描自己 pass-2 eval 消费的窗——与 age_slice 注释"cursor wraps
    ACROSS scans only/同扫描绝不重复 aging"自述冲突。影响小: 重复的只是清
    一遍 young 位(冷窗第二遍仍要过 eval 门, 候选页换出后无重复拾取), 无正确
    性影响; 触发条件=首批窗覆盖全部 arena 且预算未耗尽。行为不改, 登记
    备案, 留 T5/M7 顺手收口(如扫描级 epoch 位)。
    ④ **口径小疵(a)·scan 忽略 gfp_mask**: scan 此前不查 `sc->gfp_mask`,
    无 __GFP_IO 的调用白扫一遍且虚计 shrink_scans——本轮已补门(见上)。
    ⑤ **口径小疵(b)·count 路径 mmput 可睡**: count 走查在 RCU 段外对 pin 住
    的 mm `mmput()`(末引用触发 __mmput→exit_mmap, 可睡)+cond_resched——
    reclaim 语境合法(PREEMPT_RCU 下 RCU 段亦可睡, 无原子性谎言), 但与 R6-6
    "count 不得睡眠"字面冲突且此前未登记, 此处补记。
  - DESIGN.md §6 勘误(评审①): M6 回收/swap 节旧文"MGLRU aging 只读统计、
    不改 PTE、无事务需求"有误, 已按 M5.T1a §3 白名单先例做最小编辑 + r07
    标注勘误(SPEC §1.2 P2/P3: 上游 walk_mm aging 对 LRU 页本就清 young 位
    vmscan.c:3717, arena young 位写必经事务; lru_gen 对 arena folio 结构性
    skip 判定一并文档化)。
  - 验证(评审指名重点 = evict lockdep 实跑盲区闭环, 全部本轮重跑, 详证
    results/r07/m6t34-verify.md §7.2): =y 零新增警告 + KUnit corten*
    on×2/off×1 全绿 + =n 八对象 + checkpatch --strict 0E/0W(arena.c 余
    1W 基线); **lockdep 变体(PROVE_LOCKING+DEBUG_ATOMIC_SLEEP 首开) guest
    实跑 `evict 200000000`: rc=0/34s/67565 页多轮 reclaim 全路径,
    evict 窗口本片 shrink/evict 路径零 splat, 零 D 状态(3 采样),
    数据完整读回 verdict PASS(ops=1162125)**; 普通配置 guest run13
    fails=0 复验(swapped_out=swapins=69333 对账) + evict 冒烟 rc=0
    (20005 页/9s); patches/r07-m6t34.diff 备份后重导出(+1604/−126,
    两版逐行核对增量=本轮 4 处处置); =y 终版归档 bzimg/r07-m6t34-rev。
  - **复审新发现(非 B1/非本片引入, 登记移交)**: DEBUG_ATOMIC_SLEEP 首次
    纳入 lockdep 变体(既往轮只有 PROVE_LOCKING, 原子内睡眠不报告)即暴露
    既有缺陷: `corten_arena_zap_window()` 在 desc 写锁临界区
    (corten_txn_begin, write_lock_bh 形态)内调 `tlb_finish_mmu()` →
    `__tlb_batch_free_encoded_pages()`(mm/mmu_gather.c:141, 批量页释放
    可睡)。触发面 = arena unmap/DONTNEED/munmap 路由 + 退出 teardown 的
    zap 事务全族: lockdep KUnit 2 例(gup_state4_pin_zap/zap_keep_perm,
    套件仍全绿) + guest 累计 268 处(回溯 100% = munmap→zap 事务路径,
    无一触及 shrink/evict)。五轮复审未现 = 该开关从未开过; 修法 =
    tlb_finish_mmu 移出 desc 写锁临界区(covering-lock 协议重组, 远超
    ~20 行终审增量)——本班不改, 移交 maintainer/M7 裁决; B1 判据按窗口
    划清: evict 窗口零本片路径签名已达成(verify.md §7.3)。

## 2026-09-21 凌晨(深班后续): §7.3 移交项已修复 —— zap_window tlb_finish_mmu 协议修正
- **修法**: 照 perf1c 先例, flush/finish 全部移出 desc 写锁临界区。
  gather 打开(原子安全)留在锁内; 批溢出经新增 per-window `struct
  corten_zap_win` 上抛, 两 driver(unmap_chunk_flags / mmap_route)重组为
  "锁内 zap 一段 → 放锁 → 锁外 tlb_flush_mmu → 重锁续走"的轮循环; 窗口
  lazy gather 的 tlb_finish_mmu 由 driver 在最后一轮放锁后执行(全错误
  出口无泄漏)。park 路由 caller-owned gather 的 post-downgrade 收尾
  ([perf1c]) 保留。语义红线核对通过: flush 在 PTE 清理后/folio 释放前
  (mmu_gather 内部次序), 每区间恰一次冲刷, 轮间 FRESH 补填与 post-op
  补填同形(无 r03-C 裸 PTE 形状)。
- **前后对照(DEBUG_ATOMIC_SLEEP guest 实跑, 同驱动三形态 unmap/churn/
  DONTNEED 各 15s)**: 修复前基线件 47 处 mmu_gather.c:141 splat(复现,
  含新工具 ds_dontneed 12.7 万 ops 实跑) → 修复后件 **0**。lockdep
  KUnit 严格零签名(撤销 §7.3 的"已知放行") on×2 全绿——既往 2 例 zap
  签名同步绝迹。
- **回归**: =y 零新增警告; KUnit on×2/off×1 全绿(102/49 ok); =n 八对象
  RC=0; checkpatch 0E/基线 1W; guest run13 fails=0(70077 页对账) +
  JThreadBench 3/3 rc=0 + dmesg 静默。
- **产物**: patches/r07-m6t34.diff 重导出(+1838/−223, 修前快照备份
  .prezap, 增量 452 行); bzimg/r07-m6t34-zapfix(sha256=7dd8c3e2...);
  bin/r07-m6t34-zapfix-verify.sh + bench/share/m6t34-zapfix-ds.sh +
  bench/share/ds_dontneed.c(新); results/r07/m6t34-zapfix/ 全套日志;
  verify.md §8(根因/修法/前后对照/回归矩阵)。
- **登记(非阻塞)**: 宿主多 VM 过载下 corten_test_txn_uninstall_interlock
  2 例 flake(时间窗超睡所致, 代码路径与本修零交集, 重跑即绿, 详
  verify.md §8.5); ds_dontneed 首版 DECLARE EINVAL 已修(范围须恰为一
  VMA)。修法判据=全绿, 本班收口, 待 review 终审后 maintainer 提交。

## 2026-09-21 凌晨(终审班): M6.T3/T4+zapfix 终审 PASS —— maintainer 提交, M6 收口
- **终审结论(只看上轮 NO-GO 未覆盖增量, 全部核过)**:
  ① B1 修复成立: evict_mm `for(;;)` 每条退出路径均已解锁(break 在
  spin_unlock 后; busy 重取失败记 evict_busy break, 首轮 busy 维持
  -EBUSY), 各片已收 swap delta 照报; 与 corten_shrink_mm 同形属实。
  ② zapfix 轮循环成立: 溢出页续走=force_flush 约定(zw->addr 停溢出页,
  续轮只补元数据); 两 driver 全部错误出口都过 tlb_finish_mmu(park
  caller-owned gather 的 post-downgrade 收尾保留); FRESH 补填与 post-op
  补填轮间同形(事务性成对, 无 r03-C 裸 PTE 形状; wholesale meta drop
  门加 !zw->force); 常态单轮零新增锁往返, 性能由 run13 fails=0 覆盖。
  ③ B2 xa_destroy 落码 + B3 登记(DESIGN §6 r07 勘误 / STATE R6-6 ①-⑤)
  在档。④ M5.T3 一行属实(删多余 folio_put + refcount==1 断言)。
  ⑤ =n 八对象 RC=0; checkpatch 于 format-patch 复跑 0E/0W/9C(arena.c
  3976 braces=基线 1W 未触); M4.T0 mprotect 路由零触碰。
- **入库**: commit **2639d3294b9d** tag **corten-r07-m6t34** "mm:
  CortenMM arena: reclaim shrinker, observability and sleep-correct zap
  (M6.T3/T4)"(基座 b51754002f2f; 主树应用快照后 5 文件 cmp 字节全等);
  format-patch → patches/0001-mm-CortenMM-arena-reclaim-shrinker-
  observability-and.patch; checkpatch → results/r07/checkpatch-m6t34.txt
  (于 format-patch 复跑, 覆盖 zapfix 前旧件); bzimg/r07-m6t34/(=zapfix
  终态件 sha256=7dd8c3e2..., green.txt 全绿登记) + run/green.txt
  registry 行。
- **M6 收口**: T1(0e469cd9d055) / T2(b51754002f2f) / T3+T4(2639d3294b9d)
  全提交; T5 判据已由 run13/run5c 前置达成; M4.T1/T2 + T1c 均已提交。
  余项: M4.T5 五次加测固化 + M8 终稿 + M7 周期。worktree m6t34 未触碰
  (增量保持未提交态); 未 push。

## 2026-09-21 通宵班(M8 收尾): G1 五轮固化 + **G5 门实测 NOT MET(新事实)** + REPORT.md v1.0 终稿
- **G1 固化(前班 t5final 已落, 本班无新测量)**: MET 2/4 = REPORT §4.2 终读数
  (results/r07/g1-final.md)。
- **G5 门(M5.T4)实测 = NOT MET**: lat_proc MODE 臂 fork/exec/shell = 1696.9→10454.1 /
  4932.1→13402.2 / 14261.5→64679.8 µs(3 rep 中位, 同 boot, run2 干净口径;
  run1 判废存档=hook atexit fork-probe 被子进程继承 + 9p stderr 污染)。
  **根因机制级证实且与忠实 fork 无关**(fork_faithful 34→34 不变, 零 arena fork=E1 no-op):
  prctl ENTER 急切建 shrinker registry(corten_arena_mode_enter) → fork_begin 急切复制子
  registry → exit_mmap 每 mm `list_del_rcu + synchronize_rcu()`(完整 RCU GP≈8-50ms,
  隔离实验: /bin/true 带 hook 生命周期 +50ms vs plain 6.75ms)。暴露面=进程派生密集 MODE
  工作负载; 长寿命进程(apps/JTB)摊销不可见——与 T5 无回退自洽。JTB MODE 回归 rc=0 零 CFE
  (第六次)。**修复方向登记 REPORT §7-A5**(惰性入册 / 空 state kfree_rcu 快速退出, 小 diff);
  本班未实施(禁改内核树约束)。证据 results/r07/g5-gate/。
- **REPORT.md v1.0 终稿落盘**(publish/REPORT.md, M8.T3): 12 处 PENDING 清零(8 填充 +
  4 转显式计划中 A5-A8); 头部刷新至 HEAD 2639d3294b9d(25 提交/24 tag); §1.2-1.4 全表刷新;
  §2.4 LoC 终态账实测(VMA 三件 12,720 vs corten 19,229=净增 51%, 如实声明);
  §3 M4/M5/M6/M7/M8 里程碑行全刷新(M5.T1b=1f8dfc78/T3=5c545359 已提交入账;
  M6 guest 判据 69164/69356 roundtrip + RSS 281→3.8MB 入账); §5.1 KUnit 终态行
  (HEAD=102 pass+3 skip 0 fail); §6 lat_proc 行、§7 遗留重分级(已闭环节+A5-A8+B8/B9)、
  §8 计划表、§9.1 提交清单(25/24)、附录自检。G2/G7 定量/G6 二夜/G5-fix 转计划中。
- VM vm-t5final 留运行(port 10026, #90=2639d3294b9d); 未碰内核树 worktree(一次误建
  raw/ 目录已即时移出并 git status 复核零残留); 未 push; 密码未落盘。

## 2026-09-21 午后(review/maintainer 班): M9-P2 + perf2a 复审 PASS 入库 + defconfig 守卫修复 + REPORT v1.3
- **M9-P2 复审 PASS → 提交**（m9 worktree, 分支 m9-arm64）: commit **93f834060cd1**,
  tag **corten-r07-m9p2**, `mm: CortenMM: arm64 pte alloc/free hooks (M9.P2)`。
  复审核实: ① asm-generic `__pte_alloc_one_noprof()` 钩子落点在两个失败返回之后
  （pgalloc.h:94）; pgalloc.h:5 已有 `#include <linux/corten.h>`（M9-P1 落的, 全架构
  继承者可编译）; arm64 无 `__HAVE_ARCH_PTE_ALLOC_ONE` 整体继承 = 单落点成立
  ② x86 `pte_alloc_one()` 纯删除**必要且正确**: `corten_ptdesc_install()` 对已
  tracked pfn 二次 install 走 replace+WARN_ON_ONCE（mm/corten.c:356 实码核实）,
  保留 x86 侧调用则每次分配必告警 ③ arm64 `__pte_free_tlb` 钩子在 asm/tlb.h:93
  （`tlb_remove_ptdesc` 前, 批量路径不经 pte_free() 的论证与 x86 `___pte_free_tlb`
  同构）④ KUnit 证据抽查: 4×25/0/0 绿臂（run3 实证 ok 25）+ 3× interlock TCG
  时钟伪影 = M7 登记项跨架构再现（x86 KVM 同基座 ok 19 绿反证）⑤ checkpatch
  --strict **0E/0W/0C**（results/r07/checkpatch-m9p2.txt, publish 镜像）。
  format-patch → patches/0001-mm-CortenMM-arm64-pte-alloc-free-hooks-M9.P2.patch。
  提交注记: 实际 stat +29/−10（任务书 -13 为记误, 内容与快照 r07-m9p2.diff
  byte-identical）。arm64 Image 未重归档（M9-P1 已有 + 增量验证件
  results/r07/m9p2-verify.md, 已镜像 publish）。
- **perf2a 复审 PASS → 提交**（perf2 worktree, 分支 perf2）: commit **825b4d59e9d0**,
  tag **corten-r07-perf2a**, `mm: CortenMM arena: thin-peek metadata drop on the park
  path`（4 文件 +46/−77）。复审核实（作者"逐字节等价"主张成立）: ① `corten_query()`
  实为 `corten_txn_meta()` 的 copy-out 包装 → recorded 谓词等价; `corten_txn_meta_drop`
  全树零残余引用 ② 原语契约: 槽复位回归 perf1b 之前的 `corten_unmap()`（含 M6.T3
  nr_mapped/nr_swapped 逐转移维护 = 原 wholesale drop 同总量）; **corten_meta_free()
  是纯 kfree** → perf2a 的逐槽 unmap 连 payload 释放都比 perf1b wholesale drop 更严格
  （对无 payload 引用形状为 no-op）③ 失败路径: 中途 bail → 已处理槽 Invalid 不被
  RELEASE 重复计数, 尾段交调用方真 RELEASE = perf1b success-gate 同理 ④ UNMAP_PAGES:
  PMD 对齐 arena 独占 PT 页 → 走查范围 ⊇ 记录槽集合, 逐槽 inc == drop 的 nr 一次加
  ⑤ KUnit on×3（Totals 30/0/2 抽查实证）+ off（6/0/26）全绿; 台账 335==335 与
  munmap_releases==pool_parks==49055 见 perf2.md §2/§3 ⑥ checkpatch --strict
  **0E/0W/0C**（results/r07/checkpatch-perf2a.txt）。bzimg 终件归档规范名:
  **bzimg/r07-perf2a/**（bzImage-perf2a-final sha256 442f1af8…d2823 与 perf2.md 登记
  一致 + base 对照件 + SHA256SUMS）。
  **未采纳候选与机制结论（M8 素材, 已入 REPORT v1.3 §4.6）**: 批 mark/窗粒度重构 =
  后续杠杆（§7-C1 同归）; 残差 = PMD 窗粒度 park/take 512-PTE 扫描 ~13µs/op（最大
  T0-only 单项）+ IPI/调度地板; 同代码 boot 漂移 −15~−31% 证明比率地板, 多 boot 中位
  口径登记（并入 −14~−22 家族带宽, 上沿到 −31）; park 512-PTE 高水位界缩 = r03
  defect C 禁止的信任方向, 判为 C1 素材不硬凑。
- **defconfig 守卫修复（M6 遗留, 主树独立小 commit）**: **101b0aac8bff**, `mm:
  CortenMM arena: compile the memcg shrinker hooks only under MEMCG`（+9/−0,
  mm/corten_arena.c 两处 `#ifdef CONFIG_MEMCG`）。要点: ① 任务书预选 IS_ENABLED
  **不可行**——受护符号（`shrinker->id` 成员、`mem_cgroup_is_descendant` 原型）在
  关闭态根本不存在, 未走分支仍须名字解析 → 按 shrinker.h 自身惯例用 #ifdef
  ② 首诊归因更正: `shrinker->id` 实为 **CONFIG_MEMCG** 门控（shrinker.h:107-110,
  并非 SHRINKER_DEBUG——两符号在验证配置同时关闭导致首诊张冠李戴）③ !MEMCG 语义 =
  cgroup 过滤退化 match-all, 与既有 `mem_cgroup_disabled()` bail 同答。验证: defconfig
  + CORTEN_MM + ARENA 对象 **0E/0W**（results/r07/defguard-defconfig-build.log）+
  工作配置（MEMCG=y）回归 **0E/0W**（defguard-workcfg-build.log）; 过程注意: 直接
  `make mm/corten_arena.o` 会强制构建未选中对象（CORTEN_MM=n 时报 mm_struct 成员错
  = 伪失败）, 正确口径 = defconfig 后 `-e CORTEN_MM -e CORTEN_MM_ARENA` + olddefconfig
  再构建对象。主树 .config 已原样恢复（备份 /tmp/corten-main-config.bak 复核）。
- **REPORT.md v1.3 落盘**（publish/REPORT.md）: 头部版本块/树状态（主线 HEAD
  101b0aac8bff, 29 提交/27 tag = 主线 27 + 分支件 2）; §1.3 M9 行 P2 ✓;
  §1.4-8 未及项更新（M9 余 P3）; §3-M8/M9 节刷新（M9 P2 详条目）; §4.6 机制成本节
  补强（perf2a 保留 + 未采纳候选 + 机制结论 + 残差家族带宽上沿 −31）; §7-0 新增
  defconfig 守卫闭环行（含归因更正与 IS_ENABLED 不可行理由）; §8 行 8/10 刷新;
  §9.1 提交清单 27/B1/B2 三行; §9.2 证据索引 m9p2/perf2a/defguard 三组;
  附录自检 v1.3（新增路径逐项复核 OK）。全部新引用路径存在性已核（m9p2-verify.md
  已镜像 results/r07/）。
- 边界: 未 push; m9/perf2 worktree 各自增量已提交、未再触碰; 主树 .config 复原;
  密码未落盘。

## 2026-09-21 午后(maintainer 班#2): 分支集成收口 — m9-arm64+perf2 并入主线 + 统一验证 + green 门 + worktree 清理
- **集成 merge（均 --no-ff, 零文本冲突）**: ① 4d3b28669e36 `Merge branch 'm9-arm64'
  (M9.P2 arm64 hooks)`（3 文件与主线增量零重叠, 干净合并）② 4ca6ef1048f0 `Merge branch
  'perf2' (perf2a park-path thin peek)`（perf2 的 mm/corten_arena.c 与主线 A5/defguard
  增量文本块无重叠 → ort 自动合并**未触发冲突标记**; 任务书预期冲突未发生, 改以语义核实
  兜底: HEAD^2..HEAD diff 逐 hunk 核对 = perf2a 逐槽薄 peek(corten_txn_slot+slot->state)
  与主线 A5 全部 hunk(call_rcu 延迟回收/懒 registry/ENTER 不建 registry/fork 条件建)+ 
  defguard #ifdef CONFIG_MEMCG 共存; corten_txn_meta_drop 全树零残余引用; corten_txn_slot
  在 mm/corten.c:1461 在位）。合并增量 266 行。
- **统一验证矩阵（results/r07-integrated/）**: =y `make -j12` exit=0 零新增警告（仅基线
  objtool cpuidle; modpost memblock 本轮未触发）; KUnit corten* 三套件 on×2 = 24/0/1 +
  48/0/0 + 30/0/2 ×2, off = 25/0/0 + 18/0/30 + 6/0/26（plain 跑了 ×3, 超任务书 off×1,
  顺带 flake 检查）全绿零 not-ok — 与 A5/perf2a 登记形状逐格一致; **口径注意**:
  corten-verify.sh 的 filter_glob=corten 只跑主套件, 三套件口径须 filter_glob=corten*
  （perf2-kunit.sh 同参）, 本班两口径都跑了; =n 八对象（A5 口径 memory/mmap/migrate/
  rmap/swapfile/gup/oom_kill/x86-fault + m5t3 口径 kernel/sys+mempolicy/mremap/madvise/
  mprotect 复核）全部 RC=0、nm 零 corten 符号（任务书"八对象"两代口径都过）; checkpatch
  --strict 合并增量（101b0aa..HEAD）0E/0W/0C（checkpatch-integrated.txt）。
- **bzimg 归档 + green 门**: 终件 #95 `6.18.32-g4ca6ef1048f0`（=y 配置复原重编, 与 HEAD
  精确一致）→ bzimg/r07-integrated/bzImage-integrated-final sha256 12e3b715…4b7f69 +
  SHA256SUMS。green 门: VM tmux **vm-integrated**（port **10032** — 10031 被 perf2 留运行
  qemu 占用, 其 tmux 会话已亡但进程存活; 新会话端口绑定失败即退, 登录核验 uname 兜底发现,
  首启教训）corten=on mitigations=off kunit.enable=0, img trixie-integrated.img（=trixie-
  perf2.img 副本, 不污染 perf2 件）。登录 OK + uname=g4ca6ef1048f0 + arena_stats enabled=1
  + zram lz4 2G prio100(+swapfile -2) + 9p hostshare OK + run_mode_smoke（T1c 契约件
  share/t1c/corten_mode_smoke, sha f45157cf, **9p 根目录旧 binary 有 24/26+栈粉碎既有伪影
  勿用**）26/26 + SMOKE-DRIVER PASS 台账 0→0 + JThreadBench 2000×3 ×3 JVM rc=0 零 CFE;
  终态 ptdescs==meta_arrays==324, munmap_releases==pool_parks==seg_claims==2, dmesg
  corten warn=0 Oops/BUG=0。→ run/green.txt 登记 r07-integrated 行。证据
  results/r07-integrated/guest-green-gate.log + guest-green-gate-tail.txt。
- **tags 核对**: `git tag --merged HEAD` = 27/27 全量（含全部 corten-r07-* 11 个 +
  corten-r06-* 5 个; 分支 tag m9p2/perf2a 合并后进入 HEAD 可达集）。
- **worktree 清理（14 移除 + prune, 保留 kcov）**: m9/perf2/m5t1a 干净直删; 其余 11 个
  （m3b/m3b46/m4fix/m4t0a/m4t0b/m4t12/m5t1b/m5t3/m6t1/m6t2/m6t34）status 有脏 → force。
  **说明**: 脏=历次「worktree 验证构建 + 主树 mint byte-identical commit」流程的草稿残留
  （green.txt 各里程碑"cmp 字节全等"注记在档）; 抽查证实为被主线超越的中间态（m3b 脏
  hunk=M3a interlock 已提交文本的前身; m6t34 脏内容引用 corten_txn_meta_drop — perf2a
  已从主线删除该函数 = 主线严格更新）; 且 14 个分支全部 `merge-base --is-ancestor` HEAD
  通过。/tmp/wt-oldbase 已消失（git worktree prune 收编）。保留: linux-6.18-kcov（M7
  二轮在用）。分支 ref 未删（任务书只要求 worktree）。
- **REPORT v1.4 落盘**（publish/REPORT.md）: 头部版本块 + 树状态（HEAD 4ca6ef1048f0,
  31 提交 = 主线 29 + 集成 merge 2 / 27 tag）; §9.1 标题刷新 + 表增 M1/M2 两行 + B1/B2
  标注"已并入主线" + 序 27 摘除"=主线 HEAD"标记。
- **边界**: 内核树未 push（无远端 push 配置, 任务书禁令确认）; 密码未落盘; timegate 记录
  式放行（构建/VM 各步均记录时间戳; D14 豁免仅 09-18, 本班照同日午后班惯例即时收口,
  窗口外放行如实登记）; VM vm-integrated = #95 corten=on 留运行; kcov 侧 VM/m5t3/m6t2/
  basecheck 等未触碰。- **D20-a (2026-09-22 主 agent 批准 M-V 规格两处诚实化偏差)**: ①"零 VMA"=**树空**而非零
  vm_area_struct——保留不入树的 detached carrier 作 rmap/anon_vma/COW/GUP-pin 载体（拒 carrier
  = 重写 fork/COW/GUP ×3 工程量, 论文 OS 无此包袱）; ②**Dominion 划分**: 窗口域 [16T,64T) 零 VMA、
  委托遗留域（exec 期映射/栈/vdso/brk/MAP_SHARED+窗口内 file MAP_FIXED 植入）保持白名单 VMA。
  终判据 J1-J4 采纳（find_vma 零调用审计/委托封闭断言//proc 双源 oracle/GUP 零 VMA 走查）。
  9 切片 ≈4700 行/9-12 夜获批分阶段实施。spec=publish/MV_VMA_FREE_SPEC.md。
- **[2026-09-22 07:1x r07 maintainer: M-V A.0 ✓ —— M-V 系列启动]** D20-a 批准后首片。
  复审 PASS（六焦点: ①struct 八字段与 §2.2 签名逐一对齐、region 内嵌 1:1; ②register
  `may|ar->prot` OR 闭合（CORTEN_PERM 平位编码, 超集由构造成立）; ③5 个 rclass 写点
  全在 mmap_write 下（declare 3 caller=prctl/auto-attach assert/do_mmap 路由, fork_commit
  双 mm assert, park/reactivate 注释+caller, take 头部 assert）, 且覆盖全部生命周期迁移
  ——arena 分配点仅 declare:873/fork child:2899 两处均已盖章, take 先盖 record 后翻 idle
  顺序正确; ④lookup=arena_lookup 精确别名（parked/sentinel 不覆盖=legacy 语义）, next=
  xa_find 升序+指针去重+sentinel 跳过, 与既有 fork_unfreeze/mm_exit 走查同形, sentinel
  比较沿用 `== &corten_va_reserve_sentinel` 惯例; ⑤=n 三 inline 桩+前置声明, KUnit +4
  恰一次（48→52）质量好（真值表含 MAY 吸收臂/迭代器真实杂志段 512 sentinel/visibility
  split/fork 拷贝）; ⑥零行为变化核实: -7 全为注释 rewrap+debugfs printf 重排, 无路由
  改动、无新锁、生产无新调用者）; patches/r07-mva0.diff 与 worktree 增量逐字节一致。
  提交: worktree mv-a0 commit **34f1ae661be3** → 主树 ff-merge（HEAD=34f1ae661be3,
  tag **corten-r07-mva0**; patches/0001-mm-CortenMM-arena-land-the-region-record-MV-V-A.0.patch
  + r07-mva0.patch; checkpatch --strict **0E/0W/0C** 845 行 = results/r07/checkpatch-mva0.txt）。
  bzimg/r07-mva0（bzImage sha 74c1aba2…=验证构件, green.txt 快验: 留运行 VM 同构件引导,
  run_mode_smoke maintainer 复跑 rc=0 26/26 SMOKE-DRIVER PASS, dmesg corten 零 WARN/BUG,
  arenas 表头 cls/rflg/pcs 新列在）。
  **移交 A.1**（park 去残留: PROT_NONE 预约 VMA 消失, §3.1.1）; A.0 遗留登记: INV-MV3
  debug checker（may⊇prot 逐页/rclass-idle-池互恰）随 A.1+ 首个消费切片落位（铁律 3,
  数据面已就绪）; J2 白名单审计 walker 定位 A.3（§3.1.3, 验证报告已披露）; lockdep 基线
  既有 xa_destroy softirq splat（pristine 复现同签名, mva0-verify.md §4）建议 A.1+ 顺手
  换 __xa_destroy。未 push; 密码未落盘。

- **[2026-09-22 07:4x r07 集成收口复核班: 分支集成独立复核 PASS + =n 证据补档]** 任务书
  触发的复核（发现 09-21 午后班#2 已完成集成, 本班以独立证据收口, 未重做）: ① merge
  4d3b28669e36(m9-arm64)/4ca6ef1048f0(perf2) 均在 HEAD 祖先; 两侧增量 diff-of-diffs
  **逐字节等价**（perf2 4 文件 / m9 3 文件, 仅 index 行与 hunk 头偏移不同 = A5 +55 行前置,
  正文零差异 = 干净合并实证）; 语义并集在 HEAD(含 mva0) 全在位: corten_txn_slot
  (mm/corten.c:1461) + corten_txn_meta_drop 全树零残余 + defguard #ifdef CONFIG_MEMCG
  ×2(arena:3531/8762) + A5 懒 registry/xa_destroy(:366/:371)/call_rcu(:1701)。
  de8a685/87383f 核实在主线（任务书疑问关闭, 无需补 merge）。② tags: 全部 28 个
  corten-r0* `--merged HEAD`（含 m9p2/perf2a/mva0）。③ **=n 八对象证据缺口补档**: 原
  班声称 =n RC=0 零符号但 r07-integrated/ 无落盘日志 → 本班在隔离 worktree(/tmp/corten-nobj,
  用后即撤, 主树未触碰) @ 34f1ae661be3 defconfig(CORTEN_MM 默认 n) 重建 13 对象
  （A5 口径 memory/mmap/migrate/rmap/swapfile/gup/oom_kill/x86-fault + m5t3 口径
  kernel/sys/mempolicy/mremap/madvise/mprotect）**全部 RC=0、nm 零 corten 符号**
  → results/r07-integrated/nobj-eq-n-verify.log（+publish 镜像）。构建窗口内(07:33 CST)。
  其余统一验证证据本班逐项复核属实: KUnit on×2 24/0/1+48/0/0+30/0/2、off 25/0/0+
  18/0/30+6/0/26 全绿零 not-ok; checkpatch 0E/0W/0C; bzimg/r07-integrated sha256
  12e3b715…4b7f69 现算一致; green 门日志 26/26+SMOKE-DRIVER PASS+JTB ×3 rc=0+
  uname g4ca6ef1048f0。④ worktree: m9/perf2 已由原班移除, 现仅存 kcov(保留)+
  mva(**A.1 会话在制, 不动**); publish master 与 origin 同步(c93e51c), 本班仅补
  nobj 证据 commit。⑤ 决策: REPORT v1.4 头部(4ca6ef1048f0)不改——为该报告提交时点的
  事实, HEAD 后移系 A.0 落地（MV 轨道）, 刷新归 A 系列收口; 内核树未动未 push
  （无 push 远端）; 密码未落盘; VM vm-integrated(10032)/mva1(10033)/syzkaller 未触碰。
