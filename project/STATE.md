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
速览（**2026-09-22 v1.5 校准**——旧速览停留在 09-13 属文档漂移, 本行对齐实际）:
M0-M6 全部 ✓ | M7 ✓（两轮终判 T2 DoD PASS, results/r08/m7-final.md; KCSAN 余项）|
M8 ✓（REPORT.md v1.5 = 冻结口径; G2/G7 已闭环, 报告表 7）| M9 P1 ✓ P2 ✓（P3 未）。
**M-V 轨道（D20 移除 VMA 层, specs/MV_VMA_FREE_SPEC.md, 9 切片）**: A.0 ✓(34f1ae661be3)
A.1 ✓(d40eae59ba76) | A.2a/A.2b 代码完成（worktree mva 未入库, patches/r07-mva2.diff
2709 行, 09-22 23:00 夜验自动排程）| A.3-V.E 未开始。内核树 HEAD=d40eae59ba76,
33 提交/29 tag; 项目仓库(remote crazy-android-kernel-test)已配置 push。

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
  （无 push 远端）; 密码未落盘; VM vm-integrated(10032)/mva1(10033)/syzkaller 未触碰。- **D20-b (2026-09-22 项目 git 重构)**: publish/ 目录删除——全部内容并入项目根 git 仓库
  （/home/ppw/cortenmm, remote=crazy-android-kernel-test: master=项目全量 882bb68 起,
  legacy-publish=旧 publish 历史 3261804 止, bundle 备份 attic/publish-history.bundle）。
  结构: specs/(四代设计规格) REPORT.md(根) log/ patches/ results/ bench/ bin/ docs/。
  .gitignore: bzimg//run//attic//*.pftrace/*.img/upstream 树/publish shim。
  env.sh 注释已更新；旧 08:10 cron 的 publish/REPORT.md 路径由 shim 符号链接兼容。


- **[2026-09-22 10:4x r07 maintainer: M-V A.1 ✓ —— park 去 VMA 手术落位]** 终审 PASS + 提交。
  终审焦点四项（只审 B1/B2 修复增量 + 2 KUnit + 死计数器, 主体前审 PASS 不重审）:
  ① **B1** shrinker pick 双解锁已修: `corten_arena_shrink_walk()` 每迭代恰一次
  pte_unmap_unlock（present/special 早退一处 + aging 写后一处）; `!vma` 臂 =
  `shrink_skipped` 计数 + 裸 continue（在 unlock 之后）, 与 pinned-refusal 逐字同构;
  eval/force 共用同一 walk/pick 门（force 跳 aging 但仍过 pick 门, 计数语义一致）。
  ② **B2** fork 复制臂门已修: 新 helper `corten_arena_span_has_vma()`（arena.c:3474,
  for_each_vma_range 首相交即真, mm_exit 预扫描同形）; 仅无树覆盖窗走
  `corten_arena_fork_copy_window_novma()`（:3704 门）; fork 路径 ar->vma 读仅余
  :3655 child-skip/:3665 pvma 镜像两处（其余 7 处=写点/debugfs/park 校验/unshadow/
  mm_exit 预扫描/accessor/shrink walk 合法）——arena 级旧门零残留, punch 幸存片/
  legacy 洞由 dup_mmap 已复制不再重复制; novma 臂错误路径双侧解锁齐全、`a += PAGE_SIZE`
  修复在位、swap 臂 AnonExclusive 清位/计数对称。③ **2 KUnit 真断言**:
  `vma_free_shrink_pick`（skip 计数基线前进 + PTE present + meta MAPPED + refcount==1,
  注释带修复前判别值）; `fork_punch_novma`（punch_split 全吞形态 + fork → 子
  MM_ANONPAGES==0 判别器 + refcount==2 + SHARED 落位）。④ **死计数器**
  `corten_nr_cascade_skips` 全树零引用（grep RC=1）; checkpatch --strict **0E/0W/9C**
  （2842 checked, CHECK=对齐/嵌套注记类）与终验班读数一致; patches/r07-mva1.diff 与
  工作树增量逐字节一致（2945 行）。
  提交: worktree mv-a0（分支 mv-a0）commit **d40eae59ba76** → 主树 ff-merge
  （HEAD=d40eae59ba76, tag **corten-r07-mva1**;
  patches/0001-mm-CortenMM-arena-de-VMA-parked-windows-M-V-A.1.patch;
  checkpatch = results/r07/checkpatch-mva1.txt）。
  bzimg/r07-mva1（bzImage sha 184bd250… = 终验构件 #39, green.txt 快验: 终验班留运行
  VM(10033, 同构件引导) maintainer 复跑 run_mode_smoke rc=0 **26/26** SMOKE-DRIVER PASS,
  arena ledger 0/0, dmesg corten 零 WARN/BUG, park_unmap_fails=0）。
  验证证据: results/r07/mva1-verify.md §7（=y 构建 / KUnit on x2 57 用例 + off + DAS +
  lockdep 变体全绿 / =n 十三对象 / lockdep 下 shrinker eval+force 活体: VMA-less 窗
  evict shrink_skipped 恰 +2048 内容存活 + 满量 force 35872 页 checksum 完好 /
  punch+punch+fork ×8 零泄漏 MemFree 对账 / guest 十门）。
  **终态**: S-1(MAPERR si_code 活体)/S-4(maps 消失+mincore 判据 6d20288d) 落地实证;
  S-2 不变; S-3 登记 V-D 复测。**已知边界移交 A.2a/V-D**: B-1 VMA-less 窗无 rmap 锚
  （reclaim 不可 pick/GUP-slow EFAULT/fork 软复制——A.2b carrier 恢复）; B-2 PUD/PMD
  上级表每窗 2 页残留（V-D exit walk; 级联已整体移除+计数删除）; B-3 chunk munmap
  语义不变。遗留: xa_destroy lockdep 基线 splat 的 __xa_destroy 顺手项仍未采纳
  （A.1 范围外 1 行, 留后续决策）。worktree mv-a0 = 主树 HEAD（分支现重合, A.2a 可复用）。
  未 push; 密码未落盘; VM 10033 留运行供复核。

- **[2026-09-22 14:2x r08 收官班（用户指令唤醒）: M7 收数终判 + 文档校准 + push]** 
  - **触发背景**: 原定 08:10 的「M7 收数与 M8 终稿」自动化未触发（runCount=0, 触发时刻
    会话不在运行）; syz-manager 09-22 06:28:22 起卡死于 VM-0 lost-connection 的
    reproducer（reproducing=1 永不退出, exec total 冻结于 3,114,125, VM 停止 1h 轮换）
    = 工具面挂起非内核 crash; 12:41 启动的 mva2-night.sh 亦已死（原会话退出被带走）。
    14:2x 用户指令手动执行全部收尾。
  - **M7 终判（results/r08/m7-final.md）**: 第二轮有效 22.4h（09-21 08:01:58 →
    09-22 06:28:24 冻结）/ **3,114,125 exec / coverage 17,859 / corpus 716**;
    全量 manager log **零 KASAN/BUG/WARNING**（grep=0）; 崩溃目录 6 = 第一轮遗留 4
    （e968b22d/1a6abe07 rcu stall, e9f5b119 no output, 439c37d2 lost connection——
    后两者经 dir 内 09-19 文件锚定归属首轮）+ 第二轮新增 2（107d4fad rcu stall do_idle
    预登记 + b83ebc0f suppressed report）, **全部环境类零 corten 内存安全 crash** →
    **M7.T2 DoD（连续 2 夜无可复现内存安全 crash）= PASS**。两轮合计 4.53M exec。
    停机: 14:20 TERM 477619 优雅退出 + syz qemu 自动清零（其余 10 台复核 VM 未动）。
    残余: KCSAN 未跑（计划中）; B4 lockdep corten=on 裁定并入今夜 mva2 kunit-lk;
    corpus 716 留作 M-V 收口后第三轮种子。
  - **夜验接力修复**: 14:22 setsid 重启 bin/mva2-night.sh（完全脱离会话, 日志
    results/r07/mva2/night-runner.log, state 12:41→14:22 重写）, timegate 休眠至
    23:00 自动跑 A.2a/A.2b 全矩阵（bzimage→KUnit on×2/off→=n 十四对象→DAS→lockdep
    →guest 门→checkpatch, 首红即停）。另排 09-23 08:10 自动收数。
  - **文档校准（v1.5）**: REPORT.md v1.5（M7 终数/终判 + M-V A.0/A.1 入账 §1.2 新行 +
    §9.1 M3/M4 提交行 + 树状态 HEAD d40eae59ba76 33 提交/29 tag + A8 半闭环）;
    STATE 里程碑速览块自 09-13 漂移刷新; docs/ROADMAP.md §1 现状表刷新 + M7.T2 ✓;
    specs/MV_VMA_FREE_SPEC.md 头部加实施进度行; log/20260922-r08.md 收官报。
  - **push**: 项目仓库本班提交后 push origin master（凭据已配）。
  - 边界: 内核树未动未 push（协议不变）; worktree mva 未触碰（A.2 增量在制）;
    VM 10033 留运行供复核。

- **D21 (2026-09-22 14:3x 用户直接指令·协议改制)**: **永久撤销日/夜双窗, 持续工作制**——
  "master prompt修改一下，不区分日间夜间，开始持续工作直到完成"。对 D14（09-18 单日
  豁免）的永久化; token 计费窗经济性考虑由用户明知并放弃。落地:
  ① MASTER_PROMPT **v3**（§2 日窗条款→持续工作制+配套纪律; 铁律 1 改写; §5 每夜循环
  →持续循环去钟点化; §8 启动指令当前切片指针刷新至 M-V A.2→V.E; 头部版本链）;
  ② bin/timegate.sh 退役为**直通桩**（保留文件兼容 mva2-night.sh 等调用点, 立即放行）;
  ③ 存量执行: 14:36 杀掉休眠至 23:00 的 mva2-night.sh 旧实例并重启 → **A.2a/A.2b
  验证矩阵即时开跑**（state: bzimage 步起跑, 首红即停）; ROADMAP §5 夜窗预算标注
  为历史存档。不变: 单写者锁/基线永存/诚实汇报/决策编号/PAPER_SPEC 权威链。
  ④ 09-23 08:10 收数自动化保留为兜底（prompt 已改幂等: 若已收口只复核+总结）。

- **D22 (2026-09-22 15:0x 主 agent 裁决·M9.P3 contpte/BBM 决议)**: 采纳调研班提案
  （next/m9p3-decision-draft.md, 204 行, file:line 级证据）的**方案③′叠加①验证尾**:
  ① OQ1 r1 的"desc 写锁 vs PTL 双锁写同批 PTE"竞态在当前树**已不以其原始形态存在**
  （arena 全部 12 个 PTE 触达点 PTL 已嵌套 desc 写锁内层, corten_arena.c:4728/:3373;
  全树无 PTL→desc 取序）——剩余 = lockdep 验证 + 文档钉死; ② "fold 永不进 arena"由
  巧合合力升格显式契约: __contpte_try_fold 顶部 mm->corten_mode 一行拒绝 +
  pte_valid_cont 检测器 + debugfs 计数（~130-250 行, 实施切片 = m9p3-s1）; ③ 方案②
  CONFIG 禁用 contpte 否决（全内核 TLB reach 代价不成比例）; ④ 任务书"fold/unfold 路由
  进事务"变体结构性不可行（contpte 全程 PTL 内取 desc 锁 = 锁序互逆死锁）; ⑤ BBM
  !vma 臂裸 set_pte_at（corten_arena.c:651）登记独立切片 **S5**。ARM64_PORTING.md
  OQ1 已回填终态指针。**实施排在 M-V 关键路径后**（mm/corten_arena.c 写冲突规避）。
  M9.P3 状态: 决议落定, 实施待排（M-V A.2 收口后可并行）。

- **D23 (2026-09-22 15:2x 主 agent 裁决·J2 审计三发现)**: 依据 next/j2-audit-draft.md
  （55 调用点全量分类: N 27 / R 8 / U 21, file:line 实测 @d40eae5）:
  ① **放置面破洞 = 规格未预见的真缺陷族, 列 A.3 最优先增量**: A.1 撤预约 VMA 后
  "窗口已占用"只剩帧表知道, 放置路径不问帧表——MAP_FIXED_NOREPLACE（mmap.c:466 +
  __mmap_prepare backstop 的 if(vms->vma) 条件洞）与 hint 放置（sys_x86_64.c:145/194 +
  mmap.c:811/862）可静默装 legacy VMA 入窗, 破坏 dominion 不变量+与 reactivate/punch
  冲突。修法 P1-P4 守卫（审计钩清单）随 A.3 首片落地。**当前 HEAD 已带此洞（parked 面）,
  A.2 合并将触发面扩到活跃窗——裁决: 照常合并, 边界 B-5 登记, A.3 首片热修**（零改动
  回归集不触发该面; 与 A.1 B-1/B-2/B-3 同惯例）。
  ② **A.2a 外部访问边界 B-4 登记**: A.2 入库后 process_vm_*/ptrace//proc/pid/mem/
  io_uring pin 对窗口 -EFAULT/0 字节（shadow-VMA 退役所致）; 审计建议"gup_probe 就绪
  为 A.2a 硬依赖"——**主 agent 裁决不阻塞**（外部访问面不在零改动 DoD 套件, J3 oracle
  本就排在 V-C）, V-C corten_gup_probe 为收口, V-B/V-C 顺序不变。
  ③ **S-5 登记**: mincore/madvise-parked/msync 三条 -ENOMEM 型语义回归（A.1 直接造成,
  原未进 S-1..S-4）, 随 A.3 路由扫尾收口（msync→0 对齐匿名语义/mincore 走 C11 裁决/
  madvise 空窗终答）。
  J2 审计钩五挂点 + INV-MV2 walker + 四观测计数器清单（审计报告尾部）= A.3 实施规格输入。

- **D24 (2026-09-22 15:2x 主 agent 裁决·A.3 三处前置 + A.2 边界补注)**: 依据
  next/va3-dev-brief.md（538 行, 主树/worktree 双行号）:
  ① **A.2 边界补注 B-5 精确化**: A.2 的 NOREPLACE 守卫（do_mmap:476-483）调
  corten_arena_range_overlaps **跳过 idle/parked 帧**（worktree arena.c:8849）——审计
  #14 的 parked 窗洞在 A.2 后仍敞开, 同根因隐藏成员 = plain MAP_FIXED 压 parked
  （punch 路由 lookup_get 同跳 idle）+ __mmap_prepare if(vms->vma) 条件洞（vma.c:2472
  worktree 未改）。**A.2 收口时按此登记, A.3a 热修收口**（~+260/−40）。
  ② 三处前置裁决（开发包决策表）: plain MAP_FIXED over parked = **idle-eject 放行**
  （避免 strace 新错误类, 对齐透明接管语义）; mincore 活跃窗 = **walk_page_range_novma
  真值走查**（OQ-MV-11 提前交付, 优于 -ENOMEM）; 植入白名单 = **per-mm interval tree
  登记**（结构性, 降级谓词抓不住空洞外来 VMA）。
  ③ A.3 切片定型 4 片: A.3a 放置热修 → A.3b J1 收口 → A.3c J2 walker（A 系出口首全绿
  点）→ A.3d S-5 三终答+C5/C6/C7 扫尾（超红线拆 C7 出 A.3e）; 总量 ~+800 内核/+450 测试。
  关键工程约束: "含 idle"查询必须**新增** corten_arena_range_occupied_incl_idle（勿改
  range_overlaps——skip-idle 语义被 16+ 拒族钩子依赖）; vma_lookup 走 mtree_load 不经
  find_vma, J1 网眼覆盖形状须在 REPORT 诚实披露; 计数器维持 atomic_long。

- **[2026-09-22 17:4x r07 maintainer: M-V A.2a/A.2b ✓ —— 复审 FAIL→修复→全绿→入库]** 
  - 提交: worktree mv-a0（分支 mv-a0）commit **217a9922a7d3** → 主树 ff-merge
  （HEAD=217a9922a7d3, tag **corten-r07-mva2**; 7 文件 +1695/−600; 34 提交/30 tag;
  patches/0001-mm-CortenMM-arena-auto-attach-without-VMAs-and-fork-c.patch）。
  - **复审史（诚实记录）**: next/mva2-review.md 判 FAIL——架构面全合格（carrier 忠实
  SPEC §2.3/fork 搬家白名单 #1/LSM 改判正确/INV6 零违反/=n 完整/legacy 零扰动）,
  但 **F1 阻断**（reactivate novma 条件不重武装 carrier → declare→park→re-declare
  混合态 fork 静默丢子窗内容, uapi 可达, 夜矩阵测不到）+ **C1**（子 carrier 孤儿
  anon_vma 在 DEBUG_VM 下 WARN——**夜矩阵四 config 均 DEBUG_VM=n = 配置盲区**）
  + **C2**（mremap_move 丢 may_expand_vm 门）。修复班实施全部三项 + 真值表 48 例
  + fork_redeclare 逐页对拍锚; 中途遇限额中断, 主会话补完接线（page_word 前置
  声明/用例表注册/删脚手架）后完整复验。
  - **终验矩阵全绿**: build 0E | KUnit on×2 24/0/1 + plain 25/0/0 + das + lk（一次过）
  | =n 十四对象 | **DEBUG_VM 变体 24/0/1（复跑判定, num_active_vmas=0 = C1 实证）**
  | guest 门 PASS=23 FAIL=0（含 A.2 新锚: J1 纯 MODE 负载零命中/carriers 5→25/
  auto_vgate）| checkpatch **0E/0W/6C**（3182 行）。bzimg/r07-mva2（终件 a4f22255…
  + prefix-base 对照 d25b7639…）+ green.txt 登记。interlock flake 本日累计 3 次首跑
  （lk/dvm/dvm2 复跑全绿, M7 口径维持; DEBUG_VM/lk 开销拉长时窗的机理再次印证）。
  - **边界注册**: B-4（外部访问面→V-C）/ B-5（放置面 parked 窗洞→A.3a）, 见 D23/D24。
  - **运维事件账（本日 A.2 线全程）**: 夜脚本 guest-boot 行缺 systemd.mask/corten=on/
  kunit.enable=0（emergency mode, 续跑脚本已修）; trixie.img 9p 不自动挂载（STATE 坑
  清单既有条目, 手动挂载）; pkill 模式串自匹配杀自会话一次（老教训重演）; 7 台已收口
  切片旧复核 VM 按 D21 自律清退; vm/vm-integrated/vm-mva1 三台在矩阵窗口内死亡
  （归因未定, 证据在档, vm-mva1 将以 r07-mva2 终件重启供复核）。
  - **下一片**: A.3a（放置面热修, next/va3-dev-brief.md）与 V-B B.1（FILE 区,
  next/vb-dev-brief.md）双轨并行（分 worktree, 顺序合并）。

- **[2026-09-22 19:2x r07 maintainer: M-V A.3a ✓ —— 放置面热修入库]** 
  - 提交: worktree mv-a0 commit **788cb26e36c0** → 主树 ff-merge（HEAD=788cb26e36c0,
  tag **corten-r07-mva3a**; 6 文件 +1068/−30; 35 提交/31 tag）。
  - 内容: incl-idle 占用真源 API + NOREPLACE 换弹药 + MAP_FIXED idle-eject 放行 +
  backstop 零 VMA 臂 + P4 断言 + 植入登记（排序数组简化形态）。**审计 #14/#15/#16
  放置洞全部收口**。伴随修复: pool eject 漏摘 obs 账本的潜在 UAF（KUnit 实测 GPF
  后修, 本片 eject 路径使其变热）。
  - **D25 (三处 D24 偏移追认 + 覆盖盲区教训)**: ① erase 变体 eject（防 backstop
  自拒与杂志复发牌）② 哨兵不拒（对齐杂志 T0 obstacle 契约 :2813/:2873）③ P4
  targeted 豁免（targeted 声明 VMA 合法）——均 accept, A.3c 复议。④ **覆盖盲区
  教训（重要）**: mva2-verify.sh 的 filter_glob=corten 只匹配字面套件, **A.2 全部
  夜验实际只跑了 corten 主套件, corten_arena/corten_fault 从未运行**——该坑
  09-21 就在 STATE 登记（"三套件口径须 filter_glob=corten*"）却被 A.2 会话的
  新脚本重新引入; 已修脚本 glob（c069fad）。后果核账: A.2 增量经 A.3a 验证的
  三套件跑补覆盖（arena 67/0/0 含 A.2 全部用例）; 唯 fork_redeclare 在纯 A.2
  基座上即红（测试自身形状缺陷: PAGE_SIZE munmap 永不 park 8M 窗, 与 F1 产品
  修复无关）, A.3a 已修测试形状并实证绿。**铁律补条: 新脚本复用旧坑清单,
  verify 脚本 glob 三套件口径入 checklist**。
  - 验证: KUnit 三套件×2 零 flake + =n 15 对象 + checkpatch 0E/0W/0C + guest 门
  23/0（19:17）。bzimg/r07-mva3a（e0c6e400…）+ green.txt 登记。
  - V-B B.1（mvb worktree）仍在制; A.3b（J1 收口）待 V-B B.1 落地后接续（quota
  窗口管理: 双 agent 并行上限 2, 顺序复审入库）。

- **[2026-09-22 20:2x r07 maintainer: M-V V-B.1 ✓ —— FILE 区第一片入库（双轨第二轨）]** 
  - 提交: worktree mvb（分支 mv-b, rebase 至 A.3a）commit **c8260c0487d4** → 主树
  ff-merge（HEAD=c8260c0487d4, tag **corten-r07-mvb1**; 6 文件 +1308/−99;
  36 提交/32 tag）。
  - 内容: corten_file_may（do_mmap 文件链逐臂重放）+ FILE carrier 形态 + i_mmap
  参与（__vma_link_file 逐字）+ region 全区段 FILE_MAPPED 虚拟分配（防 FRESH
  合成零页）+ teardown/park/fork 镜像/INV-MV3(d) + 引用收支差分锚。
  - **D26 (FILE 接管暗门)**: B.1 机壳完整但 fault 臂在 B.3, 为满足"每片收口可
  运行"铁律, 路由层对 CORTEN_MMAP_AUTO_FILE 默认降级 legacy（dlopen 形态必须
  持续可工作）; 机壳经 corten_file_route_test_override 由 KUnit 直驱全测;
  B.3 落地时翻门并删 override。guest 门实证暗门有效（JTB/java 全 rc=0）。
  - 合并手术记录: rebase 至 A.3a 时 fork_redeclare 双修法冲突（A.3a 整窗版胜出,
  B.1 的 LEN−PAGE_SIZE 版弃）+ 3 处 auto_mmap_route 新签名适配（A.3a 新测试
  旧调用, rebase 缝隙）+ file_lifecycle 路由断言与暗门的交互（override 解）。
  - 验证: 三套件 ×2 全同（corten 24/0/1 / arena 70/0/0 / fault 31/0/2）+ =n 15
  对象零符号 + guest 门 23/0 + checkpatch 0E/0W/0C（1898 行, 全量 git diff HEAD
  口径——首跑 74 行教训: stash 恢复部分进暂存区, plain git diff 漏量, 已复验）。
  - **今日累计: 四片入库**（A.2a/A.2b 217a9922 + A.3a 788cb26e + V-B.1 c8260c04,
  另 M7 收官/KCSAN 闭环/D21-D26 六决策）。M-V 进度: A 系列 5/5 片中 4.5（余
  A.3b-d）+ V-B 1/4; 下一片 B.2（truncate 门+i_mmap 失效, 依赖 B.1 已就绪）。

- **[2026-09-23 00:1x r07 maintainer: M-V V-B.2 ✓ —— truncate/失效路由门入库（今日第五片）]** 
  - 提交: worktree mvb commit **874fa46c23b0** → 主树 ff-merge（HEAD=874fa46c23b0,
  tag **corten-r07-mvb2**; 5 文件 +420/−1; 37 提交/33 tag）。
  - 内容: unmap_mapping_range_vma/folio 路由门（carrier→chunk-zap 事务, KEEP_PERM
  降级 Invalid）+ zap_page_range_single 防御 backstop（拒+计+WARN）+ 六类测试电池。
  锁序声明 i_mmap_read > desc > ptl。B.1 的 i_mmap 参与从此有了 INV6 对侧。
  - **过程**: dev agent 21:47 死于 1302 速率限制（半成品 +420）, 主会话审计确认
  完成度（六类锚已全, 仅欠验证）→ 验证链（三套件含 lockdep 首跑 1 例登记 flake
  复跑绿 + =n + guest 23/0 + checkpatch 0E/0W/0C 497 行）→ 入库。B.1 遗留的
  auto_classify_file 重复注册由本片顺带清理。
  - B.3 已并行开工（mvc worktree 预铺 B.2 增量 + fault 双臂开发 agent 在制;
  收口时用 mvc-vs-main-HEAD 差量口径分离 B.3 部分, 勿重复提交 B.2）。
  - 今日(09-22)累计五片: A.2a/A.2b + A.3a + V-B.1 + V-B.2 + 昨夜 A.1; M-V 剩
  B.3(在制)/B.4/A.3b-d/V-C/V-D/V-E。

- **[2026-09-23 01:0x r07 maintainer: M-V V-B.3 ✓ —— FILE fault 双臂 + D26 暗门翻开（第六片）]** 
  - 提交: worktree mvc（差量隔离自 B.2 预铺）commit **d4badaed5b7d** → 主树 ff-merge
  （HEAD=d4badaed5b7d, tag **corten-r07-mvb3**; 6 文件 +1162/−75; 38 提交/34 tag）。
  - **FILE 轨激活**: fetch（shmem SGP_CACHE / 常规 FGP_CREAT+filler, 双复查）→ read
  （重锁 re-query 事务装页）→ cow（FILE_MAPPED→MAPPED 迁移）→ FRESH rclass-aware
  （H4 红线: FILE 区空槽重读文件, 化零页合成结构性禁止）→ CORTEN_FAULT_BUS 贯通
  → even_cows 接线（B.2 预埋参数成为可达语义）→ 暗门+override 全删（零残留）。
  - **里程碑意义**: dlopen/JVM 库形态的 addr==0 私有 file 映射从本片起真走 arena
  FILE 轨——V-B 三片（B.1 记录/B.2 失效/B.3 fault）闭环, MV spec §3.2 主体达成。
  - 验证: 5 连续 boot 三套件全绿 + off 零失败 + =n×2 + checkpatch 0E/0W/0C +
  **翻门 guest 门 PASS=23 FAIL=0**（JTB/java/metis 首跑 FILE 轨全绿, fork 交织
  预判风险未咬）。
  - 披露移交: fork+GUP-slow FILE 页交织（folio_try_dup_file_rmap_pte 路径）快钩
  KUnit 已覆盖、fork 对拍是 B.4 面; 真 fs read_folio 臂本片首跑; 显式 hugetlbfs
  fd 无 MAP_HUGETLB 位可过 file_may 且 fetch 时 BUS——登记白名单审计项（A.3/B.4）。
  - dev agent 过程: 42M tokens 全自验（含 on1 首跑 2 例 percpu batch 可见性假影
  改 percpu_counter_sum_positive 后归零——测试侧修正）; 六项风险清单在
  next/mvb3-dev-report.md。

- **D27 (2026-09-23 01:5x 用户直接指令·内核代码上 GitHub)**: 主树推送策略变更——为方便
  用户直视进展, 内核提交序列发布到 crazy-android-kernel-test 仓库 **corten-github**
  分支（**孤儿分支形态**: AOSP 基线树单快照 ce6778ba + 36 个项目提交原样重放,
  终树与主分支逐字节等价 0 diff; 全量 android17-6.18 历史 3.85GiB 留本地不推;
  远端名=github, 主分支 android17-6.18 本尊不动, tag 仍锚定本尊哈希留本地）。
  **此后每片入库收口时同步 `git -C /home/ppw/linux-6.18 push github
  corten-github`**（重放方式: `git rebase --onto <orphan> 68974e23 corten-github
  主分支新HEAD` 后 corten-github 指向结果, 勿直接 rebase 主分支——本次险情实录:
  rebase 误传主分支名导致主分支哈希链被重写, 已即时 reset --hard 恢复, 树零差异,
  34 tag 完好; 教训入坑清单: 重放操作永远在临时 checkout 上做）。

- **[2026-09-23 02:5x r07 maintainer: M-V V-B.4 ✓ —— FILE 系列闭环（第七片, V-B 4/4 全齐）]** 
  - 提交: worktree mvb commit **c4d3b55f5959** → 主树 ff-merge（HEAD=c4d3b55f5959,
  tag **corten-r07-mvb4**; 3 文件 +854/−26; 39 提交/35 tag）。corten-github 已同步
  （43059f65, D27 重放在临时 detached checkout 上完成——上轮险情教训落地）。
  - 内容: fork_mark_window pinned-aware 修复（AnonExclusive 私有形态不标 SHARED,
  消 INV7 可见漂移）+ rss 记账族对齐 mm_counter_file（shmem→SHMEMPAGES, 修子 mm
  exit 的 "Bad rss-counter state" BUG——fork 对拍差分锚实测揪出）+ hugetlbfs 白名单
  门 + 四计数器/arenas 台账 rfile/poff 列 + 4 新 KUnit（72→76）。
  - 基座事实修正登记: 6.18 已删 folio_try_dup_file_rmap_pte（file rmap fork 复制
  无条件化）, "临时拷贝"形态仅 anon 路径可达; MMF_HAS_PINNED 快门为 pinned 锚首跑
  实测教训。
  - 验证: 5 连续 boot 全绿 + off 零扰动 + =n 零符号 + checkpatch 0E/0W/0C + guest
  门 PASS=23 FAIL=0（含 no rss-counter regressions 专项）。
  - **V-B 全系列（B.1 记录/B.2 失效/B.3 fault/B.4 fork）闭环——MV spec §3.2 FILE 区
  主体达成**。下一片: A.3b（J1 收口, mva worktree）; M-V 剩 A.3b/c/d + V-C/V-D/V-E。

- **[2026-09-23 04:4x r07 maintainer: M-V A.3b ✓ —— J1 收口 + fault 终答（第八片）]** 
  - 提交: worktree mva commit **51b4b09f7751** → 主树 ff-merge（HEAD=51b4b09f7751,
  tag **corten-r07-mva3b**; 11 文件 +685/−22; 40 提交/36 tag）。corten-github 已同步
  （c5162349）。
  - 内容: J1 五挂点齐（补 find_vma_prev/find_vma_and_prepare_anon）+ walker 查窗前置
  （bottomup/generic 双修, 消 hint 打爆面）+ corten 内部豁免别名扫尾 + fault 慢路径
  窗口终答 + uffd 双腿 -ENOENT + 四观测计数器。S-5 三终答按红线纪律留 A.3d（登记）。
  - **修复史（gate 五轮迭代, 诚实全录）**: ① 首跑 smoke 14 步死——进 VM 手跑拿 segfault
  at 0x100000000000 → 终答漏植入洞形状（idle-eject/punch 产物被误 MAPERR）→ 补
  implant_covers 放行; ② 三跑同败 → 根因是空窗段 MAP_FIXED 的 legacy VMA 根本没登记
  （P1b 登记臂不覆盖）→ placement backstop 补登记臂 = INV-MV2 生产者集合补全;
  ③ KUnit occupied_incl_idle 崩（NULL+0x58）→ **corten_addr_in_window 是
  current->mm 语义的入口助手, kthread 直驱语境 NULL-mm 崩** → 改参数 mm 纯几何
  判定。三修全随片入库。
  - 运维坑新增: 入口助手 vs 参数化判定不可混用; gate 失败路径留 VM 用 qemu.pid 定向
  清理; pkill 模式串含端口号亦自匹配（坑清单第二次变体实锤）。
  - M-V 进度: **8/12 片**（A.0/A.1/A.2a/A.2b/A.3a/A.3b/B.1-B.4 全落）。剩 A.3c（J2
  walker, A 系出口）/A.3d/V-C/V-D/V-E。

- **[2026-09-23 05:5x r07 maintainer: M-V A.3c ✓ —— J2 walker + audit 门（第九片, A 系出口在望）]** 
  - 提交: worktree mva commit **76a83f8e0d03** → 主树 ff-merge（HEAD=76a83f8e0d03,
  tag **corten-r07-mva3c**; 6 文件 +999/−24; 41 提交/37 tag）。corten-github 同步
  （eb66985b）。
  - 内容: INV-MV2 活化（九触发点: mm_exit/fork_commit 常开 + 七热路径采样键）+
  audit_gate 一站式读出 + stale 分类谓词 + 5 新锚。**伴随 4 处登记闭环修复**（stale
  锚红灯挖出: ENTER-only mm 洞/backstop 快否定绕过/下界裁剪加固/**fork 镜像植入
  登记**——A.3b gate bug 的 child 侧变体, dup_mmap 植入 VMA 无登记则子终答 MAPERR +
  walker 永误报）。
  - **audit_gate 首测（guest 全电池）**: j2 27 walks 零违例零 stale, first_violation
  0x0——INV-MV2 在真实负载下成立。j1 全程恰 1 命中（smoke 植入契约形状, 合法）;
  **严格零门 = A.3d 出口**, 届时 J1 hooks 豁免登记覆盖地址即可达零。
  - M-V 进度: **9/12 片**。剩 A.3d（S-5 三终答 + J1 豁免收零, A 系终出口）/V-C/V-D/V-E。

- **[2026-09-23 07:2x r07 maintainer: M-V A.3d ✓ —— ★ A 系列出口达成（第十片）]** 
  - 提交: worktree mva commit **f4ffec5e0005** → 主树 ff-merge（HEAD=f4ffec5e0005,
  tag **corten-r07-mva3d** + 里程碑 tag **corten-r07-a-exit**; 7 文件 +1067/−12;
  42 提交/39 tag）。corten-github 同步。
  - 内容: J1 豁免收零（登记表无锁快照协议: krealloc→kmalloc+memcpy+kfree_rcu_mightsleep
  退休+发布屏障; 功能路径保持持锁版）+ S-5 三终答（msync 0/mincore PMD 门控真值走查/
  madvise parked 终答）+ move_pages 短路 + 四计数器。
  - **★ A 系列出口判据达成**（guest 电池严格读数）: gate_pass==1 ∧ j1_hits==0 ∧
  j1_probes==0 ∧ j2 27walks/0viol/0stale——**窗口域的 VMA 查找族调用归零,
  INV-MV2 活且零违例**。V-A 全family（8 提交: A.0 region/A.1 parked 去区/A.2 carrier/
  A.3a 放置守卫/A.3b J1 网/A.3c J2 walker/A.3d 终答）按 MV_VMA_FREE_SPEC §3.1 全落。
  - M-V 进度: **10/12 片**。剩 V-C（proc 双源+GUP-slow+J3 oracle）→ V-D（exit 走查）→
  V-E（brk+终判据）。

- **[2026-09-23 09:4x r07 maintainer: M-V V-C ✓ —— proc 双源 + GUP 探针（第十一片, B-4 关闭）]** 
  - 提交: worktree mvb commit **8c4706e1e862** → 主树 ff-merge（HEAD=8c4706e1e862,
  tag **corten-r07-mvc**; 8 文件 +1679/−48; 43 提交/40 tag）。corten-github 同步。
  - 内容: GUP-slow 窗口探针（carrier 应答, 外部访问面恢复=B-4 边界关闭）+ proc 全套
  双源（maps/smaps/pagemap/PROCMAP_QUERY/numa_maps）+ J3 oracle 件。
  - **修复史（oracle 实测价值实证）**: 首行丢失真 bug（游标行推进先于渲染——每行渲染
  成下一 region 跨度: 首行永不输出/末行重复/树头被吞/seq 回卷不变量破; 单 region
  探针自洽故旧测试全盲, 多 region oracle 必现）+ PROCMAP_QUERY ioctl 号错配（ENOTTY）。
  修法 = peek 头 + promote-再推进 + 树头归还 + last_pos 不变量恢复; 新归并序 KUnit 锚。
  - 判定: 电池 22/1 的唯一 FAIL = mva1_probe CHUNK 段数（**登记的 A.1 过时预期**:
  V-C 渲染存活块是设计行为; oracle 行断言接防真语义; probe 源码已失, 若 V-E 需要
  重建为 V-C 感知版）。严格门维持（全电池 j1_probes=0）。
  - residual 登记: PROCMAP_QUERY or-next 族 find_vma 残差 + bpf_iter/trace 符号化
  (#37-40) 单源——V-E 硬化项; 跨内核 J3 字节对拍（cmp_j3.sh）排 V-E 终判据电池。
  - M-V 进度: **11/12 片**。剩 V-D（exit 纯 PT 走查+上层表自拆+swapoff 复测）→
  V-E（brk 裁决+终判据+REPORT M-V 章）。

- **[2026-09-23 15:3x in-flight 心跳（GitHub 可见性）]**: V-D 猎漏 agent 存活迭代中
  （punchfork 交错形状的 4×4096B PTE 泄漏, 增量 +954/−83 且分钟级在长, KUnit 复现锚
  编写中）。内核树静默 = 红判据不入库纪律; 项目仓本条即为活跃心跳。V-D 绿判据
  （dmesg pgtables==0）达标即 commit+merge+双仓推送。队列: V-E 终章其后。

- **[2026-09-23 15:4x r07 maintainer: M-V V-D ✓ —— exit 纯 PT 走查 + ★ B-2 收口（第十二片）]** 
  - 提交: worktree mva commit **309674279b1d** → 主树 ff-merge（HEAD=309674279b1d,
  tag **corten-r07-mvd**; 5 文件 +954/−83; 44 提交/41 tag）。corten-github 同步。
  - 内容: exit 走查两相位（A 逐窗 PTE 退役 / B 上级表三趟自拆 PMD→PUD→p4d）+
  punch 幸存窗泄漏修复 + S-3 unuse 盲区计数披露。
  - **★ B-2 收口**: guest 电池 dmesg "non-zero pgtables_bytes" 归零（收敛史 25→4→0;
  最后一层根因 = 共享 PUD 页评审与 PMD 退役交错, metis 三-PUD-段形状 4 次/电池,
  猎漏 agent 4.4h 定位 + KUnit red-before/green-after 锚）。A.1 挂账至今的遗留清账。
  - 验证: KUnit 97/0/0（+2 锚）+ =n + checkpatch 0E/0W/0C + guest 门（电池 22/1
  登记 CHUNK 容差 + 严格门维持 + B-2 归零 + PageTables 稳定）。
  - M-V 进度: **12 片计划的第 12 片在制前置完成——只剩 V-E 终章**（brk 裁决 + 终判据
  电池(J1-J4 全绿 + 零改动回归集 + 跨内核 J3 对拍 + S-3 swapoff 复测) + REPORT M-V 章）。

- **[2026-09-23 19:5x r07 maintainer: M-V V-E ✓ —— ★★ M-V 全系列完成（第十三片, 终章）]** 
  - 提交: worktree mvb commit **037bfbaed020** → 主树 ff-merge（HEAD=037bfbaed020,
  tag **corten-r07-mve** + 里程碑 tag **corten-mv-complete**; 6 文件 +739/−5;
  45 提交/43 tag）。corten-github 同步。
  - 内容: brk 四臂账本 + J1 heap 探针（OQ-MV-7 分子: guest 实测 +893; 四臂接线全证;
  **裁决 = 委托域维持, V-E.2 不立项**）+ 白名单九类分类器 + 双活挂点 + wl 账本 +
  gate_pass 完整形态（KUnit 99 锚）。
  - **★★ M-V 终判据全绿**: J1 严格零 / J2 六项(手动+exit 走查/账本零/双走查器交叉
  核验) / J3 in-boot oracle / J4 六态+gup_probes / brk 接线证明 / S-3 复测定性 /
  零改动回归集 + 严格门全程维持。SKIP 三项全部登记(bpftrace×2 无 BTF、J3 跨内核
  腿 A.1 快照后补)。
  - **S-3 实测发现登记**: read-back 期间 swapins 平坦——窗口换入路径的语义问题,
  V-D 报告的 unuse 枚举升级臂(~60 行)留档待裁; 不阻塞 M-V DoD（swapoff 本身干净）。
  - 电池件三轮修复史（诚实）: hold 负载 sbrk(0) off-by-one（写 break 本身, 页对齐
  时必炸, 伪装成内核回归——A/B 归档镜像一跑还了内核清白）/ j2 真pid 解析与 kill
  目标（包装进程混淆两变体）/ s3 set-u 字符串。
  - **M-V 12 切片全部落地**（A.0/A.1/A.2a/A.2b/A.3a-d/B.1-B.4/C/D/E, 2026-09-22
  07:1x A.0 起至本片 36 小时）。D20 指令"移除整个 VMA 层"按 D20-a 诚实化口径完成:
  窗口域零 VMA、委托遗留域白名单化、单内核双 MM 形态。REPORT M-V 章回填随即。

- **D28 (2026-09-24 用户直接指令·推翻 D20-a 保留)**: "说要完全移除 VMA 就是要完全移除"——
  字面完全移除取代 D20-a 诚实化口径: 委托域白名单与 detached carrier 两处保留均不接受,
  终态 = MODE 进程 maple 树条目数 0（含 carrier 的 vm_area_struct 分配恒 0）。ADV 锁协议
  一并重开: 现状 = 设计评估已落盘（完整 _adv 裁决不做, 理由"对象不存在"）+ ADV-1 微片未做;
  按 ADV 设计文档自身的条件翻转点条款, W-1 rmap 重写后重评（先量测后动码）。
  规格落盘 specs/MV2_FULL_REMOVAL_SPEC.md（W-1 原生 rmap 拱心石 → W-2 carrier 消灭 →
  W-3 委托域迁移四件 → W-4 入场扫入 → W-5 植入消灭 → W-6 终判据 + ADV-1 随手）。

- **D29 (2026-09-24 用户终局目标定型)**: 两条——① 完整实现论文的优化思路（事务化热路径
  + 零 VMA 运行的全部主张, 已大部达成, 缺口=性能地板与字面移除, 见 MV2）; ② **对所有应用
  程序无损完整接管**——不再是 LD_PRELOAD 按进程自愿进场, 而是 corten=on 时全部进程自动
  接管（exec/mm 创建即 MODE, 含 static/suid/系统服务）, 全 syscall 语义无损。路线图影响:
  W 系列（MV2 字面移除）是前置（不迁完 exec/brk/栈/vdso 无法全程接管）, 其后 MV3 系列 =
  默认接管（exec 自动进场 + guest 全程 MODE 启动门 + N 类缺口从"登记"升级到"无损闭合"
  + mmap-pf 地板的批 mark 重构升为必做——默认接管后它是所有进程的税）+ 届时 VMA 层对
  全部进程成为死代码, 删除账本兑现。

- **[2026-09-24~25 W-1 全系列 ✓ —— 原生 rmap 拱心石落地（MV2 片 1/6）]** 七提交+tag 全入库:
  W1.a 536635d7879d（vma-free rmap 包装, folio_add/remove_anon_rmap_novma + file 侧
  novma install/remove——rmap 记账与 vma 解耦的接口面, +251）→ W1.b 0d27489f4781
  （per-inode file registry + invalidation 枚举: truncate/ invalidate 重锚定走查从
  i_mmap 改 registry 驱动, +656/−248）→ W1.c 3645f8d51e75（file install/remove 生产
  调用面翻 novma 包装）→ W1.d 430de7155f3e（ttu file 真路由: rmap_walk_file 对窗口
  folio 从 registry 枚举, +669）→ W1.e1 b02ffc85af27（原生匿名换出驱动: 驱动直接
  scan→swap-out, 不经 shrinker 通道, +616/−37）→ W1.e2 82d182cd4989（ttu 匿名守卫
  翻转到驱动——**W-1 完成** tag corten-r07-w1-complete; 实测 512MiB 全量经 ttu→驱动
  直驱 driver_swapped=131072, +598/−308）→ W1.f/f2 1b58850d59ea（unuse 窗口扫描 +
  swapcache pull 臂: 真盘 unuse 的 SWAP_HAS_CACHE 滞留形状在 pull 内改道 unuse_pte
  语义 cached-folio map, 一次 unuse_mm 收敛, +793/−24）。规格=specs/W1_NATIVE_RMAP_SPEC.md;
  各片报告 next/w1{a,b,c,d,e1,e2,f,f2}-*.md。验证口径: 各片三套件 KUnit on×2/off 全绿
  （105/0/0 → 104/0/0 等, 逐片 +1~2 锚）、=n 十四对象零符号、checkpatch 0E/0W/0C;
  guest 门 22/1（唯一 FAIL=登记的 A.1 CHUNK 容差, mva1_probe 源码已失的过时预期）;
  w1e2 门 ★ttu 匿名压测翻转实证。**移交**: S-3 read-back swapins 平坦的语义问题
  （mve 登记）由 W1.f2 收敛; futex 换出边界（见 W-3 条目）。
- **[2026-09-24 21:23 W-2 ✓ —— carrier 消灭, MODE vma 分配恒 0（MV2 片 2/6）]**
  commit **680857104271** tag **corten-r07-w2**（8 文件 +1384/−977; 报告
  next/w2-dev-report.md）。判定: X（子侧不复制 PTE 首 fault 原生路径）否决——匿名页
  无 swap entry 无 pagecache 挂点, lazy-dup 状态机语义不无损; Y（carrier 降纯元数据锚）
  采纳——W1.a-e 已拆完全部职责, W-2 只剩删壳。fork_copy_ptes 纯 metadata 化 / GUP-slow
  摘 carrier（folio 操作改 metadata 驱动）/ rmap.c+gup.c 生产面改造。终判据:
  **MODE mm 的 vm_area_struct 分配数恒 0（含 detached carrier）**——D28 操作判据的前半
  （分配面）达成。验证: 三套件 on×2/off 全绿（104/0/0 基线同数）、=n、checkpatch 0E/0W/0C
  （3658 行）、guest 门 22/1（登记容差）。worktree mva 未提交增量即本片主体。
- **[2026-09-25 09:57 W-3 ✓ —— 委托域迁移 + GUP 重构（MV2 片 3/6）]** commit
  **669a33f84279** tag **corten-r07-w3**（7 文件 +1867/−32; 报告 next/w3-dev-report.md;
  判定先行的四件裁决见报告 §0）。内容: ①brk region 化（V-E.2 复活: sys_brk GROW/SHRINK
  路由 adopt/seed/extend/trim/release, 全部 fail-open legacy; [C1] 红线=入场前已驻留堆
  不收, brk_legacy 计数披露, W-4 扫入重开）; ②线程栈 MAP_STACK 白名单翻转（guard 页
  CHUNK 事务）; ③exec 镜像判定为 W-4 扫入承接（ELF 形状全可表出为 file region record,
  本片零 binfmt 改动）; ④vdso/vvar 结构性排除+计数（timens 走查/mremap ABI/PFNMAP
  证据链, CORTEN_WL_SPECIAL 桶, D28 树归零判据豁免口径移交 W-4/W-6 裁定）; ⑤**邻接
  精度件（隐藏主件）**: 堆 region 页粒度边界共享 data 段 PMD 帧, corten_route_hit 字节
  级判定补七处范围路由, 否则邻接 mprotect/munmap/MAP_FIXED 硬失败; ⑥**W-3.2 GUP 重构**:
  __get_user_pages 三路显式流（窗口→corten 臂/legacy 链/真树缺→EFAULT）, check_vma_
  flags/follow_page_mask/faultin_page 的 vma-NULL 控制流不可达; gup_fast 汇 + futex
  arena-anon key 臂同系列。验证: 三套件绿（corten_arena 105/0/0 +1 路由锚; corten_fault
  34/0/5）、=n 14 对象、checkpatch 0E/0W/0C（992 行）; guest 电池 17/6（pre-fix 构建,
  metis_eq Aborted 一致复现=独立 bug 见下）→ post-fix 构建 smoke 26/26 + JTB 绿;
  严格门维持（gate_pass==1, j1_hits==0）; brk_legacy=2882 披露（glibc 电池, W-4 靶面）。
  **已知开口（登记独立件）**: 窗口 futex 在换出边界 EFAULT——驻留页 futex 正常, 换出页
  WAIT 得 EFAULT; 修复需 futex 路径感知 arena swap 形状。W-3b/W-3c 移交与 metis 门
  核验状态见下一班条目。

- **[2026-09-26 01-03 时 W-3 收口核验班: metis 门根因修正+双修复入库 (W-3fix)]**
  任务 2（核验 W-3b/W-3c 与 metis 门）执行中发现并修复两枚真缺陷:
  - **metis 门 = NOT MET 于 W-3 终件**（复现: rc=134 "futex facility" ×2, 而
    swapped_out==0——**W-3 的换出边界归因对本失败不成立**）。实证链: strace 定位
    `futex(0x100001000990, WAIT_BITSET|CLOCK_RT, tid)=-EFAULT` 落在 glibc 线程栈
    映射（PROT_NONE MAP_STACK declare + 顶片 mprotect RW）内; kprobe get_futex_key
    ret=0xfffffff2; gup_probe_rejects 每复现 +4 → **失败点=corten_gup_window 的
    check_vma_flags 仿真**: CHUNK mprotect 只把 perm 提交进槽位（pending-perm）,
    ar->prot 留在 DECLARE 界, 探针只读 ar->prot → STACK region 的 FOLL_WRITE 全拒。
    修=探针对齐 FRESH 门规则（m.perm ?: ar->prot, corten_ptdesc_get RCU 钉住读;
    生产者持 mmap_write/GUP 持 mmap_read 同 mm 互斥）。**修后 metis 双跑 rc=0,
    checksum 与无 hook 跑同值（零改动契约）**; KUnit 锚 w3_gup_chunk_promoted_perm
    红→绿。
  - **exit walk 终段帧退休泄漏**（pgtleak 探针族: 每 MODE 退出 4096/8192
    pgtables_bytes BUG——W 系列把 V-D 的 B-2 归零打回）: phase A 终段 run 以
    arena->end（字节粒度）收尾, free_ptes_span 整帧守卫跳过末帧 → PTE 页搁浅
    （heap region 跨帧形状=printf-only 即可复现 + 全部非帧对齐 end 窗口）。修=zap
    保持记录跨度、退休伸展到帧界（末帧已验证 walkable）。修后全部探针形态 dmesg
    泄漏=0; brk_region_exit 锚扩展（fill 末帧+mm_exit 后 pmd 条目非 present）。
  - 入库: 主树 **d7bd0dd9b5c9** + worktree 平行 e6afdde09eda, tag
    **corten-r07-w3fix**（锚主树）。验证: 三套件 on×2（24/0/1 · **111/0/0** ·
    34/0/5）+ off（25/0/0 · 24/0/87 · 7/0/32）全绿; checkpatch 0E/0W/1C; guest
    电池 22/1（唯一 FAIL=登记 carrier 计数退役容差）+ 严格门维持（gate_pass==1,
    j1_hits==0）+ 零 pgtables 残差。证据 results/r07/w3fix/（含 kunit/门日志+
    修复 diff）; **补齐 W-3 欠交的 futex-swapout-boundary.md**（results/r07/w3/
    与 w3fix/ 双份）。
  - **核验结论（任务 2 定案）**: W-3b（主栈 GROWSDOWN 臂）维持移交——依赖 W-4
    迁移事务机（本班未动）; W-3c（resident VMA→region 迁移事务机）= W-4 主体,
    本班为它清了 exit/perm 两处地基。**新登记硬阻断项**: 换出后窗口页的用户态
    fault 既不换入也无干净裁决（swapins==0, 进程静默死亡; S-3 swapoff 电池腿自
    W1.f 起持续 rc=1 同族; 回归窗=W1.e2 后, 未二分）——W-6 前必修, 首分叉点
    kprobe corten_arena_swap_in 是否被 dispatch 触达。窗口植入的 exit 上层残差
    （8192）登记为 W-5 靶面。探针件 bench/share/w45/（futexprobe/futexswap/
    swapfault/pgtleak 族, 源码+二进制）。

- **[2026-09-26 03-04 时 换入回归修复班: W-6 硬阻断项提前闭环 (W-3fix2)]**
  上条登记的换入回归当场triage 到底并修复——kprobe 链（swpin/swpinr/get_swap_
  device/swap_read_folio 返回值）拆出**三层叠加缺陷**, 全在 corten_arena_swap_in:
  ① M6.T2 时代的入口守卫要求锚 VMA, W-2 后 VMA-less 窗口是常态 → 顶部
  -EFAULT（函数体其余部分早已全有 novma 形态, 守卫纯陈旧）; ② 直读假设同步
  设备（zram）——真盘（文件 swap）的 swap_read_folio 在 bio 完成 IO 中解锁
  folio, uptodate 门对每页误判失败 → 补异步等待+锁回取; ③ W1.f2 的
  cache-stuck 形状同样抵达 fault 臂（驱动写回滞留 SWAP_HAS_CACHE,
  swapcache_prepare 每次烧满 5s 死线, kretprobe 实测 -EAGAIN 5s 节拍）→
  改道 W1.f2 的 unuse_cache_pull（映射缓存 folio, 零设备 IO）, 解缓存则回
  直读。**修后: S-3 swapoff 电池双分支首次全 PASS**（分支 A 16384 页读回
  校验和一致+swapins 0→16384+干净 swapoff; 分支 B 提前收敛= W1.f/f2 臂拉走了
  原本盲骑的条目——脚本 spin/retry/zap 腿按收敛形态门控, bench 脚本同步更新）。
  入库: **ea3ecc76911f** tag **corten-r07-w3fix2** + github 同步（2baa1b1c）。
  验证: 三套件 24/0/1+111/0/0+34/0/5 全绿; checkpatch 0E/0W/0C; guest 回归扫
  （metis checksum parity/pgtables 零残差/smoke/单页+8MB 批量换入往返,
  zram+文件 swap 双通道）全绿。证据 results/r07/w3fix2/。W-6 的 S-3 硬阻断
  项撤销; 换入路径恢复到 W1.e2 时代的完好状态以上（真盘通道此前从未绿过）。

- **[2026-09-26 07:4x r09 会话启动（Hermes 自 r08 AskUserQuestion 僵死后重拉）:
  W-3fix2 审计完成 → FAIL(2 发现) → W-3fix3 修复片开工]**
  - 锁 owner=claude-code-glm53flash（Hermes 代持, 心跳 07:47→本会话 cron 自动维护）。
  - **审计（review 级, ea3ecc76911f 逐行）通过项**: ①守卫删除论证成立——函数体全部
    vma 使用点核查（folio alloc/perm_pgprot/rmap_novma/mmu_cache/corten_pte_mkwrite）
    全有 NULL 容忍臂（corten_arena.c:873 mkwrite_novma 回退实证）; ②cache-stuck 臂
    引用收支正确（swap_cache_get_folio 引用全出口配对; folio 锁内 swapcache 成员+
    swap.val 判定稳定——缓存摘除本持 folio 锁; 锁序 folio_lock→desc_lock 与上游
    ttu 同向; prepare 失败侧自身无标记可孤儿）; ③异步等待机理正确（async 设备
    swap_read_folio 于 bio end-io 解锁, wait+relock 恢复 tail 的 folio_unlock 配对;
    sync 门=SWP_SYNCHRONOUS_IO 与上游 sync 分支同形）。KUnit 24/0/1+111/0/0+34/0/5
    与登记一致（kmain2.log 复核, 尾部 VFS panic=无盘标准收场）; S-3 电池双分支
    PASS（s3-final.log）。checkpatch 独立复跑 0E/0W/0C（115 行; 缺 S-o-b 与
    W-3fix=d7bd0dd 惯例一致, 非本片偏离）。
  - **D30 (审计判定 FAIL→修复片)**: ①F1=新加的异步等待 folio_lock_killable -EINTR
    出口（arena.c:8376）实际**不可达**——该 folio 未发布（直读路径永不进 swap
    cache/PTE 仍是 swap entry/永不 LRU）, folio_wait_locked 返回后无人能持锁,
    trylock 必成功=死防御代码; 但若未来可达即裸 return 孤儿化自家 SWAP_HAS_CACHE
    （need_clear_cache=true 窗口内）→ 永久卡死 entry。修法=plain folio_lock（上游
    do_swap_page cache 分支同形, 不可失败即无出口）。②F2=arena.c:8404
    corten_lock_range 失败 `goto out_put` 绕过 swapcache_clear——**M6.T2 既有潜伏
    缺口, 同窗口**; 每次触发永久卡死 entry（后续 fault 每次 5s 死线+swapoff 永不
    收敛）。修法=goto out_clear（一行）。③证据缺口（登记）: commit message 声称的
    guest 回归扫（swapbulk 往返/metis parity/smoke/pgtables）无落盘日志（stdout
    只进会话）, 仅 kmain2.log+s3-final.log 在档; 验证 bzImage 未归档 bzimg/（vm2
    跑 /tmp/bzImage-main-swapfix3）。**处置**: W-3fix3 修复片（两处修复+全套 guest
    证据重生成落盘 results/r07/w3fix3/）, worktree w3fix3 @ 708c329, dev agent
    在制; 审计的 S-3 硬阻断撤销判定**维持**（电池双分支 PASS 为真）, F2 属独立性
    质缺陷不入 W-6 阻断面。
  - **运维**: vm（02:15, mva 陈旧 bzImage）/vm2（03:12, /tmp swapfix3 件）双退役
    （vm2 件先 quarantine 至 bzimg/quarantine-bzImage-main-swapfix3）; mva 分支
    mixed-reset 至主树 HEAD——**发现 mva worktree 存有 W-4 在制增量 +1399 行**
    （declare adopt 臂/frozen 发布协议/sweep 八计数器/528 行测试; 基线已含 W-3fix2,
    swap_in 区与主树零差异）, 与 next/w4-dev-brief.md 安排一致=合法 dev 草稿,
    r09-B 阶段续作吸收（提交时只 stage 三内核文件, worktree project/ 为 D20-b 前
    陈旧检出噪声勿动）。
  - **D31 (2026-09-26 08:0x 用户直接指令·GitHub 同步协议)**: zcode 停机漏推
    708c329, 已补推（github 远端 corten-github=708c3294698d）。**此后每片入库
    commit 即随手 `git push github android17-6.18:corten-github`, 不攒批**。形态
    更正: 远端分支 W-2 后为 5 个同内容重放件（树与主线逐字节一致, merge-base=
    680857 实证）, 本次以 force-with-lease 一次性换回主线正身（零内容损失, 旧件
    留 reflog）, 此后推送均为普通快进; D27 孤儿分支形态事实废止（AOSP 真历史已在
    远端）; aosp 远端不碰。

- **[2026-09-26 08:5x r09 maintainer: W-3fix3 ✓ —— swap-in 出口卫生 + 证据缺口补齐]**
  - 提交: worktree w3fix3 commit **92edc8f27f94** → 主树 cherry-pick **bf2ed6d05794**
    （HEAD, tag **corten-r07-w3fix3**; 1 文件 +20/−9; pick 后 mm/corten_arena.c 与
    worktree 验证构建 cmp **字节全等**）。github corten-github 随手同步（D31）。
  - 内容: ①8376 异步重锁 wait+killable → wait+plain folio_lock（-EINTR 死出口消灭,
    unpublished-folio 论证入注释, do_swap_page cache 分支同形）②8404 lock_range
    失败 out_put→out_clear（M6.T2 既有 SWAP_HAS_CACHE 孤儿窗口闭合, D30-F2）③
    失引用 out_put 死标签折叠。无新 KUnit 锚（两形状均故障注入形, commit 正文如实
    披露, 既有 roundtrip 锚+guest 电池承担回归）。
  - 验证: 三套件 on×2（24/0/1+111/0/0+34/0/5, 与 W-3fix2 基线逐格一致）+off
    （25/0/0+24/0/87+7/0/32）; checkpatch 0E/0W/0C（50 行）; =n 13 对象 RC=0 零
    符号（主会话补跑, agent 漏项）; **guest 全套重生成且全落盘**（W-3fix2 缺口
    闭合, 22 日志 results/r07/w3fix3/）: off 冒烟干净（uname g708c329-dirty=修复
    件, sha256 ff20f366…5374d）→ smoke 26/26 → gate PASS=22 FAIL=1（唯一 FAIL=
    登记 carrier 容差）+ J1 严格门 gate_pass==1/j1_hits==0/j2 27walks 零违例 →
    S-3 电池双分支 PASS（A: 16384 页 readback+干净 swapoff; B: 提前收敛形态）→
    swapfault/swapbulk zram+file 四通道 PASS → metis_eq ×2 checksum 自一致
    （65073 词, 与 W-3fix 基准同值）→ JTB 2000×3×3 → pgtables==0 + dmesg 零
    WARN/BUG。台账健康: swapins 32772/retries 0/heals 0, gup_probe_rejects 0。
  - bzimg/r07-w3fix3/（bzImage-w3fix3-final + SHA256SUMS）+ green.txt 登记行。
  - 边界: worktree w3fix3 内容全并入后移除; agent 留下的空 tmux vm 会话清理;
    mva（W-4 在制 +1399 行）未触碰。下一步 = W-4 吸收（task #4）。

- **[2026-09-26 20:2x~09-27 01:3x r09 W-4 吸收与收口: ★ MV2 W-4 ✓ —— 入场扫入落地（第六片, 含 B1-B5 修复全史）]**
  - **吸收与重建**: mva worktree 在制 W-4 草稿（+1399）发现基座陈旧（W-3fix 时代文件,
    缺 W-3fix2 全部三修复——审计教训: 只读 diff 前 150 行即断言"swap_in 零差异"是错误,
    全量核验后用「W-3fix 基座提取纯 W-4 patch → git apply 到主树正身」重建, swap_in 与
    主树字节全等）。dev→review 轮 1-3 全程: 草稿骨架对但未收口（宿主首跑 26/116 红, 6 类
    修复）→ review r1 FAIL(B1 原子上下文 GFP_KERNEL/B2 VM_ACCOUNT committed 泄漏/B3
    DONTCOPY/WIPEONFORK 收编反转 fork 语义)+A-E 偏差五项采信（enter_sweep 拆分/无
    anon_vma_put/非排他 fail-open/LOCKED skip/第 4 文件）→ 修复+DAS 首开暴露 B4（W-2
    时代 fork_copy_ptes 在 RCU 段内分配子 PT+pinned CoW folio——每 MODE fork 必睡, m6t34
    "DAS 首开翻旧账"模式重现）→ review r2 PASS+N1(pinned 臂 folio_get scratch 引用漏
    put)/N2(classify 缺 uffd ctx 门) 一行修 → guest 门首跑 FAIL=10+sweep-live fork 腿
    segfault → B5 双根因: ①fault.c 门只接 user_mode——glibc rseq lazy-clear（fork 后首
    个内核态写, COW 保缺页）绕门 find_vma 落空 → 双进程静默 SIGSEGV(SI_KERNEL); 修复=门
    摘 user_mode+内核臂 bad_area_nosemaphore(extable EFAULT) ②V-D exit walk 混编帧整帧
    跳过 → resident slot 泄漏（metis "Bad page cache" ×2+rss BUG ×8 家族）; 修复=
    owned-mixed 臂事务 zap arena 自身域、PT 页留 free_pgtables。
  - **D32 (2026-09-27 主会话裁决·EXIT 语义)**: swept mm 的树为空, EXIT-while-alive 无
    VMA 可回（EXIT 会拆掉进程自身 TLS/heap——guest smoke 死于 ip 0x44aece 的 TLS 拆除
    后 canary 读）→ **EXIT 拒绝**: 任一 live arena 带 CORTEN_RF_ADOPTED（rflags bit5,
    随 release/park/fork 重注册生灭="still swept"而非"ever swept", punch 掉收编段门重
    开）→ -EBUSY+计数器 exit_swept_refuses+debugfs 行; MODE 对 swept 进程粘性至死亡
    （exit_mmap 全量收尾）; fork 子侧继承位=正确（a fortiori, review r3 独立复核同意）;
    ARENA 级 RELEASE 无需守卫（逃生门语义所依）。de-sweep 逆迁移= MV3 级工作。用户可见
    prctl 契约变更, commit/REPORT 披露。
  - **smoke 契约件 v2**: 源码找到（share/t0dod/mode-smoke/, maxdepth 教训二度）, 按新
    契约改写 step-7（双形态: 自进场 EXIT=0+GET=0 / swept -EBUSY+GET=1+stock 存活见证）,
    四副本同步 sha256=37df16d7…, guest 26/26 双形态全绿。case 数保持 26（gate 硬编码兼
    容）。mva1_probe 全绿（簇 A 级联全消）。
  - **入库**: worktree mv-a0 commit **65ecfc3e1702** → 主树 --no-ff merge **64593a2b621d**
    （HEAD, tag **corten-r07-w4**; 5 文件 +2339/−126; 五文件 cmp 字节全等）。github
    corten-github 随手同步（D31）。
  - **验证终读数**: =y #282 构建零新增警告; KUnit on×2（24/0/1+**121/0/0**+34/0/5）+
    off（25/0/0+26/0/95+7/0/32, skip 对账精确）+ 十锚电池（fork_mirror 走真 fork_commit
    ——手工建模 dup_mmap 的夹具正是 fork 缺陷漏网原因）; checkpatch --strict 0E/0W/0C
    （2593 行）; =n 13 对象零符号; **guest 门 PASS=22 FAIL=1**（唯一 FAIL=登记 carrier
    容差）+ J1 严格门 gate_pass==1/j1_hits==0 + metis_eq ×2 checksum 同基准 + sweep-live
    PASS + Bad page cache/rss BUG 家族 2→0/8→0 + dmesg corten-quiet + pgtables 残值 1 笔
    （混编帧 PT 面=登记 W-5 靶面）。bzimg/r07-w4（#282 sha256=16928ba0…）+ green.txt。
    证据 results/r07/w4/（fix5/fix6/fix7 + guest-fix5/6/7 全档）。
  - **遗留登记**: ①混编帧 PT 页 pgtables_bytes 残值（1 笔/电池）→ W-5 靶面（与 W-3fix
    的 exit 上层残差同族）; ②两锚 mmput-action 卫生（fork_mirror/file_exit 裸 mm_alloc
    断言中止即漏 mm）→ W-5 顺手; ③j2_stale=3（smoke implant punch 的 kfree_rcu 退休窗
    瞬态, 静默期验证零新增, gate_pass 不受影响）→ 观察; ④KUnit 合成 mm PT 泄漏（C2 片
    =task #6, W-6 前）; ⑤make 树内串行化约定（本片三次并发 make 撞车）→ 流程改进。
  - **过程教训**: agent 两次 429 殉职（配额窗 5h）+主会话接管先例三度应用（r07-B.2/
    W-3fix3/本轮）; 多 agent 并发写同一 worktree 必须串行化; smoke 源码 maxdepth 漏找
    二度发生。
