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

## 里程碑 (DoD 见 MASTER_PROMPT.md §4)
- [ ] M0 环境完备: fetch 完成 + android17-6.18 x86 bzImage 启动 trixie VM (MGLRU+zram 默认开) + guest 测试件安装
- [ ] M1 基线: 论文同款 5 微基准(1..8线程×低/高竞争) + lmbench fork/fork+exec/shell + JVM 线程创建
      + metis/dedup/psearchy 或等价 workload + mmap_lock tracepoint 基线 trace, 全部落盘 publish/baseline/
- [ ] M2 内核骨架: mm/corten (page descriptor + covering-PT-page 协议 rw 版 + 事务 API C 版) 编译通过+KUnit
- [ ] M3 fault 路径绕过 VMA (opt-in mm/arena, shadow-VMA 互操作), selftest+压力通过
- [ ] M4 事务化 mmap/munmap/mprotect + per-core VA 分配器 + TLB shootdown 优化
- [ ] M5 COW/fork + GUP 互操作
- [ ] M6 swap/zram + rmap/回收(MGLRU) 互操作 + 内存开销测量
- [ ] M7 稳定性: mm kselftests + syzkaller (KCOV 专用构建) + lockdep 构建无死锁
- [ ] M8 性能对照论文全矩阵 + perfetto 分析报告 REPORT.md
- [ ] M9 ARM64: 移植设计文档 (BBM/contpte/16K页) + bootlin 工具链交叉编译 defconfig 通过 (+qemu 可选)

## 当前阶段
- **M0 ✓ M1 ✓ M2 ✓**（2026-09-13 夜 r01，全 DoD 证据在 results/r01/ 与 publish/baseline/）:
  - M0: android17-6.18(HEAD 68974e235117) 构建 436s 零错误; bzimg/r01-m0-android17-baseline;
    启动 65s; zram lz4 2G prio100; lru_gen 0x7; git 首条空提交+tag corten-r01-m0。
  - M1: 基线全矩阵落 publish/baseline/（mmbench 5×2×{1,2,4,8,16}×3 中位 + lat_proc
    fork 1298µs/fork+exec 2969µs/shell 9874µs + JVM 2000 线程 2347ms + metis_eq 15.77s/
    psearchy_eq 4.01s/dedup_eq 4.80M blocks/s(glibc)/5.16M(tcmalloc) + pf_high8.pftrace+
    trace SQL 摘要）。**机制级发现**: 6.18 CONFIG_PER_VMA_LOCK 使 PF 几乎不碰 mmap_lock
    （30s trace: 50 个 mmap_lock 事件 vs 101 万缺页）→ M8 解读 mmap-PF 提升时不能全归因
    mmap_lock 消除，须按论文口径分开 mmap/unmap 系（VMA 树写锁）与 PF 系。
  - M2: 三提交全链路 dev→review→maintainer→qemu-exec:
    b8386e4e(m2a 骨架) → 2fd4070e745c(m2b 协议+事务 API) → 1284a235f751(修复 corten=on
    早期挂死: static key 翻转延迟到 early_initcall, +review 5 项); tag
    corten-r01-m2a/m2b/m2c-fix1; KUnit 16/16（宿主无盘 qemu + guest 双验证）;
    corten=off/on 双冒烟过; debugfs enabled=1 ptdescs=341。bzimg/r01-m2c-fix1。
  - 遗留 BUG→已修: corten=on 挂死（run-time text patching 未就绪时翻 static key）。
- **下一步 M3**（fault 路径绕 VMA, opt-in arena）开工前必做 review 遗留:
  ① uninstall 与写锁持有者互锁（"持写锁事务活过 PT 页回收"——M3 安全性核心）;
  ② hole→ensure-alloc 位于 corten_txn_begin child==NULL 分支（read→write 升级+alloc_if_none+降级, 照 verified 版结构）;
  ③ 升级窗口并发回归测试; ④ lockdep(PROVE_LOCKING) 构建下跑全套 KUnit;
  ⑤ ⑨ 清单其余（CORTEN_TXN_PATH_MAX 溢出分支/512 页满窗 mark/unmap/debugfs 冒烟/corten=on 跑 KUnit）。
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
