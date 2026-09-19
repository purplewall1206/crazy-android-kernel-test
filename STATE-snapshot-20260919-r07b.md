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

