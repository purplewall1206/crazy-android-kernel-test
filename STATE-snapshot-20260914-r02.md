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
  - 基础设施: **mm/corten.o arm64 零错误编译**（Round B, 唯一阻塞=test 文件 x86 宏, OQ5 回填乐观）; Round A defconfig Image 未完（幂等续传命令 inflight.txt）; syzkaller 构建完成+cfg 就绪（infra-report.md）。
  - 协议教训: 后台 agent 跨窗口等待会随进程消亡——重派制已实装。
- **r03 计划**: S4-S7 pud 修复→增量 review→commit; M3 DoD guest 冒烟（arena_stress/perf 符号/maps_check/ksmoke）; M3b.S8（debugfs arenas+ksmoke）; M9 Round A 续传; 余量则 M4.T0 设计评审。
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
