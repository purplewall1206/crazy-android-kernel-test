#!/bin/bash
# syzkaller-build.sh — M7 syzkaller 构建验证 (CortenMM r02 夜)
#
# 范围: 只构建 (make -j4) + 产物核验 + workdir 初始化。
#       不启动 syz-manager 本体 (运行是后续里程碑)。
# 前置: /home/ppw/tools/go/bin/go (1.23.4), /home/ppw/tools/syzkaller (已 clone)。
# 幂等: make 增量, 可重跑。
# 产物: /home/ppw/cortenmm/results/r02/syzkaller-build.log
#       cfg 初稿: /home/ppw/cortenmm/bin/syz-cfg.json (手写, 本脚本只核对其存在)
#
# 用法: bash /home/ppw/cortenmm/bin/syzkaller-build.sh [-n]

set -uo pipefail

DRYRUN=0
[ "${1:-}" = "-n" ] && DRYRUN=1

SYZ=/home/ppw/tools/syzkaller
GOHOME=/home/ppw/tools/go
RES=/home/ppw/cortenmm/results/r02
WORKDIR=/home/ppw/syzwork
JOBS=4
LOG="$RES/syzkaller-build.log"

mkdir -p "$RES"

say() { echo "[syz] $(date '+%F %T') $*"; }

run_log() { # run_log <logfile> <cmd...>  — rc 通过全局 RC 返回
    local lf="$1"; shift
    if [ "$DRYRUN" = 1 ]; then say "DRYRUN would run: $* (>> $lf)"; RC=0; return 0; fi
    "$@" > >(tee -a "$lf" >&2) 2> >(tee -a "$lf" >&2)
    RC=$?
    say "rc=$RC : $*"
    return $RC
}

preflight() {
    say "== preflight =="
    local fail=0
    [ -x "$GOHOME/bin/go" ] || { say "FATAL: go 缺失: $GOHOME/bin/go"; fail=1; }
    "$GOHOME/bin/go" version 2>/dev/null | sed 's/^/[syz] /'
    [ -d "$SYZ/.git" ] || { say "FATAL: syzkaller 源码缺失: $SYZ"; fail=1; }
    git -C "$SYZ" log --oneline -1 | sed 's/^/[syz] syzkaller @ /'
    # go 在 PATH 最前, 供 Makefile 使用
    export PATH="$GOHOME/bin:$PATH"
    export GOBIN=""
    [ "$fail" = 0 ] || exit 1
}

build() {
    say "== make -j$JOBS (首次构建预计 5-15 分钟, 增量重跑秒级) =="
    : > "$LOG"
    run_log "$LOG" make -C "$SYZ" -j"$JOBS"
    local rc=$RC
    echo "$rc" > "$RES/syzkaller-build.rc"
    return $rc
}

verify() {
    local rc=$1
    {
        echo "### syzkaller build ($(date '+%F %T'))"
        echo "源码: $SYZ @ $(git -C "$SYZ" rev-parse HEAD) ($(git -C "$SYZ" log --oneline -1))"
        echo "go:   $("$GOHOME/bin/go" version)"
        echo "make -j$JOBS rc=$rc"
        echo
        echo "### bin/ 产物"
        ls -la "$SYZ/bin/" 2>/dev/null || echo "(无 bin/)"
        echo
        echo "### 关键二进制核验"
        # syz-execprog/syz-executor 是目标特定二进制, 落在 bin/<GOOS>_<GOARCH>/ 子目录
        local tgt
        tgt=$("$GOHOME/bin/go" env GOOS GOARCH | paste -sd_ -)
        for b in syz-manager syz-prog2c syz-execprog syz-executor syz-mutate syz-db syz-repro syz-upgrade; do
            local p=""
            [ -x "$SYZ/bin/$b" ] && p="$SYZ/bin/$b"
            [ -z "$p" ] && [ -x "$SYZ/bin/$tgt/$b" ] && p="$SYZ/bin/$tgt/$b"
            if [ -n "$p" ]; then
                printf '%-14s OK  (%s) %s\n' "$b" "${p#"$SYZ/"}" "$(file -b "$p" | cut -c1-60)"
            else
                printf '%-14s MISSING\n' "$b"
            fi
        done
        echo
        echo "### cfg 初稿"
        ls -la /home/ppw/cortenmm/bin/syz-cfg.json 2>/dev/null || echo "(cfg 未写)"
        echo "### workdir"
        ls -ld "$WORKDIR" 2>/dev/null || echo "(workdir 未建)"
    } > "$RES/syzkaller-build.md.draft"
    say "核验结果写入 $RES/syzkaller-build.md.draft"
}

main() {
    preflight
    if [ "$DRYRUN" = 1 ]; then
        say "DRYRUN: would run make -C $SYZ -j$JOBS; mkdir -p $WORKDIR"
        verify 0
        return 0
    fi
    mkdir -p "$WORKDIR"
    local rc=0
    build || rc=$?
    verify "$rc"
    say "== 完成: make rc=$rc =="
    return 0   # 构建失败也不非零退出(链条继续), 判定在报告层
}

main "$@"
