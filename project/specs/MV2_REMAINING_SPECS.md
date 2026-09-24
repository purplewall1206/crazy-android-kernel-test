# MV2 收官规格补全（W-4/W-5/W-6）+ MV3 路线规格（D29 执行层）
2026-09-24。基线：主树 HEAD（W-2 入库后）。授权：STATE D28/D29。
W-3 已独立交付（specs/MV2_FULL_REMOVAL_SPEC.md W-3 段 + next/w3-dev-report.md 的 W-3b/W-3c 移交）。

## W-4 入场扫入（resident VMA→region 迁移事务机）
- 触发点：prctl MODE ENTER 时对当前 mm 的全部既有 VMA 逐条判定迁移：
  匿名私有→metadata region（内容走 W-1 原生读，零拷贝——PTE 已在，逐条
  "adopt"事务：meta 置 MAPPED/prot、树摘除、帧表登记）；file 私有→V-B file
  region（V-B.1 attach + B.2 门已备）；file 共享/MAP_SHARED→结构性保留
  （迁移会破坏 pagecache 共享一致性，登记 wl SHARED 桶）；vdso/vvar/special→
  W-3 件 4 同款排除（special-region 计数）。
- 关键风险：迁移窗口的并发 fault（持 mmap_write 全程，EXIT 同级大锁窗）；
  RSS/记账逐条对账（RLIMIT_AS/pgtables_bytes）；迁移失败的 unwind 逐条回滚。
- 规模 ~500 行 + 测试 ~250。KUnit：adopt 三态（匿名/file/special）+ unwind。
- guest 判据：MODE ENTER 后 /proc/maps 的委托域仅剩 SHARED/special 桶；
  wl_delegated 计数对账。

## W-5 植入消灭（显式 MAP_FIXED 全路由）
- A.3a 的 P1b idle-eject 已让"窗口 MAP_FIXED = eject+region"，遗留：
  punch 植入片（file MAP_FIXED over live arena）与 shadow split 幸存片的
  登记白名单。W-5 = 显式地址 file 映射全路由 file region（V-B 机制+显式
  地址形态），植入登记表降级为不可达 backstop。
- 依赖：V-B file 机制（在树）；与 W-3 exec 镜像共用显式 file 路由。
- 规模 ~200 行 + 测试。判据：registry 恒零断言翻转为"登记表 API 恒不可达"。

## W-6 终判据（MV2 DoD）
- J1 严格零（含植入豁免移除——W-5 后无植入）；J2 白名单九类收缩至
  SHARED/special/stack-w3b 三桶；J3 maps 双源（含 exec 镜像行）；J4 不变。
- **树归零 live 断言**：MODE mm 的 maple 树条目数 == 0（debugfs 常驻 +
  KUnit 锚 + guest 电池）——vdso/vvar 对（2 条/进程）按 W-3 件 4 排除口径
  计入豁免表，豁免外的条目 = FAIL。
- 零改动回归集全量 + 跨内核 J3 字节对拍（A.1 基线快照补拍）+ LoC 终账。
- 交付：REPORT M-V2 章。

## MV3 路线（D29 目标 2：全应用无损接管）
- MV3.a 默认进场：execve 换 mm 即 MODE（binfmt 钩子/`corten=on` 全局门），
  无 prctl 依赖；suid 安全立场（CORTEN 对 suid 同样接管——纯内核态无 ABI 面）。
- MV3.b 无损闭合清单：PROCMAP or-next 残差、bpf_iter/trace 符号化（#37-40）、
  mseal、S-3 read-back 发现、madvise WILLNEED 族——从登记升级到实现。
- MV3.c mmap-pf 批 mark 重构（性能税，默认接管后为全进程税 → 必做）：
  fault 路径簿记合并，目标把 -17~-22% 地板收窄到个位数。
- MV3.d 全系统 MODE 电池：systemd 全启动 + gcc 自编译 + LTP 抽样，
  全程 MODE 无回归。
- MV3.e 删除账兑现：对全部进程成为死代码的 VMA 层行列出可删清单
  （不实际删——登记为"arena 默认化后的裁剪 PR"边界件）。
- 顺序：MV2 收官 → MV3.a/b → MV3.c → MV3.d/e。
