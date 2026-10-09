# V4.3 EOF 三点收口验证 (bc86fffa, 2026-10-10)

## 内核身份
- 收口内核: android17-6.18 @ bc86fffa (reof 字段 + dispatch 门改声明界
  + file_read 零页安装 + file_cow 零源 anon 安装 + fetch 注释对齐)
- bzImage sha256 fdedf584c639...8710b (v1base2.qcow2 直启)

## Boot 验收 (世界: corten=on corten_mode_default=on)
- boot#2 (boot#1 在 sg adopt 后 console 冻结, 未见 panic/BUG, 判镜像
  态偶发; boot#2 全程复现绿): SSHOK (trixie key) + multi-user.target
  + graphical.target + nginx 起来
- 零 trap: dmesg 无 BUG: unable to handle / GPF / kernel BUG / Oops
- smoke 26/26 PASS (run_mode_smoke.sh, arenas 546->551 回落)
- arena_stats 计数: auto_mmaps=4181 stack_adopts=351 special_shadows=702
  vma_gate=0 brk_funnel=0 file_mmaps=2282 file_read_faults=55451
  file_cow_copies=1053 file_fork_mirrors=1694 auto_fallbacks=0
  auto_exhausted=0 rearm_failed=0 eagain_leaked=0 legacy_drift=0
  desc_alloc_fail=0 free_untracked=0

## 功能探针 (efsmoke.c, 本目录)
1 页文件 + 3 页 MAP_PRIVATE region (memsz > filesz 形状):
- PASS in-file content (0xAB)
- PASS past-EOF read zero (收口点: dispatch re-dispatch + file_read 零页)
- PASS past-EOF write readback / file not clobbered (收口点: file_cow 零源 anon)
- PASS fork past-EOF private page
- PASS window placement 0x100000000000 (auto file 臂路由)
- 计数增量: file_mmaps 3025->3036, file_read_faults +99, file_cow_copies +6
- NO_TRAPS

## 遗留观察 (非本改动引入)
- mm exit 时 V3 阴影 rss 漂移 (+2 MM_FILEPAGES / -2 MM_ANONPAGES,
  dhcpcd/systemd-journal) = 已入档 WARN 级已知问题 (plan sec J6 待核验项)
