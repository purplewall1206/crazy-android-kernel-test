# W-7 开发报告 · multi-record registry（frame-sharing 收编完备）

2026-10-04。agent: w7-dev (kernel-MM architect+dev, ponytail full)。基座
/home/ppw/linux-6.18-mva @ 856416f7ceec（干净；本片内核改动不 commit, 主会话
收口）。设计稿 next/w7-design.md 经主会话批复（附两项强制条件: tag 解引用
穷举审计 / 相位 A 专属红绿锚, 均§2/§3 落地）。任务书 next/w7-dev-brief.md。
工件 results/r07/w7/。**不 commit。**

## 0. 判定总表

| # | 判据 | W-6b | W-7 判定 | 关键读数 |
|---|---|---|---|---|
| J4 | 树归零 live（白名单外==0） | FAIL（5/静态 + skip_declare 288/电池） | **多段 ELF 负载 MET**: j3w live delta file/anon/unclassified 全 0, tree=5 全白名单; skip_declare 288→**0**; smoke 家族残余=其自身 stay-legacy 夹具（§5.2 wl 家族说明） | §5.2 |
| J2 | 白名单收缩 | unclassified=0, file/anon=frame-sharing 残余 | unclassified=0 维持; file/anon 逐负载 delta 0（残余=smoke 披露夹具, 非收编缺口） | §5.2 |
| J5 | 零改动回归集 | PASS | KUnit 全绿 + guest 电池见 §5 | smoke/JTB/metis/probe/S-3 |
| J1/J3/J7 | （W-6b 已绿面） | PASS | 回归维持见 §5 | j1/j2/maps 族 |
| LoC | 供数 | — | **+1636/−598**（corten_arena.c +1660 区间/头文件 77/测试 +484/fault_test 13; w7-full.diff 3047 检查行） | w7-full.diff |

## 1. 实现摘要（对设计稿的落地 + 两处设计演进）

案 A「帧内记录桶」全量落地: `corten_frame_bucket`（{nr, rcu, rec[] 按
start 升序}）+ 槽值三态（裸指针/`xa_tag_pointer(bucket,1)`/哨兵）+ 解码
helper 封闭（`corten_slot_bucket/arena/cover/has` + 写侧
`corten_slot_insert/remove` + 按起始解析 `corten_slot_by_start`）+ R1
遍历（`corten_registry_next`, 无状态 first-surviving-slot 规则）。INV2'
（同帧记录页域两两不相交）由 C1' 页域相交判定在建桩三写口
（declare/adopt、fork-child、brk grow）建立; `corten_region_invariants_ok`
扩展桶内两两不相交断言。

**设计演进两处**（实现期发现, 均为设计稿未覆盖的正确性面, 随批复原则
"其余照设计稿执行" 内的缺陷修复处置, 此处如实披露）:

1. **punch 头收缩（head-shrink）**: R1 的"头打洞重锚"子句在多帧头洞
   记录上逐帧重发（exit_punchfork 的 double-kill 实测照出）→ 改为 punch
   路由语义修复: 头打洞把记录起点重锚到 punch 端（ar->start = ps）,
   auto 记录退还前缀 charge、FILE 记录前移 rpoff（INV-MV3(d) 保持）、
   ps 帧槽重挂。R1 退化为纯 start-frame + 头洞探针两级, 所有洞形单发。
2. **T0 release-on-full-coverage 门控**: 原"CHUNK 且尾部 <2M → EXACT
   RELEASE"的 padding 论证只对 PMD 对齐记录成立; 页粒度记录（收编段/
   brk 区）的尾页是用户财产, 原_upgrade 会 release-kill 未点名的活内容。
   加"两端 PMD 对齐"门。

其余: 12 消费面改动按设计稿 §2 清单执行（fault 快/慢路径、GUP、fork、
exit、sweep、j2/wl（树侧零改）、magazine、pool、punch、implant（零改）、
debugfs（零改）、KUnit 夹具迁移）; `slot_cover` 从"单记录帧粒度"收紧为
页粒度解析（punch_head 锚实证: 收缩记录的洞在 lookup 层答 NULL, D-G''
契约保持, fault_owned tier1 留作事务封印防御）; 三处"窗口域不可能桶"
WARN 摘除（页粒度 targeted declare 使窗口桶合法, 成员逐一处理）;
W-4b 合并残留死代码（exit walk `continue` 后不可达臂）随相位 A 重构拔除。

## 2. 强制条件 1: tag 解引用穷举审计表

方法: `grep -n "xa_(load|find|for_each|for_each_range|store|erase)(&\(old_\)*\(c\)state->arenas"` 穷举,
逐点标注解码形态。**42 个访问点全部收敛在本文件**（外部文件零直触, 设计
稿 §0 事实维持）; 25 处 KUnit 直读迁移至访问器（`corten_arena_test_region_of`
改槽解码语义 + 新 `corten_arena_test_record_next` R1 遍历钩子）, 余 3 处
`xa_erase` 为夹具洞模拟（单记录帧 + ctl_lock 持有, 擦除即全成员移除, 语义
即测试所拟）。

| 函数（行） | 访问 | 解码 |
|---|---|---|
| registry_next (862) | xa_find | 桶解 + 成员迭代（R1 本体） |
| region_invariants_ok (2017) | xa_for_each | corten_slot_bucket + 两两页域断言 |
| arena_overlaps C1' (2109) | xa_load | bucket 成员迭代页域判定 |
| test_frame_reserved (3671) | xa_load | **无解码**: `== sentinel` 恒等比较（tagged ≠ 哨兵, 语义正确） |
| test_region_of (3743) | xa_load | bucket 成员页域解（夹具: idle 可见） |
| test_pool_idle (3872) | xa_load | slot_arena |
| exit_walk 相位A/B1/B2/B3 (4312/4404/4442/4481) | xa_for_each ×4 | slot 变量 + bucket 成员裁剪 zap（帧键重构, §3） |
| arena_lookup (4672) | xa_load | slot_cover（页粒度） |
| va_seg_claim (5774) | xa_load | bucket → 整帧跳（成员全在帧内）; 单记录 slot_arena |
| seg_claim 标记 (5803/5808/5817) | xa_store/erase | 自有已验空段哨兵标记/ unwind（无解码需求） |
| mag_alloc_cpu recycle (5879) | xa_load | **无解码**: `!= sentinel` 占用判定 |
| pool_take bump (5942) | xa_load | **无解码**: 同上 |
| mode_exit 拒绝扫描 (7444) | xa_find | bucket 成员逐个查 RF_ADOPTED |
| mode_exit teardown (7478) | xa_find | bucket 首成员 + release 后同帧重扫 |
| fault_owned tier2 (8827) | xa_load | slot_has |
| txn_owned [F-B seal] (8870) | xa_load | slot_has |
| pool_prepare 首帧 (12342) | xa_load | slot_arena |
| pool_prepare intact (12353) | xa_load | **无解码**: 严格裸指针恒等（桶 → 不 intact → eject, 保守正确） |
| pool_prepare 残余 eject (12404) | xa_load | bucket 成员逐一 eject |
| pool_parkable (12449) | xa_load | **无解码**: 严格 `!= ar`（桶 → 拒 park 走真 RELEASE） |
| brk_grow_extend 校验 (12931) | xa_load | **无解码**: 非空判定（桶 = 非空 → -EEXIST） |
| brk_grow_extend store/unwind (12936/12942) | xa_store/erase | 已验空帧裸存/自有帧擦除（ctl_lock 串行） |
| placement_punch_idle (14404) | xa_for_each_range | bucket 成员迭代 eject |
| range_overlaps (15749) | xa_for_each_range | 成员迭代页域判定 |
| range_occupied_incl_idle (15816) | xa_for_each_range | 哨兵槽判定 + 成员迭代 |
| placement_backstop (15882) | xa_for_each_range | 成员迭代 |
| window_parked_span (16026) | xa_for_each_range | 成员迭代 idle 判定 |
| msync_skip (16247) | xa_load | **无解码**: 仅 NULL 判（桶 = 已登记, 语义同） |
| mincore_route (16380/16411) | xa_load | slot_arena/bucket → active 分类 |
| unuse_windows (17482) | xa_find | 成员逐 clip sweep |
| mm_state_pages (18026) | xa_find | 帧键 desc 一次读（哨兵跳） |
| shrink_eval/age_slice (18229/18293) | xa_load/xa_find | 成员 pin（CORTEN_SHRINK_PIN_MAX=4 封顶, 溢出下轮再龄） |
| declare_locked 发布/unwind | slot_insert/remove | 桶 copy-update |
| release/eject/punch/brk-shrink | slot_remove | 成员移除 |

编译期收口: 桶类型为 corten_arena.c 文件私有 struct, 解码函数 static
inline 且以 `xa_pointer_tag` 单点判定; 槽值在 42 点之外不逃逸出本文件
（外部消费面全部经 route/row token, 无一持槽）。

## 3. 强制条件 2: 相位 A 专属红绿锚

`corten_arena_test_w7_frame_share_exit`（arena 套件 #15）:

- **中形断言**（"成员 A zap 后、成员 B 未 zap 的窗口 PT 页存活"）: 同帧
  双记录（A/B 各 1 驻留页, sweep 收编）→ `corten_arena_unmap_chunk` zap
  A 的字节裁剪段 → 断言 meta(B) 仍 CORTEN_MAPPED + B 内容逐字节存活
  （PT 页/邻成员翻译未随 A 的 zap 退役; 红面 = 旧游标形整帧退役即杀 B
  的翻译）。随后 zap B → 双成员 meta 归 Invalid（KEEP_PERM 语义）, 帧
  PT 页保留（退役 = exit walk 整帧行为, `pt_present` 探针即页表层级,
  断言其存活）。
- **端到端**: `corten_arena_mm_exit` 直驱 → registry unpublish、驻留计
  数归零（`resident_pages` 帧键计数不重复）、`pt_present` 双成员 FALSE
  （PT 页退役恰一次）、drain 超时计数不动。
- 相位 A 结构性保障（代码注释 + 本锚共同封）: 游程收口 zap 为内容驱动
  （[run_start, run_end) 整帧域）, 每帧先于退役清全部共帧成员页; 混合
  帧逐成员字节裁剪 zap 后按 W-4①空帧退役。

## 4. KUnit 锚（125 → 129; 全部消费面红绿）

新锚 4（arena 套件 #13-#16, 全绿）: `w7_frame_share_adopt`（三段同帧
收编 + 页粒度 lookup/row 流/INV2' 洞 + invariants 桶断言 + 破坏-恢复 +
C1' 重申）、`w7_frame_share_fork`（桶帧镜像 + 子侧双侧解析 + INV7
checker R1 走查 checked==2）、`w7_frame_share_exit`（§3）、
`w7_frame_share_punch`（头收缩: 记录重锚 punch 端、洞回 legacy VMA、
共租 B 槽/内容/lookup 存活、invariants 绿）。

既有锚迁移: 25 处夹具直读 → 访问器; `inv7_walk` 改 R1 钩子;
`fault_test_punch_head` 的 D-G'' 契约从"裸槽 NULL"升为 lookup 级
（洞答 NULL + 记录 extent 排除洞——锚的牙齿保持, 表达式跟上 W-7 形）;
`exit_punchfork` 迭代末 munmap 期望 -ENOENT → -EOPNOTSUPP（头收缩后
范围跨存活记录界, 文档化 fail-open 姿态, 原形 = 记录无槽不可见）。

## 5. 验证链（终码全链）

| 项 | 读数 |
|---|---|
| =y 新增警告 | 0（基线: objtool cpuidle_enter_state r04 判例） |
| KUnit on×2 | 24/0/1 + **129/0/0** + 34/0/5, 终码两跑全绿 |
| KUnit off | 25/0/0 + 26/0/**103** + 7/0/32（skip 对账: W-6b 99 + 新锚 4 = 103 精确; 4 新锚 off 全 skip） |
| checkpatch --strict | **0E/0W/0C**（w7-full.diff） |
| =n | §5.1 |
| guest 门 | §5.2（终判据 tree_entries） |

### 5.1 =n 折叠

CONFIG_CORTEN* 五件全 =n（config-pre-n-w7.snapshot 存档）, 全树重建
RC=0 零错误零警告（build-n-w7.log）; mm 90 + fs/proc 30 对象 nm 零
corten 符号（nm-n-w7.txt; 5 个陈旧 =y corten*.o 移除后增量重建零产出,
n-stale-objects/ 存档）; task_mmu.o =n 干净。config 已恢复 =y 重建。

### 5.2 guest 门（bzImage-w7-final sha256 02209a67…, 双跑）

**run 2（净单 boot, console-w7v2.log 全程串口留档）终读数**:

| 项 | 读数 |
|---|---|
| smoke v2 | **26 PASS / 0 FAIL** 双形态, arenas 退出归零, SMOKE-DRIVER PASS |
| JTB | 2000×3 ×3 全 rc=0 |
| metis_eq ×2 | rc=0, checksum **同基准**（65073 词 2d383eeed4ceb73b） |
| sweep-live | **rc=0 RESULT PASS** |
| mva1_probe | 17 [ok] + 1 [FAIL]（CHUNK maps-段数 = W-6b §1.2 陈旧预期, binary-only, 披露维持） |
| S-3 | 双分支 rc=0 PASS |
| J3 oracle | 注册驱动 rc=0, in-boot 断言 PASS |
| **W-7 终判据（j3w 多段动态 ELF, live pre/post delta）** | **wl_file +0 / wl_anon +0 / wl_unclassified +0**; tree_entries=5（pid 归属一致）全部白名单内（stack×1+special×3+brk×1）——D34 多段 ELF 负载收编完备直证 |
| sweep 计数族 | adopts anon 90 + file 405（W-6b: 13+34）; **skip_declare = 0**（W-6b 288）; skip_other/shared/filemay/ops/uffd = 0; skip_flags=1（S-3 mlock 形, 规格豁免）; skip_stack/special/brk = 白名单桶 |
| pgtables 残值 | 2 笔 8192B（④ 族 C2 形, 容差内）; j2_stale=0; gate_pass=1; dmesg corten 静默 |
| wl 家族说明 | 累积 wl_file=1/wl_anon=10 = smoke 家族自身披露的 stay-legacy 夹具（`mmap-shared-stays-legacy`/`mmap-populate-stays-legacy`, 测试名自证）+ mlock 形, 非收编缺口（delta 逐负载为 0） |

run 1（host 重载窗口: 4h =n 构建 + A.1 VM 同机）: smoke/JTB/metis 同绿后
sweep-live 段 guest 异常重启一次, panic 未捕获（pane 滚出）, 净单 boot
run 2 未复现; run 2 起串口全程落盘（console-w7v2.log）。两跑全部阶段
读数均绿（run 1 sweep-live 超时 124 于净机复跑 rc=0）。

## 6. 披露与移交

1. **跨记录空间操作姿态维持** -EOPNOTSUPP fail-open（设计稿 §7; 判定表
   逐形对拍: 同 shape 旧形亦拒）。punchfork 迭代末 munmap 的期望随之
   -ENOENT → -EOPNOTSUPP（§4）。
2. **CORTEN_SHRINK_PIN_MAX=4**: 收缩切片成员 pin 上限; 溢出成员下轮
   rotation 再龄（窗口域无桶, legacy 域 anon 桶为个位数段记录, 上限纯
   防御; ponytail 天花板注记在位）。
3. 夹具 3 处裸 `xa_erase`（5114/10285/11242）保留: 洞模拟语义, 单记录
   帧 + ctl_lock。
4. round-3 一过性: `corten_test_txn_uninstall_interlock` 单败（钉 CPU
   kthread 时序型, r02 既有类别, 构建重载窗口; 终轮两跑全绿）。
5. qemu 纪律: kunit VM 每 跑即清（w7kunit pidfile）; 主 VM/A.1 VM 未触。

## 7. 工件索引（results/r07/w7/）

bzImage-w7-final + bzimage-w7-final-sha256.txt（=y 终件, =n 前抢救归档）;
w7-full.diff; build-n-w7.log + build-y-restore-w7.log + config-pre-n-w7.snapshot
+ nm-n-w7.txt + n-stale-objects/; kunit-on1/on2/off.log + w7-kunit.sh;
guest-gate.log（run 2 净单 boot 终轮）+ guest-gate-run1.log + console-w7v2.log
（run 2 全程串口）+ gate-boot*.log + w7-guest-gate.sh + w7-crit-guest.sh
+ w7-n-verify.sh; j3-vc-snap/; 设计稿 next/w7-design.md; 本报告
next/w7-dev-report.md。
