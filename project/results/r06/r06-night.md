# r06 夜报 — 2026-09-17 夜窗（23:18 起）

**目标: D-G'' 修复验证 → 提交 → T5 首跑（第一组 MODE 性能数字）。结果: 第一段验证判据
9 条未全过 → VERDICT=FAIL → 按任务纪律停（不 commit、T5 不跑、生产码零改动）。
产出: 分层定责报告 + 1 个新 P1 缺陷锚点 + 1 条 r05 文档口径证伪 + T0b 既有下游缺陷定性。**

详细判据级证据: `results/r06/dg2-verify.md`（本报告为其执行摘要）。

---

## 1. 三段执行情况

| 段 | 判定 | 说明 |
|---|---|---|
| 第一段 验证 | **FAIL** | 构建/KUnit/=n 三关全绿; guest 9 条判据 5 PASS + 2 PARTIAL + 1 N/A + 2 FAIL（详见 §2） |
| 第二段 maintainer | **未执行（按纪律停）** | 前置条件"验证 PASS"不成立; 主树保持 9d74b22a1348 干净未动 |
| 第三段 T5 首跑 | **未执行（按纪律停）** | 依赖第二段的提交; 且 T0 臂必踩 T0b 既有 metis/java 崩点（§3-②）, 数字无效 |

## 2. 第一段要点（全链证据见 dg2-verify.md）

- **验证脚本三处自身缺陷已修**（生产码 5 文件零改动）: ①corten_fault_test.c const-vm_flags
  编译断裂（日窗未 make, `vm_flags_init` 惯用法）; ②punch_head 两处（尾窗 fill_upper 缺失 +
  kthread 直调 do_munmap 违反 corten_arena_test.c 文档化 worker 惯例 → NULL current->mm Oops）;
  ③探针两处（魔数断言 → pread 硬比对; 越 EOF 读 → fstat 截断）。diff 再生 1149 行
  checkpatch 0E/0W/0C, 日窗原版备份 `.dayshift`。
- **构建/KUnit/=n 全绿**: 零新增警告（仅 2 条登记基线条）; KUnit 冻结版 on×2+off×1 全绿
  （corten_fault 19/0/0 含 punch_hole/punch_head, B1 指针恒等断言过; 分层口径已注明）;
  =n 七对象零警告。
- **guest 判定（corten=on, trixie, 9 条判据）**:
  - PASS: E1（file-MAP_FIXED punch 读回=文件真值, /proc/maps 三明治逐字: 头片 arena + rw-p
    /bin/true + 尾片 arena）、E2（匿名 MARK 回归）、E5（NOREPLACE=-EEXIST / FIXED 成功可读）、
    计数器（mmap_punches 精确 +、rejects=0、eagain_leaked=0、drain_timeout=0）、
    mode_smoke 26/26、fork 冒烟（fork_demote 多片, +1）、dmesg 零 WARN、KUnit 2 新用例。
  - FAIL: java -version rc=0（MODE 下打印版本横幅后在 libc 崩）; metis_eq on-run rc=0（SIGSEGV）。
  - **新 P1（diff 引入/暴露）: DECLARE+punch 形状进程退出死锁** —— exit_group → exit_mmap →
    free_pgtables → **__vma_start_write** 挂死, D-state 对 SIGKILL 免疫, 近三轮 5/5 复现;
    T0b 对照上同形状走不到干净退出（pre-fix 读阶段即 ACCERR 崩）, 故为修复后首次可达路径。
- **对照实验矩阵定责**（D-G'' worktree 内核 vs 主树重建 T0b #26）:
  - metis_eq MODE-on SIGSEGV、probe e4 整 arena mprotect -EOPNOTSUPP: **两内核同现 → T0b
    既有, 非本 diff 回归**;
  - java: T0b rc=134 SIGABRT（无横幅）vs 修复内核横幅后崩 → **修复严格改善但 rc=0 未达**;
  - E1 读回: T0b 上该路径不可达（r05 已证 D-G'' 签名 ACCERR）→ **D-G'' 修复本体实证成立**。
- **r05 文档口径证伪一条**: dg2-analysis §4-E4 所记"整 arena mprotect EXACT 抬门成功"在两个
  内核实测均为 -EOPNOTSUPP（已登记勘误）。

## 3. 遗留与移交（修复班/规划者）

1. **P1 退出死锁**（阻塞提交）: 复现=dg_probe3 e1b/e5（share `r6dg/` + 源码
   `results/r06/probe/`）; 锚点栈已抓; 现场 VM/镜像/share 原样保留。
2. **T0b 既有下游缺陷（建议新立任务）**: "RW 提交区装 present-RO PTE"（metis_eq/java 同签名,
   dmesg `segfault at 10000c000030 error 7`; 候选区=mprotect 路由 pending-perm/zero-page 升级）;
   整 arena mprotect EXACT -EOPNOTSUPP 同属此层。
3. diff 本身: 生产码语义在探针级全部成立; 3 处验证脚本修复已折入 `patches/r06-m4dg2.diff`。
   验证条件恢复后, 构建/KUnit/=n 证据可直接复用, guest 只需补 P1 修复后的 e1b/e5/fork 退出
   + java/metis 两条 rc=0。

## 4. 时间线与合规

- 23:18 起跑; 01:5x 定稿。全程 timegate 放行; 未 push、未动其他 worktree、密码未落盘;
  主树保持 HEAD=9d74b22a1348 干净（仅重建了 T0b 对照 bzImage, 未提交任何东西）。
- VM 留运行: worktree D-G'' 内核 boot（corten=on mitigations=off kunit.enable=0）,
  share `r6dg/`（探针+hook+smoke+metis）与 `r6dg-results/`（终态产物）原样。

*产物: results/r06/{dg2-verify.md, kunit-on1..4/off1/off2.log, build-y-full.log,
build-n-seven-objects.log, probe/dg_probe3.c, probe-out/*.out, dmesg-final.txt,
arena_stats.final, arenas.final, battery-round2.log}; patches/r06-m4dg2.diff{,.dayshift}。*
