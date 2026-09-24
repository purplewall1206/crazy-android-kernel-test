# JThreadBench ClassFormatError 终验收口 —— r07/jtbcfe-close

- 班次: 2026-09-20/21（终验收口, 时盒 60min）
- 对象缺陷: MODE 进程 `java HelloFmt` / JThreadBench 100% `ClassFormatError: Unknown constant tag 0`（r06/jtbcfe-final.md 判定 (b): arena window 多页 pread 短读, 页 1..8 零页）
- 本班任务: 复测优先 —— gupfix 0719bc6ae74e 已合入主树, jtbcfe 取证早于它, 先验证 CFE 是否已消失
- **判定: CFE 已被 gupfix 完全消除。缺口关闭, 无 (a)/(b) 分叉, 无需修码。M4.T0 零改动兼容最后一个已登记缺口闭合。**

---

## 0. 结论一句话

**在主树 HEAD（b9541335a554, 含 gupfix+M6.T1/T2/T3/T4+A5 全部）内核上, MODE 下 3/3 `java -Xmx512m HelloFmt` rc=0 `[hello] done` 零 CFE + fork-probe OK; gdbpy17 金标准审计命中 r06 §2 预定复验判据逐字达标: `OREAD[3] size=32963 ret=32963 diff_vs_disk=0 zeropages=[]`（修复前 3120/28891/8 零页）; class+load 日志铁证 `sun/text/resources/cldr/FormatData_en`（修复前 100% 受害类）自 jrt:/java.base 干净加载。内核树零改动, 无 commit/无 tag（(b) 分支未触发）。**

## 1. 环境（与交接的一处偏差, 已纠正）

- 交接称 tmux `vm` 跑 bzimg/r07-m6t34+: 实测端口 10022 的 `vm` 会话内核为 **6.18.32-g5c545359e856-dirty**（linux-6.18-m6t1 worktree 的滞后构建, 且 guest 无 /mnt/share、无 java 取证环境）——不满足终验条件, 弃用。
- 改用 **tmux `vm-t5run5`（端口 10026）**: 内核 **bzImage-6.18.32-gb9541335a554-93-t5run5 = 主树 HEAD b9541335a554**, cmdline `corten=on`, 9p hostshare=/home/ppw/bench/share（guest 挂载点 /mnt/hostshare）, guest 空闲（load 0.07）。`git merge-base --is-ancestor 0719bc6ae74e HEAD` = YES（gupfix 在链上）。
- 工件: hook=/mnt/hostshare/t5run2/corten_mode_hook.so（MODE 注入+STRICT 断言+fork-probe, 每跑打印 `MODE on` 与 `fork-probe OK`）; HelloFmt.class 在 /mnt/hostshare/jtbcfe/; JDK 21.0.12.1（与 r06 同版本）。
- 首跑乌龙记录: 在 /tmp 下跑出 ClassNotFoundException（classpath 不含 HelloFmt.class, 与本缺陷无关）, 改 `cd /mnt/hostshare/jtbcfe` 后重跑, 结论以下述为准。

## 2. 复测结果（全部本班实测, HEAD 内核）

### 2.1 3/3 rc=0, 零 CFE
```
RUN1 rc=0 [[hello] x=1 y=2.500 | [hello] done | fork-probe OK (threads+arenas alive, child exited 42)]
RUN2 rc=0 [同上]
RUN3 rc=0 [同上]
grep -c ClassFormatError hf{1,2,3,4}.out → 0 0 0 0; dmesg | grep -ciE "classformat|unknown constant" → 0
```

### 2.2 金标准审计（gdbpy17, MODE 下, 命中 r06 §2 预定复验判据）
```
OREAD[3] buf=10000010b740 size=32963 off=30043897
AUDIT buf=10000010b740 size=32963 off=30043897 ret=32963 zeros=1271 diff_vs_disk=0 fnv=5ce1291b3132a5a6 zeropages=[]
（同 run: 2524B/6688B/4605B 各读 ret=全量, diff_vs_disk=0, zeropages=[]）
```
对照 r06 §1.4 失败形态: 同 offset 30043897、同 size 32963、缓冲同落 0x100… arena window; ret 3120/3472→**32963**, diff_vs_disk 28891→**0**, 零页 8→**0**。多页 pread 短读（内核根因）不复存在。

### 2.3 类页内容非零的直接证据（-Xlog:class+load）
```
732 classes loaded, rc=0
[0.219s][info][class,load] sun.text.resources.cldr.FormatData_en source: jrt:/java.base
```
修复前该类 100% `ClassFormatError: Unknown constant tag 0`（r06 §0）; 现完整解析+加载, 即 32963 字节类页逐位有效（与 §2.2 diff_vs_disk=0 互证）。

## 3. 判定

- **不是 (a) HotSpot 内部分叉**: JVM 行为与内核无关地正确（r06 §1.3 已证 base/MODE 同构）, 短读消失后 CFE 同步消失。
- **不需要 (b) 内核修码**: gupfix 0719bc6ae74e（check_vma_flags/access_error 对 VM_CORTEN shadow-VMA 下放 fault 机器给 arena metadata 裁决）正是 r06 §2.2/2.3 建议的两个修复点, 本班实证其在该缺陷上完整生效。
- A5（b9541335a554, MODE 进程 lazy registry/teardown）叠加后无回归: 5 次 MODE java 全 rc=0, fork-probe 5/5 OK。

## 4. 处置

- **主树零改动**: `git status --porcelain` 空, HEAD 仍为 b9541335a554; 无 commit、无 tag（`corten-r07-jtbcfe` 仅 (b) 修码时需要, 未触发）、未 push。
- VM: vm-t5run5（#93, corten=on）留运行; guest 仅留 /tmp/hf*.out、/tmp/clog.txt 取证残留; 宿主/其它 worktree 未动。
- **M8 口径**: jtbcfe-final §3 的"修复落地后 JThreadBench 全跑通转正式"前提已满足（gupfix.md §3: JThreadBench 2000 线程 ×3 reps rc=0 零 CFE; 本班在含 A5 的 HEAD 上复证 HelloFmt 腿）——JVM 腿 soft-fail 可转正式, OQ-JTBCFE 缺口正式关闭。
- 交接备注: 终验须用 vm-t5run5（端口 10026）或任何 bzImage≥b9541335a554 的 VM; 端口 10022 的 `vm` 会话内核为 m6t1 worktree 滞后构建（g5c545359e856-dirty）, 不代表主树, 下班交接时应修正或重启。

## 5. 复跑清单

```
ssh(~/vm/gssh2: ssh -i ~/vm/trixie.id_rsa -p 10026 root@127.0.0.1) \
 'cd /mnt/hostshare/jtbcfe && LD_PRELOAD=/mnt/hostshare/t5run2/corten_mode_hook.so java -Xmx512m HelloFmt; echo rc=$?'
# 金标准审计: 同上, 前缀 gdb -q -batch -x gdbpy17.py --args
```
