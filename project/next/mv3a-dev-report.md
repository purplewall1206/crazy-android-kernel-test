# MV3.a 开发报告 · 默认进场（execve 即 MODE, D29 目标 2 头片）

授权: mv3a-dev-brief.md（用户指令"MV3 开工, 按 D29 路线走" 2026-10-04）。
基座: worktree /home/ppw/linux-6.18-mva @ 6e106786496a（W-7/mv2-complete）。
工件: results/r07/mv3a/。未 commit（主会话收口）。build 线 #320→#326。
工作目录 diff（=tree 现状, 未提交）: 4 文件 +266 行, 零删除:

- `fs/exec.c`（+5）: exec_mmap() 新 mm 安装点（task_unlock/lru_gen_use_mm 之后,
  两个 return 路径的公共单点, 早于 load_elf 全部 do_mmap）一行门调用
  `corten_exec_default_enter(mm)` + include。=n 下折叠为头文件空 inline。
- `mm/corten_arena.c`（+100）: ①`corten_mode_default=` boot 参数（__setup 记录
  plain bool; 无静态键, 不需 initcall 延迟——与 corten=on 语义完全分离, 默认
  off）; ②`corten_exec_default_enter()` 门函数: 参数+`corten_enabled_static()`
  双门下走既有 `corten_arena_mode_enter()`（A5 裸 enter: 只置位, 不 sweep,
  registry 惰性——复用既有原语, 幂等）+ `exec_default_enters` 计数; ③
  **in_execve 路由降级**（见"设计异议"②）; ④ **registry walk 补 it->last 判定**
  （见"设计异议"③）; ⑤debugfs arena_stats 行 `exec_default_enters`。
- `include/linux/corten_arena.h`（+18）: 门函数声明 + =n 空 stub（fs/exec.o 折叠）
  + KUnit 测试钩子 `corten_exec_default_test_set()`（on/off 锚共用一核）。
- `mm/corten_arena_test.c`（+143）: 两锚 `exec_default_off`（boot 无关, =off 也跑）
  / `exec_default_enter`（=on 直驱: 置位+state 恒 NULL+计数+1+re-exec 再 +1
  （计数语义=execve 次数）+mkvm auto 路由窗口落位+auto_attach 后 region 在表+
  in_execve 降级臂）。锚 129→131。

## 设计异议（brief 默认六条的修正, 均有红证）

1. **"suid 不特判"按默认执行**: 纯内核态接管无 ABI 面, setuid 位不清除。安全
   立场披露: suid 进程无差别进 MODE = suid 二进制同样吃 arena 域语义; CORTEN
   的威胁模型（根可配）下接受, MV3.d 全系统电池会再压一遍。
2. **"后续 ELF 段加载的 do_mmap 由既有路由承接（W-5+A.2a）"不成立, 降级臂补上**:
   ET_DYN 解释器首个 PT_LOAD 是 `addr==0` 非 MAP_FIXED（fs/binfmt_elf.c
   load_elf_interp: `load_addr = -vaddr`）, 恰是 A.2a auto 路由的窗口资格形;
   首版实现在 `corten_mode_default=on` 下 init 的 ld.so 直接 SIGSEGV
   （console 首跑: `segfault at 1000000373f6 ... in ld-linux ... panic: Attempted
   to kill init`）。修正: 路由头部加 `current->in_execve` 臂——exec 加载窗内
   addr==0 形不进窗口（exec 镜像保持 legacy-stock 布局）, 进程仍默认 MODE,
   do_execve 清位后运行期 mmap 照常路由。**exec 镜像自身的 arena 收编 = 登记
   MV3.c 时代后续件**（本片不吞——硬吞需要 place-and-adopt 新路由, 超红线）。
   代价披露: 默认 MODE 进程的 exec 镜像段仍在 tree（白名单外）——D28 字面
   tree-zero 在默认进场世界按此口径回退, MV3.c 收。
3. **W-7 R1 walk 的 "head-punched survivor" 臂对多帧幸存记录逐帧重发**（红证:
   第二版 =on boot, systemd-tmpfiles 退出 GPF `corten_arena_mm_exit+0xf3`,
   obs.prev=LIST_POISON2——同记录第二次 obs_remove 打在已 list_del_rcu 的节点上;
   旧世界不触发因 prctl 进场走 entry-sweep/release, 退出时 registry 已清; 默认
   进场首次让全系统进程带着活 registry 走 mm_exit）。修正: 候选 == it->last
   （本 walk 已发记录）跳过（该字段原为死赋值）。上限注记: 两条 head-punched
   记录跨幸存帧交错需 emitted-set, 无生产者形态, 不建。

## 验证链（=y #326 终树）

- **=y 零新增警告**: 仅 cpuidle objtool + memblock EXPORT 两条, 与 W-6/W-7
  build log 同值（无关文件）。
- **KUnit on×2/off×1**（终树 #326 全跑）: on 24/0/1 + **131/0/0** + 34/0/5;
  off 25/0/0 + **27/0/104** + 7/0/32——skip 对账精确: +1 pass = exec_default_off
  （boot 无关）, +1 skip = exec_default_enter。on1 首跑遇
  corten_test_txn_uninstall_interlock 红（坑清单在案的 interlock flake）,
  复跑绿（协议内）。控制台 WARN/BUG 族与 W-7 kunit-on1.log 逐项同值。
- **=n 折叠**: corten*.o 产出 0（陈旧产物移档 n-stale-objects/）; 13 消费对象
  （memory/mmap/migrate/rmap/swapfile/gup/oom_kill/x86-fault/kernel/sys/
  futex×5/mempolicy/mremap/madvise/mprotect）+ **fs/exec.o** nm 零 corten 符号;
  mm+fs+fs/proc+kernel+arch/x86/mm 全对象零符号（n-verify-mv3a.log）。时序披露:
  =n 窗口在 in_execve 臂与 walk 修正之前——两处后续改动全在 mm/corten_arena.c
  （=n 不构建）, 消费对象折叠结论不受影响。
- **checkpatch**: 0E/0W/1C（C = `corten_mode_default=` 未入
  kernel-parameters.txt; 既有 `corten=` 同款在案姿态, Documentation 不在红线
  文件单内）。
- **bzImage**: r07/mv3a/bzImage-mv3a-y = #326（sha256 前缀 57c86f9378e5）。
  构建时间戳造成的字节差曾出现（#322 vs #324, 30 字节）, 终链证据全部收敛在
  #326。

## guest 门 · 双 boot 结果

**=on boot（corten=on corten_mode_default=on）**: 全 systemd 启动成功, gate
读数（gate-nojournald.log）:

- 参数正证据: `corten: execve default entry enabled`（dmesg 双 pr_info）。
- **裸探针（无 hook LD_PRELOAD, sshd exec 链）: mode=1**（PROBE_RC=0——探针自身
  从未 prctl ENTER, 默认进场所致）。
- 三进程族: bash 自身 maps 3 条 arena 行 + 探针 mode=1; systemd(1)/udevd/sshd
  各 8 条 arena 行（ps 全程无 hook）。
- arenas 表对拍: sshd 首窗口地址 100000000000 → 表行
  `[100000000000,100000200000) anon active` ✓。
- 计数: exec_default_enters 153（boot 后期再涨, 背景进程族全计）; arenas 106。
- **smoke v2 裸形（无 LD_PRELOAD, 靠默认进场）: 26/26, SMOKE-DRIVER PASS**。
- smoke v2 hook 形: 二进制 **26/26**（幂等如设计）; SMOKE-DRIVER rc=1——驱动器
  的"arenas 账面回基线"断言失效（全系统 MODE 下背景 churn 使账面天然漂移;
  W-7 时代安静 boot 假设不再成立——披露, 非语义红）。
- **metis_eq 裸跑 ×2: checksum 2d383eeed4ceb73b = 基准精确一致**（r06→W-7 同值）。
- dmesg: corten WARN/BUG/timed-out 计 0; 非 corten 域一条 mm.h:2648 WARN（下述
  缺口②）。

**=off boot（corten=on, 无 corten_mode_default; regress-off.log）**: R1 裸探针
**mode=0**（分离证明: 同核同 corten=on, 仅参数缺席）; exec_default_enters **0**;
smoke hook 形 26/26 + SMOKE-DRIVER PASS（旧语义零扰动）; metis hook 形 checksum
同基准; dmesg 静默。KUnit off 套件 skip 对账精确（上节）。

## 如实报红（非阻断, MV3.b/d 登记面）

1. **journald 交互缺口（=on 全系统电池的唯一拦路面）**: 带 journald 的 =on boot
   在 systemd-tmpfiles-setup-dev-early 停摆（6min 无进展）, journald 自身
   SIGTERM 后用户态不退出（stop-watchdog 90s 超时, SIGKILL 后正常; 无 oops、
   无 drain-timeout——mm_exit 干净, 判用户态停机流程卡死）。mask journald 四件套
   后**全系统启动 17s 完成**, 本报告全部 =on 门读数来自该 boot。旁证: journald
   读客户端 cmdline 触发 `mm.h:2648 WARN_ON_ONCE(!vma)`（get_user_page_vma_remote:
   GUP 成功但 vma_lookup NULL——目标 argv 页在窗口 VMA-free 域; R15
   =0x100000e0000a 窗口内; 两次捕获）, cmdline 读判 -EINVAL。机制假说（未闭环）:
   journald 对 stdout 客户端的 /proc 元数据归因循环吃到 EINVAL/空读。
   MV3.b 修复方向: MODE 进程的 arg 页远程读语义（proc_pid_cmdline 族走
   corten gup 慢门应答）或 exec 栈页不收编。
2. **默认 MODE 进程 exec 镜像留在 tree**（设计异议②的代价; W-7 字面 tree-zero
   在默认进场口径回退, 白名单外条目=每进程 exec 段; sweep 进场世界不受影响）。
   MV3.c 批 mark/收编时代闭环。
3. **churn 下 arena_stats 读变慢**: =on boot 后段（arenas 158+、load 4.29）
   `cat arena_stats` >60s 未回（VM 存活, 其余 debugfs 正常）。登记 MV3.b; 疑点
   在 stats 报告器的 registry pin/页 walks 遇高并发 registry。
4. journald 挂 boot 的 console 与 init-SIGSEGV 首跑 console 被后续同名 boot 覆写
   （运维: 证据件应随跑即改名归档; 关键行已在本文与本会话记录引用）。既有
   console-mv3a-on.log = walk 修正后的挂 boot（tmpfiles 停摆 + watchdog 超时 +
   mm.h WARN 证据在档）。

## 坑清单遵守

pidfile 纪律（qemu-mv3akunit/qemu-mv3a/qemu-mv3adiag* 各自独立, 终态全清）;
-j12 串行 make; 轮次命名空间（tmux mv3a-vm/mv3a-diag*, 端口 10028/10029, 与
w7v2/a1 全隔离）; interlock flake 复跑绿; 9p 手挂; config 翻转即归档
（config-pre-n-mv3a.snapshot, nm/n-verify 落盘后才回 =y）。

**结论: MV3.a 头片达成——corten_mode_default=on 即 execve 默认 MODE（无 prctl
依赖, suid 同接管）, =off 双 off 路径零扰动实证; 三处红均根因修复或定界登记,
guest 双 boot 读数如上。建议: journald 缺口（红1）作为 MV3.b 首件, MV3.d 全系统
电池以 mask-journald 或修复后形态开跑。**
