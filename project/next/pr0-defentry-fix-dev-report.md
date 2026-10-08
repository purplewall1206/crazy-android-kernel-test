# PR-0 =on 默认进场回归 —— 根修报告（2026-10-08 上午, 主会话修复轮）

## 0. 判定一句话

根因 = **W-7 帧的 KEEP_PERM 陈旧槽 perm 与 PR-0 收编 region 的新合同竞争**：
exec 期 `corten_arena_zap_window()` 的内容清除以 `CORTEN_UNMAP_KEEP_PERM` 保留
槽内旧 perm（(INVALID, perm=9) 形），随后 `vm_brk_flags` 的 bss declare
（PR-0 路由，"tree-zero、registry zero-write"）不写槽元数据；首写 fault 进
FRESH 门时，"槽带 perm 即 mprotect 合同"的仲裁让**陈旧只读 perm 赢过
declare 的 RW 界** → ACCERR → init 首个用户写 SIGSEGV → kernel panic。
修复 = **anon 新合同 declare 的槽 scrub**（`corten_scrub` + declare_locked
挂接）：declare 成功路径上把 `[addr,len)` 槽位重置为 pristine，FRESH 门改由
`ar->prot` 派生合同。

## 1. 诊断链（六轮崩溃 boot 的收敛，console 存 pr0-fix/）

| 轮 | 构建/形状 | 读数 |
|---|---|---|
| console-dbg-crash1 | small2-wt @2d3febc4 + 19 诊断点 | `bss DECLARE ok [100000037000,100000038000) prot=3`；`fast ACCERR addr=100000037000 ar=[同一区间) st=2` —— fault 落在**正确 arena**，被判 ACCERR |
| console-dbg-crash2 | +FRESH 门/COW_COPY 打点 | `FRESH-gate ACCERR write=1 gate.perm=9 m.perm=9 ar.prot=b` —— **槽带 9（USER\|READ，无写位），declare 界 0xb 被绕过** |
| console-dbg-crash3/4 | +corten_mark/DROP 追踪（过滤器常量错一轮） | 零合法写入者 → 槽内容非 declare 后写入 |
| console-dbg-crash6 | 修正常量后全链追踪 | `DROP page=100000037000 keepperm=1 m.perm=9 from corten_arena_zap_window+0x1ed`（**先于** declare 6.135→6.143）；query 命中 (INVALID, perm=9)；FRESH 门 ACCERR |

崩溃面身份（由 w6v2 底图 debugfs 抽件对证）：RIP `0x10000001de6d` 与写目标
`0x100000373f6` 均落在**映射于窗口基址的 ld-linux-x86-64.so.2**（偏移
0x1de6d 的指令字节与 console Code 逐字节一致）；写目标在 ld.so `.bss`
（0x36b60–0x375e8），正是解释器 bss 经 vm_brk_flags 被 PR-0 收编的靶面。
陈旧 perm=9 的来源 = exec 镜像采纳期的窗清除（与 elf_map pad-unmap 的窗尾
punch 形状一致），KEEP_PERM 保留了 RO 段的槽 perm。

## 2. 修复机械（309 行 diff，4 文件）

- `mm/corten.c`：新协议函数 `corten_scrub(txn, start, len)` —— 槽位重置为
  pristine（state 不动，perm/flags/__resv 清零）；调用方合同 = [C1] 空性
  探针已过（无内容），遇内容槽 WARN+拒绝（协议漂移的响亮面）。
- `include/linux/corten.h`：声明 + =n stub（`-EOPNOTSUPP` 折叠）。
- `mm/corten_arena.c`：`corten_arena_declare_scrub{,_one}()` —— 逐帧事务；
  **整帧范围拆两半**（lock_range 的 covering-level 测试对整帧回答 PMD，
  PTE 级事务无法表达整帧 —— 首轮实现的 -EOPNOTSUPP 回归由
  declare_probe_stale_pt/probe_skip_mm_attribution 两锚抓出）；锁范围跳过族
  = {-ENOENT（无 PT 页，pristine by absence）, -EAGAIN（描述符退役，M2a
  豁免）, -EOPNOTSUPP（huge leaf，无 PTE 级元数据）}，仅 -ENOMEM 传播
  （退化到 funnel，安全向）。挂接点 = declare_locked 的 **`!file && !adopt`
  臂**、frame 插入前、out_unwind_pin（未发布即退化）。FILE 臂自 scrub
  （whole-region mark 覆写全槽）；adopt 臂不 scrub（范围即内容，sweep
  两阶段回填）。
- `mm/corten_arena_test.c`：新锚 `pr0_bss_stale_perm` —— 头 punch（基锚定、
  尾 >2M，避开 release 精化的 park 重分类）制造 (INVALID, RO) 槽 → bss
  declare → 断言 scrub 合同（bss_declares+1、树上无 VMA、槽 pristine、
  region RW）。首写服务与内容持久面由三联 =on boot 与 pr0_bss_adopt 锚
  承担（见 §4 P2-b）。

## 3. 门读数（最终工件 09:54 bzImage，除非注明）

- 生产 config（主树 .config，非 lockdep）`corten=on corten_mode_default=on`
  全系统 systemd boot ×3 连续（全新 overlay，09:20 轮）：ssh up ×3、
  panic 行数 0/0/0、kernel `6.18.32-g7c4ceef8abe0-dirty`。
  【注：此轮跑在 09:16 中间构建上；最终工件 ×3 复跑见 §3b/STATE 收口条】
- KUnit on（filter_glob=corten*）：corten 26/0/1 + arena 150/0/0（含新锚）
  + fault 34/0/5（kunit-on4，149/1 轮的 1 红即下述 P2-b 的锚环境面，冻结
  后归 kunit-on6+ 复跑）。
- checkpatch --strict：0E/0W/0C（309 行）。
- 中间构建教训（登记）：09:16 构建先于 scrub 整帧拆分修复，其上跑过的
  ×3 boot 与三腿电池作废重跑；kunit-on1/3 的 arena 3 红同因（两锚回归 +
  新锚形状）。

## 4. 登记的新发现（不阻塞本修复发运）

- **P2-a `BUG: non-zero pgtables_bytes on freeing mm: 12288`**：=on 默认进场
  世界 boot 期 3 次（boot1 console 11.9/12.4/13.4s）+ kunit 卸载路径。
  vma-less region 的 PT 页退休与 pgtables_bytes 计账不平衡，MV2 时代即
  存在的既有暴露（本修复不触碰 PT 页生命周期）。登记 P2，后续轮修。
- **P2-b punched-frame 上 arena 事务的异步拆除交互**：新锚在
  declare→fault→page_word 读回链上偶发 PTE 消失（kunit-on1/on5 痕迹：
  zap_window 扫窗与 declare/fault 交叠）。锚已冻结到 scrub 合同面；
  fault 服务面由 boot 世界证明。独立探针片登记（park/eject 与 declare
  的锁覆盖窗），后续轮。
- **flake 族**（既有在案）：`txn_uninstall_interlock` 于宿主 load>7 时红
  （本轮 kunit-on6，与 T3 agent 并发构建同窗），安静窗复跑绿为准。

## 5. 处置

- PR-0 =on 默认进场回归（台账 P1 首项）：**根修落地**，定罪矩阵全表转绿。
- 验收门状态见 §3 与 STATE.md 收口条；E2 二期实删的排期裁决提案另呈用户。
