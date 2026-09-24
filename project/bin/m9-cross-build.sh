#!/bin/bash
# m9-cross-build.sh — M9 ARM64 交叉编译验证 (CortenMM r02 夜)
#
# 口径 (按任务指令 + publish/ARM64_PORTING.md §8 P1):
#   Round A 基线: 独立 worktree(m9-arm64 = 主树 HEAD, 只含 corten M2 三提交,
#                 不含 M3a 未提交改动) + arm64 defconfig + MGLRU/zram(lz4)
#                 交叉编译 -j6 过 = M9 P1 基线。
#                 同时把 -e CORTEN_MM 传给 Kconfig 并记录其被 depends on X86_64
#                 裁掉(预期行为)。
#   Round B 试验: 同 worktree 独立试验分支 m9-arm64-corten-trial (不回主、不 commit),
#                 临时把 CORTEN_MM depends 放开为 (X86_64 || ARM64),
#                 make -k 编 mm/corten.o + mm/corten_test.o, 收集失败点清单。
#                 失败是预期成果不是事故; 分类在 infra-report.md 人工完成。
#
# 纪律: 必须在 timegate 放行后运行; 先轮询等 M3 验证退出(最多到 01:00);
#       全程 -j6; 不碰主树工作区; 不启 VM。
# 幂等: make 增量; worktree/分支/配置幂等化; 重跑安全。
# 产物: /home/ppw/cortenmm/results/r02/m9-*
#
# 用法: bash /home/ppw/cortenmm/bin/m9-cross-build.sh [-n]
#   -n = DRYRUN (只打印计划动作, 不等待 M3、不构建、不写 worktree)

set -uo pipefail

DRYRUN=0
[ "${1:-}" = "-n" ] && DRYRUN=1

KDIR=/home/ppw/linux-6.18
WT=/home/ppw/linux-6.18-m9
BRANCH=m9-arm64
TRIAL_BRANCH=m9-arm64-corten-trial
CROSS=/home/ppw/tools/aarch64-toolchain/bin/aarch64-linux-
RES=/home/ppw/cortenmm/results/r02
JOBS=6
WT_COMMIT_FILE="$RES/m9-commit.txt"

mkdir -p "$RES"

say() { echo "[m9] $(date '+%F %T') $*"; }

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
    [ -x "${CROSS}gcc" ] || { say "FATAL: 交叉工具链缺失: ${CROSS}gcc"; fail=1; }
    "${CROSS}gcc" --version 2>/dev/null | head -1 | sed 's/^/[m9] toolchain: /'
    git -C "$KDIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { say "FATAL: $KDIR 不是 git 树"; fail=1; }
    for wt_existing in /home/ppw/linux-6.18-m3b /home/ppw/linux-6.18-m3b46; do
        [ -e "$wt_existing" ] && say "注意: 存在其他 worktree $wt_existing (不碰)"
    done
    [ "$fail" = 0 ] || exit 1
}

m3_wait() {
    [ "$DRYRUN" = 1 ] && { say "DRYRUN: 跳过 M3 等待"; return 0; }
    local h deadline
    h=$(date +%H)
    if [ "$h" -ge 23 ]; then
        deadline=$(date -d "tomorrow 01:00" +%s)
    elif [ "$h" -lt 9 ]; then
        deadline=$(date -d "today 01:00" +%s)
    else
        say "当前仍在计费时段(应在 timegate 之后运行); 跳过等待直接开始"
        return 0
    fi
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if pgrep -f "make -j1[26]" >/dev/null 2>&1 || pgrep -f "qemu-system-x86_64.*kunit" >/dev/null 2>&1; then
            say "M3 验证仍在运行(make -j12/-j16 或 qemu kunit), 睡 300s 再查 (上限 01:00)"
            sleep 300
        else
            say "M3 验证已退出, CPU 让给 M9"
            return 0
        fi
    done
    say "已到 01:00 上限, 不再等待, 直接开始(-j6 已是保守并行度)"
}

wt_ensure() {
    [ "$DRYRUN" = 1 ] && { say "DRYRUN: would ensure worktree $WT on branch $BRANCH"; return 0; }
    local pinned want
    if [ ! -f "$WT_COMMIT_FILE" ]; then
        want=$(git -C "$KDIR" rev-parse HEAD)
        echo "$want" > "$WT_COMMIT_FILE"
        say "锁定 worktree 基线提交 = $want (主树 HEAD, 口径记录于 m9-commit.txt)"
    fi
    pinned=$(cat "$WT_COMMIT_FILE")
    if [ -d "$WT"/.git ] || [ -f "$WT"/.git ]; then
        local cur
        cur=$(git -C "$WT" rev-parse HEAD 2>/dev/null || echo none)
        if [ "$cur" != "$pinned" ]; then
            say "worktree HEAD=$cur != 锁定提交 $pinned, reset --hard 对齐 (worktree 内无主树改动, 安全)"
            git -C "$WT" reset --hard "$pinned" >/dev/null
        fi
        # 清理上次试验轮可能残留的 mm/Kconfig sed (唯一会改的 tracked 文件)
        git -C "$WT" checkout -q -- mm/Kconfig 2>/dev/null || true
        git -C "$WT" checkout -q "$BRANCH" 2>/dev/null || git -C "$WT" checkout -q -B "$BRANCH" "$pinned"
    else
        if git -C "$KDIR" show-ref --verify --quiet "refs/heads/$BRANCH"; then
            git -C "$KDIR" worktree add "$WT" "$BRANCH"
        else
            git -C "$KDIR" worktree add "$WT" -b "$BRANCH" "$pinned"
        fi
        git -C "$WT" checkout -q "$BRANCH"
    fi
    git -C "$WT" checkout -q -B "$BRANCH" "$pinned" 2>/dev/null || true
    say "worktree 就绪: $WT @ $(git -C "$WT" rev-parse HEAD) (branch $BRANCH)"
    # 口径自证: 该提交应包含 corten M2 三提交
    git -C "$WT" log --oneline -4 | sed 's/^/[m9]   /'
}

round_baseline() {
    say "== Round A: arm64 defconfig + MGLRU/zram 基线交叉编译 (-j$JOBS) =="
    local CFGLOG="$RES/m9-baseline-config.log"
    local BLDLOG="$RES/m9-baseline-build.log"
    : > "$CFGLOG"; : > "$BLDLOG"

    if [ "$DRYRUN" = 1 ]; then
        say "DRYRUN: would run defconfig + scripts/config + olddefconfig + make -j$JOBS in $WT"
        return 0
    fi

    run_log "$CFGLOG" make -C "$WT" ARCH=arm64 CROSS_COMPILE="$CROSS" defconfig
    [ $RC -eq 0 ] || { say "FATAL: defconfig 失败"; return 1; }

    # 按任务指令的符号集; CORTEN_MM 会被 depends on X86_64 裁掉(预期, 留证)
    "$WT/scripts/config" --file "$WT/.config" \
        -e LRU_GEN -e ZRAM -e ZRAM_BACKEND_LZ4 -e ZRAM_DEF_COMP_LZ4 \
        -e CORTEN_MM -e CORTEN_MM_KUNIT_TEST >> "$CFGLOG" 2>&1
    run_log "$CFGLOG" make -C "$WT" ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig
    "$WT/scripts/config" --file "$WT/.config" --set-str ZRAM_DEF_COMP lz4 >> "$CFGLOG" 2>&1
    run_log "$CFGLOG" make -C "$WT" ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig

    # 证据: 关键符号落点
    {
        echo "### 关键符号检查 ($(date '+%F %T'))"
        grep -E "^CONFIG_(LRU_GEN|LRU_GEN_ENABLED|ZRAM|ZRAM_BACKEND_LZ4|ZRAM_DEF_COMP|ZRAM_DEF_COMP_LZ4)=" "$WT/.config" || true
        grep -E "^CONFIG_(PAGE_SHIFT|PGTABLE_LEVELS|ARM64_4K_PAGES|ARM64_16K_PAGES|ARM64_64K_PAGES|ARM64_VA_BITS)=" "$WT/.config" || true
        grep -q "^CONFIG_CORTEN_MM=" "$WT/.config" \
            && echo "!!! CORTEN_MM 竟然在 arm64 上生效了(与 depends on X86_64 预期矛盾, 需人工核查)" \
            || echo "CONFIG_CORTEN_MM= <absent>  (预期: Kconfig depends on X86_64 在 arm64 上裁掉)"
    } >> "$CFGLOG" 2>&1

    say "开始基线编译 (-j$JOBS, 预计 20-40 分钟)"
    local t0=$SECONDS
    run_log "$BLDLOG" make -C "$WT" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS"
    local rc=$RC
    local mins=$(( (SECONDS - t0) / 60 ))
    echo "$rc" > "$RES/m9-baseline.rc"
    if [ $rc -eq 0 ]; then
        say "基线编译 PASS (${mins} 分钟)"
        cp "$WT/.config" "$RES/m9-baseline-defconfig"
        {
            echo "Round A (基线): PASS"
            echo "提交口径: $(cat "$WT_COMMIT_FILE") (= 主树 HEAD, corten M2a/M2b/M2c-fix1 在内, M3a 未提交不在内)"
            echo "并行度: -j$JOBS  耗时: ${mins} 分钟"
            echo "工具链: $CROSS ($("${CROSS}gcc" --version | head -1))"
            echo "Image:  $WT/arch/arm64/boot/Image.gz"
            ls -la "$WT/arch/arm64/boot/Image"* 2>/dev/null
        } > "$RES/m9-baseline-summary.txt"
        if [ -f "$WT/arch/arm64/boot/Image.gz" ]; then
            cp "$WT/arch/arm64/boot/Image.gz" "$RES/m9-baseline-Image.gz"
            say "已归档 $RES/m9-baseline-Image.gz ($(du -h "$RES/m9-baseline-Image.gz" | cut -f1))"
        fi
        grep -E "warning:|error:" "$BLDLOG" | sort | uniq -c | sort -rn | head -30 > "$RES/m9-baseline-warnings.txt" || true
    else
        say "基线编译 FAIL rc=$rc (${mins} 分钟) — 错误清单见 $BLDLOG"
        grep -nE "error:|Error [0-9]" "$BLDLOG" | head -50 > "$RES/m9-baseline-errors.txt" || true
    fi
    return $rc
}

round_trial() {
    say "== Round B: CORTEN_MM depends 放开试验分支 (预期失败收集) =="
    local CFGLOG="$RES/m9-trial-config.log"
    local BLDLOG="$RES/m9-trial-build.log"
    : > "$CFGLOG"; : > "$BLDLOG"

    if [ "$DRYRUN" = 1 ]; then
        say "DRYRUN: would create branch $TRIAL_BRANCH, sed mm/Kconfig depends, build mm/corten.o mm/corten_test.o with make -k"
        return 0
    fi

    git -C "$WT" checkout -q -B "$TRIAL_BRANCH" "$(cat "$WT_COMMIT_FILE")"
    # 幂等: 先还原可能残留的上次 sed
    git -C "$WT" checkout -q -- mm/Kconfig 2>/dev/null || true

    local before after
    before=$(grep -n "depends on MMU && X86_64" "$WT/mm/Kconfig")
    after=$(echo "$before" | sed 's/depends on MMU && X86_64/depends on MMU \&\& (X86_64 || ARM64)/')
    {
        echo "### CORTEN_MM depends 临时放开 ($(date '+%F %T'))"
        echo "### before: $before"
        echo "### after : $after"
    } >> "$CFGLOG"
    sed -i 's|depends on MMU && X86_64|depends on MMU \&\& (X86_64 \|\| ARM64)|' "$WT/mm/Kconfig"
    grep -n "CORTEN_MM" -A2 "$WT/mm/Kconfig" | head -8 >> "$CFGLOG" 2>&1

    "$WT/scripts/config" --file "$WT/.config" \
        -e CORTEN_MM -e CORTEN_MM_KUNIT_TEST -e KUNIT >> "$CFGLOG" 2>&1
    run_log "$CFGLOG" make -C "$WT" ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig
    grep -E "^CONFIG_(CORTEN_MM|CORTEN_MM_KUNIT_TEST|KUNIT)=" "$WT/.config" >> "$CFGLOG" 2>&1 || \
        echo "!!! CORTEN_MM 未在 .config 生效" >> "$CFGLOG"
    cp "$WT/.config" "$RES/m9-trial-defconfig"

    say "试验编译: make -k mm/corten.o mm/corten_test.o (失败=预期成果)"
    local t0=$SECONDS
    run_log "$BLDLOG" make -C "$WT" -k ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS" mm/corten.o mm/corten_test.o
    local rc=$RC
    local mins=$(( (SECONDS - t0) / 60 ))
    echo "$rc" > "$RES/m9-trial.rc"
    say "试验编译 rc=$rc (${mins} 分钟) — rc!=0 即预期成果(失败点清单)"

    # 失败点抽取: 每个独立 error 行 + 所属文件
    grep -nE "([^ ]*\.(c|h|S):[0-9]+:[0-9]+: )?error:" "$BLDLOG" | grep -v "^.*multiple definition" \
        > "$RES/m9-trial-errors.txt" 2>/dev/null || true
    grep -E "error:" "$BLDLOG" | sed -E 's/:[0-9]+:[0-9]+:/:/' | sort | uniq -c | sort -rn \
        > "$RES/m9-trial-errors-uniq.txt" 2>/dev/null || true
    local n=0; [ -f "$RES/m9-trial-errors.txt" ] && n=$(wc -l < "$RES/m9-trial-errors.txt")
    say "试验错误行数: $n (分类归并在 infra-report.md 人工完成)"

    # 还原 mm/Kconfig (试验分支保持干净, sed 不入库不回主)
    git -C "$WT" checkout -q -- mm/Kconfig
    say "试验分支 $TRIAL_BRANCH 保留(未 commit, 未回主); worktree 保留供重跑"
}

main() {
    preflight
    say "窗口检查: 应已在 timegate 放行之后 (23:00-09:00)"
    m3_wait
    wt_ensure
    local bas_rc=0
    round_baseline || bas_rc=$?
    round_trial
    say "== 完成: 基线 rc=$bas_rc, 试验 rc=$(cat "$RES/m9-trial.rc" 2>/dev/null || echo NA) =="
    say "产物目录: $RES (m9-*)"
    return 0   # 试验轮 rc!=0 不是脚本失败; 报告层判定
}

main "$@"
