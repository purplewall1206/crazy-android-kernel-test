# M-V V-D 泄漏猎杀报告：guest 电池 4×4096 pgtables BUG 行（根因 + 修法 + 锚）

- worktree: `/home/ppw/linux-6.18-mva`（mv-a0 = 8c4706e1e862 + V-D 增量 + 本片泄漏修复）
- diff: `patches/r07-mvd.diff`（全量 +954/−83，5 文件；V-D 主体 + 本片 ~+80 行修法 + ~330 行锚/清理）
- 未 commit；checkpatch --strict **0E / 0W / 0C**（1186 行）

## 1. 根因（两句）

**V-D exit 走查的相位 B 把 PMD 页退役与 PUD 页退役交错在同一个按 arena 升序的
registry 遍历里：第一个触达某 512GiB P4D 段的 arena 在自己的迭代里评审共享 PUD 页
时，同段更后 arena 的 PMD 页还坐在里面 —— 全 none 扫描失败、保守跳过、游标
（done_pud_seg）已记账、永不再来；该 PUD 页的 4096 记账与页面本体双双滞留，
legacy free_pgtables 又因窗口域无 VMA 永远下不去 —— 每个 such mm 恰好一条
"BUG: non-zero pgtables_bytes on freeing mm: 4096"。** 触发几何 = 两个 live arena
同 P4D 段、不同 PUD 段（≥1GiB 间距）。

## 2. 形状（不是 punchfork —— 是 metis）

电池时间相位（52-53s）是误导：BUG 行在 __mmdrop 打印，idle CPU 的 lazy-TLB
active_mm 把 mmdrop 推迟到 punchfork 的 fork 风暴唤醒 CPU 时才落地。按 workload
在 guest 逐台复验（vm #91 内核）：

- **metis_eq 单跑即稳定复现（~2 行/跑）**；smoke +1；punchfork 探针 0；
- strace 破译 metis 形状：8MiB 语料 FILE 区 @16T（4 窗）+ 128MiB PROT_NONE
  arena @16T+1G（尾 64MiB CHUNK munmap）+ 小 arena @16T+2G —— **三个 PUD 段、
  同一个 P4D 段**；钩子的 fork-probe 子进程与 worker 各自继承后退出 = 每跑 2 行；
- 4 行 = metis×2 跑 × 2 个 mm（52.63/52.64 与 53.153/53.154 两对完全吻合）。

**定位链**（guest 内核临时插桩，全数已移除）：exit 入口 pgt 超出实链页 →
走查后 legacy 前仍 +1 页 → 地址级追踪显示走查退役了全部 PMD 页
（seg=16T / 16T+1G / 16T+2G）但**没有 pud-page free 行** → 相位 B PUD 臂的
`pud_page_clear` 在第一个 arena 的迭代点必然失败（后两个 arena 的 pud 项还在）。

## 3. 修法（只动泄漏路径，B1/B2/B3 三趟）

`corten_arena_exit_walk()` 相位 B 由"单遍交错"改为**每级一趟升序 registry 遍历**：

- **B1**：全部 arena 的 PMD 页退役（PUD_SIZE 段，done_pmd_seg 游标不变）；
- **B2**：全部 arena 的 PUD 页评审（P4D 段，done_pud_seg）—— 跑在 B1 之后，
  全 none 扫描看到的是整段下层已清空；
- **B3**：p4d 页（仅 la57 实存；折叠 no-op）—— 同理跑在 B2 之后。

修法只**重排既有 pX_free_tlb/pud_clear/mm_dec_nr_* 退休语句的执行次序**，
不动判据、不动相位 A、不动 V-C 渲染与 A.3c walker（红线 ✓，INV6 ✓）。
代码内注释完整记载了交错序为何是错的（含 metis 几何）。

## 4. KUnit 锚（两个新用例，97/97 全绿）

1. **`corten_arena_test_exit_multi_segment`（根因锚）**：16T 与 16T+1G 两个
   carrier arena（各 2 窗带内容），真实 `mmput()->exit_mmap()` 漏斗（mmgrab
   保活断言）—— 断言 `mm_pgtables_bytes==0` ∧ exit_upper_pmds Δ==2 ∧
   exit_upper_puds Δ==1。**修复前实测失败，输出与 guest 逐字同形**
   （`mm_pgtables_bytes == 4096` + `BUG: non-zero pgtables_bytes: 4096` +
   upper_puds 差 1）；修复后绿。
2. **`corten_arena_test_exit_punchfork`（交错回归锚）**：8 轮 punch→fork→
   子退→munmap 全链（真 do_mmap 放置 + 真 memfd MAP_FIXED punch 漏斗 + 真
   子 mmput），断言每个子 mm 与父 mm pgtables==0。实测该形状本就干净——
   锚的价值是把四个候选形状里最大的一个钉死为回归基线。

## 5. 验证结论

| 项 | 结果 |
|---|---|
| make -j8（-j12 实跑） | PASS（bzImage #128） |
| KUnit =on（corten=on, filter_glob=corten*） | **corten 24 pass/1 skip、corten_arena 97 pass/0 fail、corten_fault 31 pass/2 skip**，零 not-ok，零 4096 残留（8192 族 16 行 = V-D 既有低址测试几何，与基线同数）；results/r07/mvd/kunit-leakfix-on.log |
| KUnit =off（默认引导） | 三套件全绿（skip=门约定）；kunit-leakfix-off.log |
| 锚有效性 | 交错序（修前）下 multi_segment 锚稳定红出 guest 同形 4096；B1/B2/B3 下绿 |
| guest（本会话 vm 内） | **metis_eq×3 → dmesg pgtables BUG 行 0**（修前 5-9 行）；exit_upper_puds 计数恢复前进（12 pmds/6 puds） |
| =n 折叠 | 14 对象构建 0E0W，nm 零 corten 符号 |
| checkpatch --strict | **0E/0W/0C**（1186 行） |
| 红线 | INV6 ✓；=n ✓；未 commit ✓；渲染/walker/V-D 其余主体零触碰 ✓ |

**全量 guest 电池复验留给主会话**（判据不变：串口 `grep -c "non-zero
pgtables_bytes"` == 0）。注：本会话为定位曾反复重启 `vm` tmux 会话，当前 VM 跑的
#125 与终版 #128 仅差测试文件空白——复验前请用终版 bzImage 重启。

## 6. 顺带登记（不修，移交）

- **punched arena 的整程 munmap 返回 -ENOENT**（`corten_arena_pool_release` 按
  start-frame 查找，洞帧为 NULL）：probe punchfork 每轮 `munmap(w, 8M)` 静默失败，
  arena 连同植入体滞留至进程退出（MemFree 对账靠 exit 收口）。V-C 起即存在
  （mvc gate 同形），非本片泄漏，锚中以 -ENOENT 断言记录现状。修法候选：
  pool_release/EXACT 分类用 arena->start..end 内任一命中帧解析描述符。
- **CHUNK maps 残段 FAIL**（"maps shows 1 segment"）为 V-C 双源渲染对
  chunk-dropped 窗口的既有行为（mvc gate 同 FAIL），与本片无关。
- CHUNK zap 会为其首个窗口 fill_upper（如 64MiB PROT_NONE 尾巴的 zap 路径），
  为无内容窗引入 PT 页；行为正确（退出/释放路径成对回收）但值得在 V-E 记一笔。
