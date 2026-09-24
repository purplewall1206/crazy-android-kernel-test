# M7 稳定性预热 · syzkaller 挂机（r07, x86_64）

日期: 2026-09-18 晚（D14 全天解禁窗口）
执行: M7 syzkaller 预热 subagent
状态: **管线打通, 挂机运行中**（最终启动 23:46:15, 计划运行至 08:00 由主 agent 收）

## 1. KCOV 内核构建判定: PASS

- worktree: `/home/ppw/linux-6.18-kcov`（分支 `kcov-build`, 基座 **025756094542** = tag
  corten-r06-m9p1, 含全部 corten M3b+M4.T0+D15+present-RO 修复）。
  注: 任务书写"基座=当前主树 HEAD"; 实际主树 HEAD 已被并行 M5 工作推进到 0719bc6ae74e
  （arena defer GUP 提交, 未验证）。按任务书命令字面锁定 025756094542, 避免踩并行新提交。
- 配置: 主树 .config 种子 + `KCOV=y, KCOV_ENABLE_COMPARISONS=y, KASAN=y(KASAN_INLINE,
  KASAN_STACK), DEBUG_INFO=y(DEBUG_INFO_DWARF5)`, CORTEN_MM/ARENA/KUNIT_TEST 保持 =y;
  第二轮追加 `USERFAULTFD=y`（machine check 发现缺, 见 §4）。
  注意 DEBUG_INFO 是派生符号, 须显式选 `DEBUG_INFO_DWARF5`（否则 DEBUG_INFO_NONE 残留）。
- 构建: `make -j6` 两轮均 exit 0。第一轮 33 分钟（21:41-22:14, bzImage 35.6MB）; 第二轮
  增量 ~28 分钟（23:03-23:3x, USERFAULTFD 触发全量重编）。
  唯一 warning: `objtool: cpuidle_enter_state+0x296: return with instrumentation enabled`
  —— KCOV_INSTRUMENT_ALL 常见无害项。零 error。
  日志: `/home/ppw/cortenmm/results/r07/kcov-build.log`（两轮追加）。
- 产物: `/home/ppw/linux-6.18-kcov/arch/x86/boot/bzImage`（`uname -r` =
  `6.18.32-g025756094542 #1 SMP PREEMPT_DYNAMIC`）。
- 宿主内存: 15G 总量下 KASAN 内核 + 4G VM + 并行 agent 的 2-3 个 VM 共存, 未 OOM。

## 2. syz-cfg 修正清单（初稿 → results/r07/syz-cfg-final.json）

初稿 `bin/syz-cfg.json` 的键全部符合 syzkaller rev 70a60e12 的 mgrconfig schema
（逐键对过 pkg/mgrconfig/config.go）, 修正的是值与缺失项:

| # | 修正 | 原因 |
|---|------|------|
| 1 | `kernel_obj` → `/home/ppw/linux-6.18-kcov`; `vm.kernel` → 同目录 bzImage | KCOV 内核在独立 worktree |
| 2 | `image` → `/home/ppw/vm/trixie-syz.img`（`cp --sparse=always` 副本, 已 `e2fsck -fy` 修复 live-copy 的 inode 计数） | 资源隔离, 不碰另一 agent 在用的 trixie.img |
| 3 | `vm.cmdline` 加 `root=/dev/vda rw net.ifnames=0` | amd64 arch 默认 `root=/dev/sda`(IDE); 改 virtio 需覆盖 root |
| 4 | `vm.image_device` → `-drive if=virtio,format=raw,file=` | 该 rev 的路径替换逻辑要求末元素以 `file=` 结尾; 首次写成 `file=,if=virtio...` 导致 qemu 拒绝（EOF 循环） |
| 5 | cmdline 加 `systemd.mask=sys-kernel-config.mount` | **关键修复**: 无此 mask 时 guest configfs mount 失败 → systemd emergency mode → sshd 永不起 → boot 超时循环（与本项目 launch_vm.sh 惯例一致） |
| 6 | cmdline 加 `kunit.enable=0` | KUNIT_DEFAULT_ENABLED=y + CORTEN KUnit =y 会在每次 VM boot 跑 KUnit; boot 期 console 的 WARNING 会被 syz-manager 判成 crash（实测 `WARNING in corten_txn_begin` 入 crash 库）→ 确定性假 crash 毁掉零基线。构建期 CORTEN_MM_KUNIT_TEST=y 不变 |
| 7 | `enable_syscalls` 去掉 `fork` | syz-manager FATAL `unknown enabled syscall: fork`——amd64 DB 无 fork 描述符（clone 覆盖） |
| 8 | `enable_syscalls` 加 `openat` | machine check: 无 fd 资源生产者 → mmap 全族 transitive disabled; openat 解锁 mmap/file-backed mmap（正对 D-G'' 的 file mmap MAP_FIXED 回归面） |
| 9 | 加 `tag: m7-preheat-r07-kcov` | 日志/崩溃可追溯 |

## 3. KCOV 内核单 VM 冒烟（tmux `syz-vm`, 端口 10023, pid 文件 `/home/ppw/vm/qemu-syz.pid`, 已退出）

- boot ~100s 到 SSH（KASAN 内核; guest 可见内存 3.3G/4 vCPU）。
- `/sys/kernel/debug/kcov` 存在; KASAN_INLINE 内建; corten KUnit 三套件（corten /
  corten_arena / corten_fault）boot 自跑。
- **KUnit 发现（kunit.enable=0 之前的观测, 如实记录）**:
  - 73 ok / 2 not-ok: `corten_test_txn_uninstall_interlock` 30.05s 超时失败（KASAN+KCOV
    +lockdep 三重开销下复现——即 STATE r07 已登记的 "interlock 用例 lockdep 开销下 1 次时序
    flake, M7 清单" 项, 本夜再+1）; 套件级 `not ok 1 corten` 由它连带。
  - 4 条 WARNING 均对上源码断言（KUnit 负向用例故意踩的路径, 所属测试全 ok）:
    `mm/corten.c:864 WARN_ON_ONCE(txn->nr_path >= CORTEN_TXN_PATH_MAX)`、
    `mm/corten.c:811 WARN_ON_ONCE(child->level <= cur->level)`（corten_txn_begin）、
    `include/linux/rwsem.h:88 rwsem_assert_held_write_nolockdep`（corten_arena_fork_demote）×2。
  - 判定: 非 KASAN 新 bug; interlock flake 属已知时序问题（建议 M7 清单里注明 KCOV+KASAN
    下加重）。真崩溃计数 = 0。

## 4. syz-manager 排障记录（全部收口）

1. `unknown enabled syscall: fork` → 删（§2#7）。
2. `--drive file=,...: A block device must be specified` → §2#4。
3. VM boot 10 分钟 SSH 超时 → tmux `syz-dbg` 复现抓到 emergency mode（§2#5）; 修后
   45s 到 SSH。
4. 坏配置期残留 EOF 假 crash（9 条）污染基线 → wipe workdir 重启。
5. machine check 剔 syscall → §2#8 + 内核补 USERFAULTFD=y 重建 + clone/ptrace 上游限制:
   - `userfaultfd: syscall not present`（内核 =n）→ 重建后 **present**。
   - `mmap: missing resource fd [creating syscalls]` → openat 解锁, 现 mmap 96 inputs。
   - `clone`/`clone3`: 上游 `(disabled)` 属性硬禁（防 executor 子进程自咬）, cfg 无法覆盖
     → fork/clone 家族与 `ptrace`（transitive: pid 资源无生产者）**本 rev amd64 不可开**。
     内核侧 fork 路径覆盖交给 M5 专项测试, 不属本管线。

## 5. 挂机状态（最终启动 23:46:15, PID 见下）

- 启动方式: `nohup syz-manager -vv 1 -config /home/ppw/cortenmm/bin/syz-cfg.json >
  results/r07/syz-manager.log 2>&1 &`（-vv 1 用于把 machine check 原因留档）。
- **manager 进程: PID 1784337**（nohup 脱离会话, 08:00 前持续运行）。
- 40 分钟核验（00:06）: exec total 23521 (19/s 稳定), corpus 507↑, coverage 14279↑,
  crash types 0, **crashes 0（crash 目录不存在）**, suppressed 0, pending/reproducing 0,
  VM 无 boot 循环。
- 启用 syscall 面: mmap/munmap/mprotect/madvise/mbind/userfaultfd/openat(含可达 openat$*
  变体)。资源隔离: 独立镜像 trixie-syz.img（-snapshot, 基盘只读不脏）、独立 workdir
  /home/ppw/syzwork、随机空闲 ssh/monitor 端口、http 127.0.0.1:56741。未触碰 tmux `vm`、
  `m5t1a-vm`、basecheck(10024) 与主树/其它 worktree。

### 状态查询命令（08:00 主 agent 收夜用）

```bash
# 进程与日志（manager PID 1784337）
pgrep -af '[s]yz-manager -config /home/ppw/cortenmm'      # manager PID
tail -20 /home/ppw/cortenmm/results/r07/syz-manager.log   # 最近 stats（candidates/corpus/coverage/exec/crashes）
# web 界面
curl -s http://127.0.0.1:56741/syscalls | ...             # 或浏览器开 http://127.0.0.1:56741
# 崩溃计数（期望 0; 若非 0: ls /home/ppw/syzwork/crashes/*/  + description 文件）
ls /home/ppw/syzwork/crashes/ 2>/dev/null
# corpus 规模
/home/ppw/tools/syzkaller/bin/syz-db stats /home/ppw/syzwork/corpus.db
```

### 停止命令

```bash
pkill -f '[s]yz-manager -config /home/ppw/cortenmm/bin/syz-cfg.json'
sleep 2; pgrep -af '[q]emu.*trixie-syz' && pkill -f '[q]emu.*trixie-syz'   # 防孤儿 VM
```

### CortenMM 遗留项复现观察位（本夜目标=基线, 命中即高价值）

- D-G''（file mmap MAP_FIXED 打洞）: openat+mmap file-backed 已在 fuzz 面上。
- OQ-D / present-RO 族: munmap/mprotect/madvise/mbind 混合已在线。
- 崩溃若指向 corten: 崩溃报告在 `/home/ppw/syzwork/crashes/<hash>/`（description +
  repro），如实上报勿硬凑。

## 6. 结论

- KCOV+KASAN+DWARF5+CORTEN 内核构建 PASS; syzkaller 管线全链路打通
  （cfg→qemu→SSH→machine check→fuzzing→corpus 增长）。
- 零崩溃基线建立（修复了三处会造成假阳性的配置坑: emergency mode / boot-KUnit WARN /
  坏 image_device EOF）。
- 已知限制: fork/clone/ptrace 因 syzkaller 上游 syscall 属性设计在本管线缺席（文档化,
  非缺陷）。
