# W-5 开发任务书 · 植入消灭（显式 MAP_FIXED 全路由）

授权: specs/MV2_REMAINING_SPECS.md §W-5 + STATE W-4 收口条目遗留①②③ + D28/D29。
基座: worktree /home/ppw/linux-6.18-mva @ 893c804（HEAD, 干净, =y .config 在位）。
代码风格: ponytail full（最短可用 diff, 复用既有 helper——尤其 frame 退役与 file 路由;
不做未要求的抽象）。DoD/验证协议不打折（用户明确要求）。

## 目标
1. 显式地址 file 映射全路由 file region: 现走 punch+implant backstop 的形状
   （file MAP_FIXED over live arena 的植入片、shadow split 幸存片）改为
   V-B file region 路由（corten_file_may → declare(file)/attach + B.2 失效门 +
   B.3 fault 双臂, 全部在树）。路由失败形状 fail-open 留 legacy, 但不得再产生
   新的 registry 写入（backstop 路径保留为不可达死代码, 注释点名 W-5 判据）。
2. 植入登记表降级为不可达 backstop: W-5 后生产路径零 registry 写入。
   判据（规格原文）: 「registry 恒零断言」翻转为「登记表 API 恒不可达」——
   代码审计口径（corten_implant_mark 的全部生产调用点枚举 + 每点不可达论证）
   + guest 全电池 implant/registry 相关计数恒零。
3. W-4 遗留收编:
   ①混合帧 PT 退役: exit walk owned-mixed 臂 zap 后, 若帧内 PTE 全空
   （ptl 下扫; arena 槽已 invalid + 共居 legacy 无驻留）→ 复用既有整帧退役
   helper 收殓 PT 页（消除 pgtables_bytes 残值; W-4 遗留①）。
   ②两锚 mmput-action 卫生: sweep_fork_mirror / sweep_file_exit 的裸
   mm_alloc 改 kunit_add_action(mmput) 形（mixed_frame_exit 已有范例）。
   ③j2_stale=3 瞬态: registry 不可达后应消失, guest 验证时确认归零。

## 红线
- INV6（PTE 写经事务）; 树摘除/路由全在 mmap_write 下; 失败臂 fail-open;
  =n 折叠完备; checkpatch --strict 0E/0W。
- 已编译程序零破坏: 显式 file 映射的兼容契约不因路由改变（成功语义不变,
  仅宿主从 legacy VMA 换成 region）。
- 文件白名单: mm/corten_arena.c / mm/corten_arena.h /
  include/linux/corten_arena.h / mm/corten_arena_test.c。超白名单（如需动
  mmap.c 路由点）先在报告里说明理由再动。
- 不 commit（主会话收口）; 不动主树; worktree project/ 噪声勿碰。

## 验证链（全部落 results/r07/w5/）
make =y 零新增警告 → KUnit on×2/off×1（filter_glob=corten*, 三套件全跑,
旧锚不红）→ =n 13 对象零符号 → checkpatch --strict → guest 门
（trixie.img/10022/pidfile 纪律; smoke v2 26/26 双形态 / metis_eq ×2 checksum
/ sweep-live / mva1_probe / S-3 电池 / **pgtables_bytes 残值 == 0**（遗留①
收编后判据）/ registry 计数恒零 + j2_stale 归零 / dmesg 静默）。
报告落 next/w5-dev-report.md（含 registry 调用点不可达论证表）。

## 坑清单
pidfile 杀 qemu（pkill 自匹配 tmux server）; filter_glob=corten* 带星号;
9p 手动挂载; KUnit 无盘 boot 尾 VFS panic=标准收场; interlock 用例负载
敏感复跑绿; make 串行化（同时只允许一个构建）。
