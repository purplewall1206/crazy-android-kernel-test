#!/bin/bash
# mva2-night.sh - M-V A.2 通宵矩阵一键跑 (timegate 后由后台任务调用)。
# 首红即停, 每阶段日志落 results/r07/mva2/, 状态写 mva2-night-state.txt。
set -u
BIN=/home/ppw/cortenmm/bin
LOGDIR=/home/ppw/cortenmm/results/r07/mva2
SHARE=/home/ppw/bench/share
TREE=/home/ppw/linux-6.18-mva
STATE="$LOGDIR/mva2-night-state.txt"
echo "night matrix start $(date '+%F %T')" > "$STATE"

step() { echo "[$(date '+%T')] == $1 ==" >> "$STATE"; }

# 0. gate (timegate blocks till 23:00; idempotent if already inside)
step "timegate"
bash "$BIN/timegate.sh" >> "$STATE" 2>&1 || { echo "timegate failed" >> "$STATE"; exit 1; }

# 1. =y 全量构建 + bzImage
step "bzimage"
bash "$BIN/mva2-verify.sh" bzimage "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL bzimage" >> "$STATE"; exit 1; }

# 2. KUnit on x2
step "kunit-on1"
bash "$BIN/mva2-verify.sh" kunit-y "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL kunit-on1" >> "$STATE"; exit 1; }
step "kunit-on2"
bash "$BIN/mva2-verify.sh" kunit-on2 "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL kunit-on2" >> "$STATE"; exit 1; }

# 3. KUnit off x1
step "kunit-off"
bash "$BIN/mva2-verify.sh" kunit-plain "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL kunit-off" >> "$STATE"; exit 1; }

# 4. =n 十四对象 (恢复 =y config)
step "n-objects"
bash "$BIN/mva2-verify.sh" n-objects "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL n-objects" >> "$STATE"; exit 1; }

# 5. DAS + lockdep 变体 (各自恢复 config)
step "kunit-das"
bash "$BIN/mva2-verify.sh" kunit-das "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL kunit-das" >> "$STATE"; exit 1; }
step "kunit-lk"
bash "$BIN/mva2-verify.sh" kunit-lk "$LOGDIR" >> "$STATE" 2>&1 || { echo "FAIL kunit-lk" >> "$STATE"; exit 1; }

# 6. guest 矩阵 (bzImage 用第 1 步的 =y 存档)
step "guest-boot"
KERNEL="$LOGDIR/bzImage-mva2-y" bash /home/ppw/bench/host/launch_vm.sh >> "$STATE" 2>&1 || { echo "FAIL launch" >> "$STATE"; exit 1; }
for i in $(seq 1 60); do
	/home/ppw/vm/gssh "echo up" >/dev/null 2>&1 && break
	sleep 5
done
/home/ppw/vm/gssh "echo up" >/dev/null 2>&1 || { echo "FAIL guest ssh" >> "$STATE"; exit 1; }
step "guest-gate"
/home/ppw/vm/gssh "bash /mnt/hostshare/mva2-guest.sh" > "$LOGDIR/guest-gate.log" 2>&1 \
	|| { echo "FAIL guest gate (见 guest-gate.log)" >> "$STATE"; exit 1; }
grep -E "^\[ok\]|^\[FAIL\]|DONE" "$LOGDIR/guest-gate.log" | tail -25 >> "$STATE"
step "guest-halt"
/home/ppw/vm/gssh "poweroff" >/dev/null 2>&1 || true
sleep 20; tmux kill-session -t vm 2>/dev/null || true

# 7. checkpatch 终版 + diff 备份
step "checkpatch"
cd "$TREE"
git diff > /home/ppw/cortenmm/patches/r07-mva2.diff
./scripts/checkpatch.pl --strict --no-signoff --ignore FILE_PATH_CHANGES \
	/home/ppw/cortenmm/patches/r07-mva2.diff > "$LOGDIR/checkpatch-mva2.txt" 2>&1
tail -4 "$LOGDIR/checkpatch-mva2.txt" >> "$STATE"
echo "night matrix DONE $(date '+%F %T')" >> "$STATE"
