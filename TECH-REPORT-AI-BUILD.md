# 一共使用了 552 小时的 AI，实现了移除内核 VMA 层的目标

CortenMM → Linux 6.18 移植最终技术报告。

项目周期：2026-09-12 晚启动，2026-10-05 完成主目标，墙钟 23 天，约 552 小时。执行形态：主控会话 1 个（r00-r08，09-12 至 09-23），Claude Code 接力会话 1 个（r09，09-26 至 10-05），每个开发切片再派 1 至 3 个 subagent 并行（dev / review / verify 分工）。主力会话单会话上下文用量在 866.8k token 量级，仅作单个会话规模的参考，全文计时一律用墙钟时长。文中所有数字均标注出处文件，原始日志在 results/ 下。

## 摘要

结果一句话：在一台 8 vCPU 的 QEMU 虚机上，AI 以多会话接力的方式把 SOSP'25 论文 CortenMM 的核心主张移植进 Linux 6.18，23 天后 MODE 进程的窗口地址域实现了零 VMA 运行，内核树走查实测零违例，零改动应用全部照常跑完，unmap 类热路径拿到十倍以上的提升。

关键数字：

- 墙钟约 552 小时；内核项目提交 107 个、tag 59 个（主树 git rev-list 10-05 实测；M-V 收官时点为 45 提交 / 43 tag，project/REPORT.md §9.1）。
- KUnit 测试锚 157 个（project/REPORT-FINAL.md；至 MV3 时代 on 臂单轮 pass 数已达 194，project/next/mv3cfeat-dev-report.md §3）。
- 性能：unmap-virt 低竞争 t4/t8 五轮固化 +1142% / +2583%（project/REPORT.md §4.2）；mmap 全族 +141% / +251%；fork 相对基线 -10.65%；mmap-pf 每页事务税从实测 -84.3% 收窄至 -63.2%，批 mark 首刀中位数改善 +134.6%（project/next/mv3cfeat-dev-report.md §2.3）。
- 正确性：syzkaller 两轮 453 万次执行零内存安全崩溃（corten=on 面 311 万，results/r08/m7-final.md）；窗口域树走查 J2 累计 69001 次、违例 0；J1 探针 617 次、非法命中 0（project/next/mv3e-dev-report.md §1.1）；MV3.d 全系统默认 MODE 电池连续 7216 秒零 panic 零 corruption。

## 1 背景与动机

CortenMM（SOSP'25）针对的是 Linux 内存管理热路径上一个存在了二十多年的软件层：VMA 树（6.18 里是 maple tree）加保护它的 mmap_lock。论文在自研原型系统上证明了这个层可以从事务化协议加 per-PTE 元数据替代，并拿到量级性能收益。原型是独立 OS，留下一个悬而未决的问题：这套思想能否 retrofit 进一个承载着全部生态兼容包袱的成熟内核。

本项目（STATE.md「使命」节）把问题收敛为三件事：第一，干掉 VMA 后内核照常运行；第二，mmap/unmap 等热路径取得可测提升；第三，fork 等全地址空间操作允许论文量级的回退。验证环境是 QEMU x86_64（8 vCPU / 4G / KVM），与论文 384 核只做方向性对照（D4 决策，project/STATE.md）。

执行者全部是 AI：一个主控会话负责裁决与收口，开发、复审、验证由 subagent 承担，人类只在少数架构级节点下达指令（D20「移除整个 VMA 层」、D28「说要完全移除就是要完全移除」、D29 终局目标均来自用户直接指令，project/STATE.md）。

## 2 这个疯狂的 idea 是什么

### 2.1 先讲给不看内核的人

把一个进程的内存想象成图书馆里的书。Linux 一直以来的做法：每批书进门，先在前台的卡片柜登记一张目录卡，写着这段内存从哪开始、多长、可读还是可写、来自哪个文件。之后每一次找书、退书、改权限，管理员都得先跑去翻卡片柜。卡片柜只有一份，还配一把大锁（mmap_lock），读者一多就在柜子前排长队。

CortenMM 的主张听起来有点极端：把卡片柜扔掉。每本书的书脊上直接贴上它的全部管理信息，书架本身既是藏书的地方也是唯一的账本。找书直接看书脊，前台和那把大锁从日常流程里消失，取而代之的是一套"借还必须走柜台事务"的规矩，保证几十个管理员同时动手时账目不打架。

这次移植要回答的问题是：Linux 这种被几十万程序依赖的系统，能不能动这种手术。答案分两半。窗口域（每个 MODE 进程划出的 48TB 地址段）真的做到了零 VMA：树走查 69001 次零违例，find_vma 探针 617 次零非法命中；全部进程的字面树归零没有做，堆、栈、vdso 这几类合同内住户按白名单留在树上，删除账清单已建好待裁剪（project/next/mv3e-dev-report.md §1）。

### 2.2 技术版：论文的三条主张

论文（project/docs/PAPER_SPEC.md 为唯一权威解读）给出三件事：

1. 事务化热路径。每张页表页配一个描述符（rwlock 加 per-PTE metadata array），fault/map/mark/unmap/COW 全部在 covering 写锁下走事务接口，metadata 是唯一真源，页表只是硬件缓存。
2. VMA-free 运行。VMA 树与 mmap_lock 从热路径上消失。
3. 性能收益。论文原型拿到量级提升。

### 2.3 我们的移植形态：单内核双 MM

retrofit 路线无法把 VMA 层连根拔：gup、rmap、khugepaged、/proc 全走它（D1 决策）。移植形态是 opt-in 域隔离：corten=on 的机器上，MODE 进程的窗口域 [16T, 64T) 里，匿名与文件映射的全生命周期走 corten 元数据与事务，从不产生 VMA；委托遗留域（堆、主栈、vdso、非零 hint 装载的库段）按白名单保留树住户，detached carrier 曾作为 rmap 锚保留。

口径在这里经历了一次重要的演进。D20-a（09-22）批准的诚实化口径接受白名单保留，理由是拒 carrier 等于三倍工程量；D28（09-24）用户以"说要完全移除就是要完全移除"推翻它，才有后来的 W 系列：W-1 原生 rmap、W-2 carrier 消灭（MODE 的 vm_area_struct 分配恒 0）、W-3 委托域迁移、W-4 入场扫入、W-5 植入消灭、W-7 multi-record registry。终态由 W-6 的实测读数钉住：动态多段 ELF 负载下 tree_entries=5，全部落在豁免桶（栈 1、special 3、brk 1）（project/REPORT.md §10，即 MV2 终表）。最终形态回到"窗口域零 VMA、白名单域合同保留"，但这次保留是逐桶实测对账的结果，账目在 project/next/mv3e-dev-report.md §1.1 的桶表里。

## 3 AI 是怎么一步步实现的

### 3.1 分阶段路线

M0-M3 奠基（09-12 至 09-16）。环境完备、基线测量、内核骨架（页表页描述符加事务 API，首片 3739 行，KUnit 16/16）、协议加固加 arena fault 绕 VMA。M3 的头条主张在这里首次实证：churn 4t 压测中 367K 个 perf 样本，find_vma 与 mmap_lock 六符号宽口径零命中（project/REPORT.md §3-M3）。

M4-M6 事务化加 fork/rmap/swap（09-16 至 09-21）。MODE 透明接管（T0a/T0b，已编译程序不改一行照常运行）→ per-cpu VA 杂志加 munmap 批处理 → T1c 常驻池（arena 生命周期税 17.4µs 降到 1.3µs，13 倍）→ perf1 TLB 风暴修复（ftrace 实测 MODE 侧 3 秒窗 82,942 次 tlb_flush 事件对基线 95 次，自持风暴；修后 unmap-virt t4 吞吐 0.012 到 0.791 ops/µs）→ 忠实 fork、COW、GUP 互操作 → rmap 守卫、swap 全事务、shrinker 压力通道（换出换入 69164=69164 笔笔对账）。这条线的 G5 判定首测 NOT MET：fork 回退 +516%，根因是每个 MODE 进程退出都付一个完整 RCU 宽限期，A5 惰性 registry 修复后 fork 转为 -10.65%（project/REPORT.md §4.4-4）。

M7 稳定性（09-21 至 09-22）。lockdep 全家族首检零 splat；DEBUG_ATOMIC_SLEEP 首开抓到 zap 路径 47 处原子内睡眠，修到 0；syzkaller 两轮挂机共 453 万次执行。

M-V 与 MV2 字面移除（09-22 至 10-04）。M-V 12 切片 36 小时（A.0 region record 到 V-E brk 裁决）完成窗口域零 VMA，tag corten-mv-complete；D28 推翻保留口径后，W-1..W-7 六片完成字面移除，tag corten-mv2-complete（project/REPORT.md §10）。

MV3 默认接管（10-04 至 10-05）。execve 即 MODE（exec_default_enters=153 实证，project/next/mv3a-dev-report.md）、journald 远程访问面双层根因修复、exec 镜像收编加 warm park 批 mark、全系统默认 MODE 电池 7216 秒，最后是删除账清单与 D29 闭环判定（project/next/mv3e-dev-report.md §3）。

### 3.2 多 agent 协作模式

主会话裁决，subagent 分工。每个切片的固定流程：主会话写任务书（dev-brief，附默认裁决条款）→ dev agent 在隔离 worktree 开发 → review agent 只读复审（给 PASS 或 FAIL 加逐条 findings）→ maintainer 收口入库，入库动作包括 checkpatch --strict 零警告、主树应用后与 worktree cmp 字节全等、打 tag、green.txt 台账登记。裁决走决策编号制，D1 到 D35，每条一段话加证据锚写进 project/STATE.md；单写者锁（run/lock）保证同一时刻只有一个会话动树。

dev agent 死在半路时主会话接管逐渐形成先例：V-B.2 的 dev agent 21:47 死于 1302 速率限制，留下 +420 行半成品，主会话审计确认完成度后补齐验证链入库；W-4 期间 agent 两次 429 殉职（配额窗 5 小时），同样的接管前后应用了三处（project/STATE.md 09-23 00:1x 条、09-26 20:2x 条）。

复审有真实否决权，且对 AI 自己也一样。M6.T3+T4 一轮复审给过 NO-GO 加三阻断项（自旋锁内睡眠、xarray 泄漏、偏差账缺失），逐条处置并复验后才入库（project/STATE.md 09-20 晚班条）；W-3fix2 自验全绿，r09 审计仍判 FAIL，从死防御代码和绕过 swapcache_clear 里抠出两处真缺口（project/STATE.md D30 条）。规格层同样如此：MV3.a 任务书的六条默认裁决被 dev agent 用红证推翻或修正了三条，其中一条直接来自 =on boot 首跑 init 的 SIGSEGV 现场（project/next/mv3a-dev-report.md「设计异议」节）。brief 是假设，guest 是裁判，这条次序在 23 天里没有例外。

### 3.3 gate 驱动开发

每片过全协议链，缺一不入库：=y 构建零新增警告；KUnit 三套件 on×2 加 off，skip 对账精确到个位；=n 编译折叠后 nm 验证零 corten 符号；checkpatch --strict；lockdep 变体（后来加 DEBUG_ATOMIC_SLEEP）；guest 电池（smoke 26/26、metis checksum 同基准、J1/J2 严格门）；bzImage 归档带 sha256。

gate 红了就修，修的过程全录。A.3b 的 gate 五轮迭代是典型：首跑 smoke 14 步死，进 VM 手拿 segfault 地址，发现终答漏了植入洞形状；三跑同败，根因是空窗段 MAP_FIXED 的 legacy VMA 根本没登记；KUnit occupied_incl_idle 又崩，corten_addr_in_window 是 current->mm 语义的入口助手，kthread 直驱语境下 NULL-mm 直接解引用（project/STATE.md 09-23 04:4x 条）。三轮修复全部随片入库，教训写成新的坑清单条目。

这套协议链的成本在数字上看得见：一个中型切片通常烧 5 到 7 个连续 boot 的验证矩阵，lockdep 变体和普通变体各跑一遍，=n 检查的对象清单从早期八对象扩到十五对象；入库环节坚持"主树应用后与 worktree cmp 字节全等"，把 worktree 验证与主线提交之间的等价性钉死在字节级。速度因此不快，MV2 修复轮 65+ 提交里相当一部分是判据面与锚的加固（project/REPORT.md §10），但 453 万次 fuzz 零内存安全的结果，就是这套慢流程换来的。

### 3.4 修复迭代实录（四个案例）

案例一：换出/换入边界的 futex 三连修（W-3fix / fix2 / fix3）。症状：W-3 终件上 metis_eq 以 rc=134 死于 "futex facility"，同 boot swapped_out==0；swapoff 电池腿自 W1.f 起持续 rc=1。定位：strace 抓到 futex WAIT_BITSET 返回 -EFAULT，落在 glibc 线程栈映射内；kprobe get_futex_key 返回 0xfffffff2；gup_probe_rejects 每次复现 +4。根因：corten_gup_window 的 check_vma_flags 仿真只读 ar->prot，而 CHUNK mprotect 只把 perm 提交进槽位（pending-perm），栈 region 的 FOLL_WRITE 全被拒。修复：探针对齐 FRESH 门规则（m.perm 优先，退 ar->prot），KUnit 锚红转绿，metis 双跑 checksum 与无 hook 跑同值。紧接的 W-3fix2 从换入路径拆出三层叠加缺陷：入口守卫还要求锚 VMA（W-2 之后 VMA-less 已是常态）、直读假设同步设备（真盘 async IO 在 bio 完成时才解锁 folio）、缓存滞留形状每次烧满 5 秒死线；修后 S-3 swapoff 双分支首次全 PASS。W-3fix3 是审计轮 FAIL 的产物：新加的 -EINTR 出口实际不可达（未发布 folio 无人能持锁），另一处 goto 绕过 swapcache_clear 会让 swap entry 永久卡死，后者是 M6.T2 时代就潜伏的缺口（project/STATE.md 09-26 三条）。顺带说明，W-3.2 的 GUP 重构里还落了 futex arena-anon key 臂：窗口页没有 VMA，get_futex_key 的取键路径需要一条 arena 臂才能服务 JVM 这类高频用户。

案例二：futex 路径的双重 put（MV3.b corruption 族）。症状：默认 MODE 的全系统 boot 下 6/6 复现 Bad page map（mapcount -1）、Bad page cache、PCP LIST_POISON2，进而 oops 加 RCU stall。开发期初判"MV3.a 时代 slot 竞态"，后来被 DEBUG_PAGEALLOC 加 PAGE_OWNER 的定罪链推翻：bad_page 的释放栈落在 __access_remote_vm 内联 folio_put 上。根因：首版 remote face 臂把 page 交给 corten_gup_window 时没带 FOLL_GET；真 MMU 的 GUP 对非空 pages 数组强制取引用，探针因此不取，而循环收尾的 folio_release_kmap 照 put，一次 put 对零次 get，引用计数压穿，页被提前释放、帧重用、双重释放（project/next/mv3b-dev-report.md §2）。修复是两处 face 臂各补一个标志位，×5 gate 5/5 零签名，MV3.d 长跑零复发。初判错误的推翻过程也留了痕，这类"证据改判"在账上出现过不止一次。

案例三：journald 连锁冻结（MV3.b）。症状：corten_mode_default=on 带 journal 的 boot 停在 systemd-tmpfiles-setup-dev-early 六分钟以上，journald 收到 SIGTERM 后用户态不退出，watchdog 280 秒击杀。MV3.a 的初判是"用户态停机流程卡死"。定位拆出两层：表面层是 get_user_page_vma_remote 的 post-GUP vma_lookup 在窗口域必 miss，页真实拿到了，只是没有树 VMA 可找；真根因是 __access_remote_vm 的 corten 短答臂返回时没有释放 mmap_read，读一次漏一把，目标 mm 的下一个 mmap 写操作冻结，PID1 的 cmdline 被读于是 PID1 冻结，journald 对同一 mm 的第二次读在 mmap_read_lock_killable 上自锁（project/next/mv3b-dev-report.md §1.1）。修复改 arm-local 直驱，错误判定先 unlock 再返回，读面加 FOLL_NOFAULT 硬化。修后 tmpfiles 9.08 秒（此前 6 分钟以上停摆），systemd 全启动约 16 秒。

案例四：/proc/maps 游标首行丢失（V-C）。J3 oracle 首测发现 maps 渲染错乱。根因是游标行推进先于渲染，每行渲染成了下一 region 的跨度：首行永不输出、末行重复、树头被吞、seq 回卷不变量破。单 region 探针自洽，所以旧测试全盲，多 region oracle 一上就现形（project/STATE.md 09-23 09:4x 条）。修复是 peek 头、promote 后再推进、树头归还、last_pos 不变量恢复，配新归并序 KUnit 锚；同片还抓到 PROCMAP_QUERY 的 ioctl 号错配（ENOTTY）。

### 3.5 运维事故：AI 长跑项目的真实面

限额中断。除 3.2 节三处接管外，09-22 夜验脚本随会话退出被带走（12:41 启动的 mva2-night.sh 已死），14:22 用 setsid 重启并脱离会话；08:10 的自动收数 cron 在触发时刻会话不在运行未触发，14:20 手动收数（project/STATE.md 09-22 14:2x 条）。

网络。GitHub SSH 22 端口被封，改走 HTTPS 加 PAT；TLS 瞬断让推送多次累积后补推；代理下 git 挂死，设 http.version=HTTP/1.1 解决；51 个 tag 悬在与远端不相交的 140 万提交历史链上，数 GB 补传从本机网络不可行，如实放弃并登记（project/STATE.md 10-04 补推收口条）。

pkill 自匹配。坑清单第一条就是"pkill qemu 用 [q]emu 防自匹配"，后来重演两次：一次模式串自匹配杀掉自会话，一次模式串里含端口号同样自匹配（project/STATE.md 09-22 与 09-23 条）。同一条坑以不同变体咬人三次，说明坑清单的价值在执行不在记录。

其余留档的还有：rebase 误传主分支名导致哈希链被重写（即时 reset --hard 恢复，教训是重放操作永远在临时 checkout 上做）；并发 make 撞车三次后立树内串行化约定；W-5 时 config 翻转致增量构建状态腐坏，验证件在被 =n 链接覆写前抢救归档；w6 与 w6b 两个会话的同名 gate 脚本碰撞，靠 md5 权威件仲裁（project/STATE.md 各条）。

## 4 效果和评估

### 4.1 性能账

全部同 boot BASE vs T0 双臂对照（ABAB 交错，8 vCPU QEMU，mmbench 动态口径）：

| 项目 | 读数 | 出处 |
|---|---|---|
| unmap-virt（PTE-less 窗口解除映射）低竞争 t4/t8 | 五轮固化 +1142% / +2583%，20/20 配对腿全正号；run5 终验 +1156% / +2327% | project/REPORT.md §4.2 |
| unmap（预映射区）t4/t8 | 固化 +45.4% / +142.0%；run5 +61.2% / +138.7% | 同上 |
| mmap 全族 | 固化 +141.3% / +251.4%（T1c 池消除生命周期税之后） | 同上 |
| fork / fork+exec / shell | 修复前 +516% / +172% / +354%；A5 修复后 -10.65% / +3.77% / +0.52%（门限 ≤30%） | project/REPORT.md §4.4-4 |
| dedup（tcmalloc 档） | 六个独立 boot 全正向，+8.7% ~ +19.2%，幅度不定 | project/REPORT.md §4.4-2 |
| mmap-pf（映射区缺页） | T5 时代地板 -17% ~ -22%；D35 刷新后本树起点实测 -84.3%；warm park 首刀后税 -63.2%，中位数改善 +134.6%（t4）/ +135.4%（t8） | project/next/mv3cfeat-dev-report.md §2.1-2.3 |

机制归因举两例。unmap-virt 的胜出来源被 G2 trace 钉死：池命中把 mmap_lock 写获取从每 op 2.02 次砍到 0.27 次（7.6 倍），基线侧 maple tree、kmem_cache、rcu_preempt 的 VMA 机器从每 op 路径上消失（project/REPORT.md §4.4-G2）。mmap-pf 的负差是论文 per-fault 事务语义的裸成本：每 fault 一事务（fill_upper、lock_range、xa_load、query、mark）加 take/park 簿记；终态 profile 两臂同形，corten 侧 self 仅 3.5 个百分点，差异进入 IPI/调度噪声地板（project/REPORT.md §4.6）。mmap-pf 的读数史有一段必须交代：t5final 时代的 -17%~-22% 在 D35 被判定过时，W-2/W-7/M-V 各轮累计把这条通道的成本推高，本树单进程起点实测 -84%，逐轮 bisect 未做、登记为独立小片，warm park 把税收到 -63.2%，距离个位数目标还有三刀（project/STATE.md D35 条）。

### 4.2 正确性账

- syzkaller 两轮共 453 万次执行（corten=on 面 311 万），零内存安全崩溃，6 个崩溃目录全部环境类逐条定性（results/r08/m7-final.md）。首轮曾把一条无关代码误归因为"正面旁证"，v1.2 版撤回并如实缩限覆盖面，更正记录在案。
- J1/J2 实测：J1 窗口域 find_vma 五原语探针累计 617 次、非法命中 0；J2 全树走查累计 69001 次、违例 0（project/next/mv3e-dev-report.md §1.1）。这两个累计读数是 live 转述、无归档原件，报告以归档同形读数（327/0、224/0）加结构论证为主判据，live 值作旁证，出处文件里写明了这一保留。
- KUnit 锚 157 个（MV3 时代 on 臂单轮 pass 194）；lockdep 家族全绿；DEBUG_ATOMIC_SLEEP 首开 47 处原子内睡眠修到零；KCSAN 短跑零数据竞争。
- 复审与独立验收层（J2 审计、J3 oracle、fork 差分锚）合计抓出并修复 25+ 个真缺陷，包括混合态 fork 静默丢内容、rss 记账族错位、双源游标首行丢失、共享 PUD 页搁浅（project/REPORT-FINAL.md §四）。
- MV3.d 全系统电池：P2 =on 世界（全部进程默认 MODE）连续 7216 秒，LTP 全编译安装运行、systemd 全栈、零 panic 零 corruption；dmesg 唯一签名是 75 笔 pgtables 噪声计数，属登记容差（project/next/mv3e-dev-report.md §3.2）。

### 4.3 代码量账

如实说，净增。M-V 收官时点：corten 侧生产 19,676 行，被替代的 VMA 层四件 13,360 行，净增约 6,300 行生产代码（project/REPORT-FINAL.md §三）。到 MV2 终账（W-6 J6）：VMA 层四件 13,408 行（vanilla 12,955），corten 生产 23,761 行加测试 20,573 行，生产侧净增 +10,806（project/next/w6-dev-report.md §6）。测试另有约 1.7 万到 2 万行。

"可删性已建立"与"已删"需要分开说。删除账清单已按五组 24 项建好，逐项带 file:line、死代码判据、风险评级，配裁剪 PR 序列 PR-0..4，每枚 PR 的验收门是 j1/j2/wl delta 恒零加全套电池（project/next/mv3e-dev-report.md §1.3）。判据本身是实测的：每一枚窗口域树走查的产物只可能是 NULL 或登记过的 implant。但清单明确"本账只登边界不登数"，实际删除等裁决后排期。死的是窗口域臂；heap、stack、special 三桶加 wl_file（非零 hint 装载的库段，约每进程 9 到 70 条）是合同内永久树住户，全树归零不在账的承诺内。

### 4.4 诚实边界

- J2/J4 在 W-6 中期判定曾 FAIL（special 桶分类器失明、sweep fail-open 面宽、J3 还有 W-2 起的 /proc/maps 渲染回归），按修正口径在 W-6b 与 W-7 修复后 PASS，FAIL 判定史如实保留（project/next/w6-dev-report.md §0）。
- 性能三刀未完：脏域有界的 park reset 走查、per-fault 簿记批化、地板演化 bisect，全部登记为独立小片（project/next/mv3cfeat-dev-report.md §2.4）。
- 51 个 tag 未补传 GitHub（网络条件所限），MV2 主链 8 个关键 tag 在线。
- G3 的字面门（真实应用单项稳定 ≥10%）没过，按原文登记 NOT MET；机制面成立，六个独立 boot 方向恒正。
- 其余登记项：suid 专项未单测；bpf_iter/task_vma 窗口段整段缺失，按显式降级口径闭合；库 hint 收编是超红线裁决件，留树不碍无损。

## 5 结论

### 5.1 判定

论文核心论断"VMA 层并非热路径必需"在这个移植里成立，且可复核：窗口域全生命周期零 VMA（J1/J2 实测封口）、内核照常运行、零改动应用全兼容（smoke 26/26、metis checksum 三方同值、JThreadBench 2000 线程 rc=0）、unmap 族数量级提升、每页事务税有测量与归因。D29 两目标的终态：目标①（事务化热路径加零 VMA 运行的全部机制主张）机制面全达成；目标②（对所有应用无损完整接管）达成于显式降级与响亮拒绝的口径。论文的性能量级未追平：mmap-pf 地板仍在 -63%~-77%，个位数目标未达，三项杠杆已设计在案（project/next/mv3e-dev-report.md §3.3）。

### 5.2 剩余工程

残留台账 16 开项（project/next/mv3e-dev-report.md §2）：P1 四项以 brk 路由 PT 生命周期 UAF 定罪为首；性能三刀；库 hint 收编语义裁决；裁剪 PR-0..4 排期；arm64 侧 contpte 决议实施（D22 已定方案）。D29 级开项为零，全部余项降级为独立小片。

### 5.3 对 AI 长期自主开发系统软件这个模式的观察

以下是我在 23 天里的第一手观察，供同行参考。

gate 比生成重要。23 天里修复的真缺陷，绝大多数拦在协议链上而非靠模型的一次性正确率：lockdep 变体抓出 47 处原子内睡眠，DEBUG_PAGEALLOC 定罪了漏 FOLL_GET 的引用压穿，J3 oracle 揪出被空真断言掩盖两代的渲染死码，review agent 在 W-3fix2 的"全绿"里仍判了 FAIL 并找出两处潜伏缺口。A.3b 的 gate 五轮、W-3 的三连修都在说同一件事：质量控制的主体是流程与判据，模型负责在其中填空，判据本身的覆盖面（比如 KUnit filter_glob 只跑主套件的 D25 覆盖盲区，09-21 登记过的坑被新脚本原样重新引入）才是真正容易漏的地方。

多会话接力可行，交接面必须落盘。r08 主控会话僵死后，r09 从 STATE.md 这份一千六百多行的现场账无缝接续，W-4 的在制增量都能从 worktree 里找回来吸收。上下文会死，账本不能死，"任何 agent 接手前必读"要写进文件头并执行。

诚实化口径会被用户推翻，而这推动工程走得更远。D20-a 的白名单保留在 09-24 被用户指令推翻后，才有 W 系列的 carrier 消灭与 tree_entries=5；反过来，MV3.e 又把"字面全删"精确化为"死的是窗口域臂"，逐桶对账。诚实边界和激进目标在反复对撞中把真实工程边界磨了出来。

运维面是长跑的隐形杀手。限额中断以"主会话接管"三度化解的前提，是每片的半成品都留在 worktree 里可审计；网络故障的代价被 green.txt 台账与 sha256 归档摊薄；pkill 自匹配三次变体说明运维教训需要 checklist 化执行，记录本身不产生免疫。

最后回到标题的账。552 小时墙钟里，AI 会话完成了一篇 SOSP 论文核心主张到 Linux 6.18 的移植、107 个提交、59 个 tag、157 个测试锚、453 万次 fuzz 与两代字面移除口径的迭代；代价与欠账（约六千到一万行的净增、-63% 的缺页税、51 个未补传的 tag、16 项残留台账）全部写在 project/ 下的对账文件里，可逐条复核。这份账本本身，或许就是这 552 小时最像样的一项产出。
