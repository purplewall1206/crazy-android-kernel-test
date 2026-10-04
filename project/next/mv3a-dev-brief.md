# MV3.a 开发任务书 · 默认进场（execve 即 MODE, D29 目标 2 头片）

授权: 用户指令"MV3 开工，按 D29 路线走"（2026-10-04）+ specs/MV2_REMAINING_SPECS.md
§MV3.a + STATE D29。基座: worktree /home/ppw/linux-6.18-mva（HEAD 同步主树）。

## 目标
corten=on 时全部进程 execve 换 mm 即自动进 MODE——无 prctl 依赖; suid 同样接管
（D29 裁决: 纯内核态无 ABI 面, 不豁免）。

## 设计默认（主会话裁决, dev 可在设计稿里提出异议但默认执行）
1. **门位**: fs/exec.c 的 exec_mmap()（新 mm 安装点, 早于 load_elf 的全部
   do_mmap）——corten_enabled 静态键下置 mm->corten_mode=true。**不调**
   enter_sweep（此刻 mm 近空, sweep 无物可收）; registry/state 走 A5 惰性
   （首个 arena 工作按需建）。后续 ELF 段加载的 do_mmap(MAP_FIXED file/anon)
   由既有路由承接（W-5 explicit_region_route + A.2a auto attach）, brk/heap
   走 W-3 件, 栈路由既有（smoke 的 mmap-stack-routes-to-window 先例）。
2. **开关分离**: 新 boot 参数 `corten_mode_default`（默认 off）。corten=on
   维持现语义（仅 prctl/hook 进场）; default=on 才启用 exec 默认进场——
   既有全部电池语义零扰动, MV3.d 全系统电池时翻它。
3. **计数/可见性**: exec_default_enters 计数器 + debugfs 行; /proc 面无新增
   （mode 读数已有）。
4. **suid**: 不特判（D29）。setuid 位不清除（内核接管无 ABI 面）; 在报告
   披露安全立场一句。
5. **已 MODE 进程 re-exec**: 新 mm 从零开始, 位重置后再置 = 幂等, 无特判。
6. **kthread/内核线程**: exec_mmap 只服务用户进程, 天然不涉及。

## 红线
- corten=off / default=off 双 off 路径零扰动（编译折叠+静态键, 基线永存）。
- 只动 fs/exec.c（门位一行级）+ mm/corten_arena.c（计数器+参数）+
  include/linux/corten_arena.h + mm/corten_arena_test.c + KUnit。
- checkpatch 0E/0W; =n 折叠; fail-open。

## KUnit 锚
- exec 门函数直驱合成 mm: mode 置位 + 后续 mkvm 走 auto 路由（窗口落位）。
- default=off 时门函数不置位。
- 计数器前进断言。

## 验证链
=y 零新增警告 → KUnit on×2/off×1 → =n 13 对象+fs/exec.o → checkpatch →
guest 门: **corten_mode_default=on boot**（新参数）→ ps 全进程族无 hook
LD_PRELOAD 下: 任取 3 进程（systemd 子进程/bash/sshd）读 mode 位=1 + arenas
表见其 region + smoke v2 26/26（hook 形, 应幂等）+ **裸 smoke（无
LD_PRELOAD, 靠默认进场）26/26** + metis_eq 裸跑（无 hook）checksum 同基准 +
dmesg 静默 + =off boot 回归冒烟。报告 next/mv3a-dev-report.md。

## 坑清单
同 W-7 brief 尾部（pidfile/-j12/glob/9p/interlock/串行 make）。
