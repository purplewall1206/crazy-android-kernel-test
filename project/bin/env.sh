#!/bin/bash
# env.sh — CortenMM→Linux 移植项目环境事实（2026-09-12 由主 agent 逐一验证）。
# 所有 subagent 开工前 source 本文件（或至少通读）。
# source /home/ppw/cortenmm/bin/env.sh

# ---- 内核 ----
export KDIR=/home/ppw/linux-6.18            # android17-6.18 内核树 (git, 远端 aosp)
export SEED_CONFIG=/home/ppw/kernel/linux-6.18/.config   # 已在本 VM 验证可启动的 6.18 种子配置
export BZIMG=$KDIR/arch/x86/boot/bzImage
export KJOB=-j12                            # 宿主机 16C/15G, -j12 安全
# ---- 项目 ----
export PROJ=/home/ppw/cortenmm              # 状态/日志/补丁主控目录
export PAPER=/home/ppw/paper/corte/paper.txt  # CortenMM 论文全文 (pdftotext)
export ARTIFACT=https://github.com/TELOS-syslab/CortenMM-Artifact  # 论文 Rust 参考实现
# ---- 虚拟机 ----
export VM_IMG=/home/ppw/vm/trixie.img       # Debian trixie rootfs (syzkaller create-image.sh 制作)
export LAUNCH=/home/ppw/bench/host/launch_vm.sh
export GSSH=/home/ppw/vm/gssh               # ssh root@127.0.0.1:10022
# 换内核必须先 tmux kill-session -t vm（qemu 只在启动时读 -kernel）:
#   KERNEL=$BZIMG bash $LAUNCH $VM_IMG "systemd.mask=sys-kernel-config.mount"
# 串口控制台: tmux capture-pane -p -t vm | tail -40
# ---- 基准设施 (memcg-bandit 项目遗留, 直接复用) ----
export BENCH=/home/ppw/bench                 # guest 端 /root/bench/bin/{run_once,run_all,...}
export SHARE=/home/ppw/bench/share           # 9p 挂载进 guest (mount -t 9p -o trans=virtio hostshare /mnt)
# ---- 工具 ----
export TP=/home/ppw/tools/perfetto/linux-amd64/trace_processor_shell
export PERFETTO_CLI=/home/ppw/tools/perfetto/linux-amd64/perfetto
export TRACEBOX=/home/ppw/tools/perfetto/linux-amd64/tracebox
export SASHIKO=/home/ppw/tools/sashiko/third_party/prompts/kernel/subsystem  # 67 个子系统知识文件
export PFSKILLS=/home/ppw/tools/perfetto-skills   # Perfetto 分析 skills (Gracker)
# [2026-09-12 冒烟验证过全链路, 结论与标准用法见 $PROJ/log/20260912-r00-smoke.md]
#   - perfetto 二进制解压后必须 chmod +x（已修好）
#   - 原始 tracepoint 事件在 trace_processor 的 ftrace_event 表（不是 slice 表）
#   - guest 采集: gssh '(cd /root && ./tracebox -c /mnt/<cfg> --txt -o /mnt/<out>.pftrace &) && sleep 1 && <workload>'
#   - 采集 cfg/fchurn.c/SQL 模板: $PROJ/results/r00/
# ---- 已就绪/仍缺 ----
# [sudo 可用(密码用户 privately 提供,不落盘)] -> 可 apt 装包
# qemu-system-aarch64 6.2.0 已装(M9 可 TCG 真启动); 仍缺: Go(syzkaller 用, apt 版太老,
#   用 tarball: curl -L https://go.dev/dl/go1.23*.linux-amd64.tar.gz | tar Cx /home/ppw/tools)
# ---- 硬约束 ----
# 1) 所有实验窗口 23:00–09:00 CST, 必须经 /home/ppw/cortenmm/bin/timegate.sh 放行
# 2) 内核任何改动必须 CONFIG_CORTEN_MM 默认 n / boot 参数默认关, 基线 bzImage 永远可回退
# 3) 每夜结束: 更新 $PROJ/STATE.md, log/ 追加报告, git commit+push（2026-09-22 起 publish/ 已并入项目根 git 仓库）
export GH_REMOTE=https://github.com/purplewall1206/crazy-android-kernel-test.git
