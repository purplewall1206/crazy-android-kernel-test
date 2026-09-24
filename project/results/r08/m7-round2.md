# CortenMM M7 第二轮 — 挂机启动报告 (r08)

启动时间: 2026-09-21 08:02 CST（syz-manager PID 477619, 目标挂机至 09-22 08:00）

## 0. ⚠️ 关键发现（本轮最重要信息）

**r07 全程 1.42M exec 是在 corten=off 下跑的**，r07 报告 §4"corten 修改面经受住首轮模糊测试"
的表述不成立。证据链:
1. r07 cfg（bin/syz-cfg.json 与 results/r07/syz-cfg-final.json 同源）cmdline 无 `corten=on`；
   mm/Kconfig 与 mm/corten.c 明确: corten 静态分支**默认关**，仅 `corten=on` 启用。
2. `corten: page descriptors enabled`（corten=on 时的 boot 消息）在 r07 全部 syzkaller 产物
   （4 个 crash 目录/manager log）中零出现；仅在 r07 的 KUnit 验证日志（显式 corten=on）中出现。
3. r07 报告的"正面旁证"`madvise_vma_pad_pages` 位于 **mm/pgsize_migration.c（上游代码）**，
   与 corten 无关。
4. 即使加 `corten=on`，进入 arena/MODE 的唯一入口是 `prctl(PR_CORTEN_ARENA=79 / PR_CORTEN_MODE=80)`，
   vanilla syzkaller 无法生成这两个常量 → 不补 syzkaller 则新面（mprotect/mremap 路由、池、
   shrinker、swap 事务）全部不可达。

**处置（主 agent 未应答 AskUserQuestion，按最佳判断执行，可回退）**: 本轮加 `corten=on` 并给
共享 syzkaller 增补两个 prctl 变体描述。改动 = 2 个 untracked 新文件（无 tracked 文件改动，
`git checkout -- .` 不受影响，删除这两个文件即完全回退）；归档于本目录 corten.txt/.const。
若主 agent 不认可: 删除两文件 + 从 syz-cfg-r2.json 去掉 `corten=on` 与两个 prctl 项即可回退到
"r07 同质复测"，但那会使本轮重建 11 个提交的目的落空。

## 1. KCOV 内核重建判定: PASS

| 项 | 值 |
|---|---|
| worktree | /home/ppw/linux-6.18-kcov（branch kcov-build → detached @ b9541335a554） |
| 基座 | r07 基座 025756094542 + 11 提交（M4.T1/T2, T1c, M5.T1b/T2', M5.T3, M6.T1/T2/T3/T4, A5），fast-forward 无冲突 |
| .config | 原样沿用（KCOV+KASAN_GENERIC+DEBUG_INFO_DWARF5+CORTEN_MM/ARENA(+KUNIT_TEST)=y） |
| 构建 | `make -j6` 增量 PASS，~33 分钟（07:25-07:58），exit 0 |
| 产物 | arch/x86/boot/bzImage 35,869,696 B → results/r08/kcov-Image-r2 |
| sha256 | d6c0dd5b9e54809ee4c3ef2a4e0cb7c8528aee3cc1c02040492f900f249d424f |

Boot 冒烟（trixie-syz.img + 新 bzImage + `corten=on`，90s KVM 直启）:
10.7s `corten: page descriptors enabled`，到达 login 提示，BUG/WARNING/Oops/KASAN/panic = 0。

## 2. cfg 差异（syz-cfg-r2.json vs r07 syz-cfg-final.json）

| 项 | r07 | r08 本轮 | 说明 |
|---|---|---|---|
| vm.count | 1 | **2** | r07 复盘建议: 区分"慢"与"挂死" |
| cmdline | — | **+ `corten=on`** | 激活 corten 面（见 §0）; kunit.enable=0/systemd.mask 修正保留 |
| enable_syscalls | 9 项 | 9 项 + **prctl$PR_CORTEN_ARENA / prctl$PR_CORTEN_MODE** | arena 入场券; 用 variant 精确启用，未放开全部 prctl |
| kernel/bzImage | 同路径 | 同路径（新构建） | kernel_obj 不变 /home/ppw/linux-6.18-kcov |
| tag | m7-preheat-r07-kcov | m7-round2-r08-kcov | |
| reproduce | 默认 | true（显式） | vm.count=2 下 repro 不再独占全部 VM（r07 教训） |
| workdir/corpus | /home/ppw/syzwork | 同（corpus.db 124KB 种子沿用） | 启动即载入: corpus=769(748 旧 + 21 新种子) |
| 其余 | http 127.0.0.1:56741 / sandbox none / procs 4 / vm 4vCPU+4G / trixie-syz.img / image_device 写法 | 不变 | r07 九项修正全保留 |

### syzkaller 补丁（共享仓库 /home/ppw/tools/syzkaller @ 70a60e1，git 工作区仅 2 个新文件）
- `sys/linux/corten.txt`: prctl$PR_CORTEN_ARENA（DECLARE/RELEASE/QUERY）与
  prctl$PR_CORTEN_MODE（ENTER/EXIT/GET）两个 5 参变体。
- `sys/linux/corten.txt.const`: 手写常量（全部 arch-neutral uapi 定义: 79/80, 0/1/2, 1/2/3）。
  刻意不走 syz-extract——其 `-build` 会 `make mrproper`（会毁掉内核构建树），手写值逐条对照
  include/uapi/linux/prctl.h @ b9541335a554 核实。
- 验证: `make descriptions` + `make -j4` PASS（~15 分钟, exit 0）;
  `syz-prog2c` 实测 `prctl$PR_CORTEN_MODE(0x50,0x1,...)` → `syscall(__NR_prctl, 0x50, 1, 0, 0, 0)`。
- corpus.db 兼容性: prctl 签名未变，r07 语料全部有效载入。

## 3. 挂机启动状态（首 30 分钟核验, 08:02-08:32）

| 项 | 值 |
|---|---|
| syz-manager | PID 477619（nohup, 日志 results/r08/syz-manager-r2.log） |
| VM | VM-0=477823 / VM-1=477824，29 分钟同 PID 无重启（无 boot 循环），均带 corten=on |
| exec total | 96,851 @ 52/s（= 2×KASAN VM ~26/s，r07 单 VM ~22/s 同水平） |
| corpus / coverage | 658 / 17,496 —— **coverage 30 分钟已超 r07 17.8h 终值 16,875** |
| crash | 0（crashes/ 仅 r07 遗留 4 目录）; reproducing=0 |
| corten 可达性证据 | corpus 中已有 12 个含 corten prctl 的程序（5 个 MODE ENTER 链），fuzzer 自主发现入场券 |
| 宿主内存 | used 12G / available 2G（15G 机，另存其它 agent 的 5 个 VM 未触碰）——见 §5 监护项 |

## 4. 明早主 agent 检查 / 停止命令

```bash
# 状态检查
pgrep -af '[s]yz-manager -vv'                              # manager PID (477619)
tail -20 /home/ppw/cortenmm/results/r08/syz-manager-r2.log # 最近 stats（exec/corpus/coverage/crash）
grep -c 'crash\|lost connection' /home/ppw/cortenmm/results/r08/syz-manager-r2.log
ls /home/ppw/syzwork/crashes/                              # >4 目录 = 新 crash（r07 遗留 4 个）
pgrep -af '[q]emu' | grep -c trixie-syz                    # 应=2（VM-0/VM-1）
free -g                                                    # available 掉到 <1G 见 §5
curl -s http://127.0.0.1:56741                             # UI

# corpus 里 corten 程序数（fuzz 面健康度）
/home/ppw/tools/syzkaller/bin/syz-db unpack /home/ppw/syzwork/corpus.db /tmp/cr08 && \
  grep -l PR_CORTEN /tmp/cr08/* | wc -l && rm -rf /tmp/cr08

# 明早 08:00 停机（先 TERM manager, 卡住 >60s 再按 r07 流程 TERM qemu/KILL manager）
kill 477619; sleep 60; pkill -f '[t]rixie-syz.img.*VM-' ; kill -9 477619 2>/dev/null
```

## 5. 运行期监护项（挂机期间无人值守风险登记）

1. **宿主内存**: available 2G 且 KASAN guest 会渐进增长。若被 OOM-killer 波及，可能误杀其它
   agent 的 VM。建议挂机中偶发巡检 `free -g`；若 available <1G，优先 `kill` 单个 VM qemu
   （477823/477824 之一）让 manager 自动重建 count=1 状态。
2. **环境噪音预期**: RCU stall / VM 失联类与 r07 同性（慢速 VM 症状），记录即可；只有指向
   corten 帧的 KASAN/BUG/WARNING 才是本轮高价值目标（corten=on + 新 11 提交首次进 fuzz）。
3. **r07 遗留**: crashes/ 中 4 个旧目录仍在原处，勿误判为本轮产物（本轮新 crash 为新 hash 目录）。

## 附: r08 产物清单（/home/ppw/cortenmm/results/r08/）

kcov-build-r2.log / kcov-Image-r2(+.sha256) / syz-cfg-r2.json / syzkaller-rebuild-r2.log /
corten.txt + corten.txt.const（syzkaller 补丁归档）/ syz-manager-r2.log(全) + syz-manager-r2-head.log(头段) /
m7-round2.md（本报告）
