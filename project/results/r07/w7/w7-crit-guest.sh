#!/bin/bash
# w7-crit-guest.sh -- W-7 终判据 guest 侧驱动 (pre/post delta 形):
# pre = 累积计数器直读; 拉起多段 ELF 负载 (MODE); post = 发布负载 pid 后
# 直读。判据: d_file + d_anon + d_unclassified == 0 (该负载白名单外归零),
# 且 post 的 tree_entries(_pid) 描述本负载。
set -u
DBG=/sys/kernel/debug/corten
tag=$1; bin=$2; shift 2
cd /tmp/w7

pick() { grep -E "^wl_(shadow|implant|stack|special|brk_vmas|file|anon|unclassified) |^tree_entries " "$1"; }

cat "$DBG/audit_gate" > "pre.$tag"
setsid "$bin" "$@" > "$tag.out" 2>&1 < /dev/null &
for i in $(seq 60); do
	pgrep -x "$(basename "$bin")" >/dev/null 2>&1 && break
	sleep 0.2
done
pid=$(pgrep -x "$(basename "$bin")" | head -1)
echo "$tag pid=$pid"
echo "$pid" > "$DBG/whitelist" 2>/dev/null
echo "wl_write_rc=$?"
cat "$DBG/audit_gate" > "post.$tag"
echo "--- post audit (tree_entries_pid 应为 $pid):"
grep -E "^tree_entries(_pid)? " "post.$tag"
pick "pre.$tag" > pre.txt
pick "post.$tag" > post.txt
echo "--- delta (post - pre, 逐桶, 按桶名对齐):"
awk 'NR == FNR { pre[$1] = $2; next }
	{ d = $2 - pre[$1];
	  printf "%-18s pre=%-6d post=%-6d delta=%d\n", $1, pre[$1], $2, d;
	  if ($1 == "wl_file" || $1 == "wl_anon" || $1 == "wl_unclassified")
		s += d }
	END { printf "W7_CRITERION_%s=%d (d_file+d_anon+d_unclassified)\n",
		toupper(t), s }' pre.txt post.txt
