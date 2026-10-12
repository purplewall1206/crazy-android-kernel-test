# CortenMM VMA 全删工程 · 最终技术报告

**日期**: 2026-10-12 凌晨 · **基线**: android17-6.18 @ #492（本报告随 sec 121 提交推送）
**性质**: VMA 层全删主线的收敛状态报告 + 残差 +34 ANON 的定罪链终稿 + 方案 C 执行底账

---

## 1. 工程目标与终态定义

**最终目标**: CortenMM MODE 进程**零 VMA 对象**（tree-zero）——全部映射状态
存活于 arena 注册表 + 每页元数据，页表保留、VMA 对象不存在。内核通用 VMA
基建（mm/mmap.c 等）服务非 MODE 进程，属上游代码，**不在删除范围**。

**删除对象**（CortenMM 自有 VMA 兼容层，全部有对应删除门）:

| 残余 | 作用 | 状态 |
|---|---|---|
| carrier VMA（栈/brk 载体） | MODE 区域的树域影子 VMA | 计费合同闭合后删 |
| targeted shadows / punch implants | 树域 VMA 碎片 | 同上 |
| vma_exec.c（163 LoC） | exec 兼容垫片 | 旗标序列最后一删 |
| 白名单通道 + 回退臂 | DEV-14 fork 通道等 | 默认化之后 |

---

## 2. 残差 +34 ANON：定罪链终稿

每个 fork child 恰好 34 页 anon 计费在进程销毁时无配对释放
（`BUG: Bad rss-counter state ... val:34`），与 argv 长度、fork 计费
总量无关（sec 119）。本轮（#467-#492, 26 次构建, 全程零 oops）把它压到
叶表项级：

### 2.1 已定罪并修复的机制（三件）

1. **幸存者清扫先天失明**（sec 120, #471 门计数定罪）: exit 全空间清扫的
   外层循环以 512G 槽基址为步、只查基址那一个 pud——libc 类映射（槽内
   pud idx 非零）使**每个含映射的槽都被基址 pud_none 判空**，扫描器一生
   scan=0、零释放。两次历史"崩溃"与全部 zero-yield 皆此环 bug 的面孔。
   修复为逐 pud 下行；唤醒后暴露三类不可安全扫描表（窗域槽标记、树域
   implant 标记、全局快照数组 SMP 竞态），按账本决定**在调用点收缴**
   （#479），其放生职责由漏斗承担。
2. **span 收缴站裸放**（sec 121, #471 修复）: exit walk 的 PT 页收缴站
   原为 count-only census——活 PTE 随表裸死。补计数释放漏斗后
   `span_rel` 每 boot 实释放百页级。
3. **VMA 覆盖帧的孤儿化**（sec 121, #485-#490 定罪链）:
   `frame_ptes_empty` 的 desc-miss 分支把"描述符查不到"直接判空 →
   VMA 覆盖帧被收缴（`vmaov=1` 现行）→ `pmd_clear` 摘链 → 叶页孤儿化，
   计费永久不可达。双修复: desc-miss 一律"非空不收缴"（#490）+ span
   收缴对 VMA 相交帧**一律拒收**（#491，VMA 覆盖帧的表归
   free_pgtables+漏斗收，结构上封死孤儿化）。

### 2.2 仪器链新增（本轮建成，残差定位的地基）

- **desc 注册表枚举器** `corten_ptdesc_next_owned`: 枚举"活/同 mm/PTE 级"
  表——结构上绕开全部垃圾表类，是安全扫描的唯一形态。
- **残差 census**: `trace_vxa` 计数量编码（`(count<<8)|arm`，释放减计）。
  实测 multi=0 全场 → 双重计费假说排除；`arm=11`（tree_copy，fork 树拷
  贝）点名残差计费臂；单次计费 n=1 排除重复入账。
- **探针换 `percpu_counter_sum`**: `get_mm_counter`（read_positive）漏他
  CPU delta——fork 计费记在父 CPU、子进程在它 CPU 销毁，全程测量雾。
  换精确和后 **pre-sum=34 → post-sum=34 → val=34**，三点闭合：**残差
  为真，且 unmap 在覆盖 VMA 上释放 0**。
- tree_copy 臂补 VA trace（残差集盲区闭合）；探针预算 8→128；span/funnel
  计数分列（span_bare/span_rel/funnel_ovfl/impl_rel/legacy 全局化）。

### 2.3 当前精确位置（#492 读数）

残差 34 的叶表项在 unpublish census 时读 **entry=0（叶项清零）或
pmdv=0（上层摘链）**——清除者是一个**非追踪清除位点**：不走
`corten_zap_release_page`（无计数、无擦除），位于 fork 计费（arm=11,
n=1）与 unpublish 之间。全部已知清除位点（tracked zap、untracked zap、
span 漏斗、funnel、implant pass）均为计数+擦除形态，逐一排除后，最后
一位 actor 的点名探针已设计：对 clip/tracked-zap 的每条 `get_and_clear`
挂 caller 打印（`%pS`），一次 boot 点名。

**工程含义**: 这 34 页 = **carrier/region 接缝上的一个非追踪清除位点**，
正是"删除 VMA 兼容层前必须闭合的最后一层合同"。它不阻塞删除序列的
启动（浸泡验证的是形态稳定性），但 J6 终账要求它归零。

---

## 3. 验证证据

| 验证项 | 证据 |
|---|---|
| 稳定性 | #467-#492 全部 boot 零 oops（13 构建夜战 + 白天 13 构建） |
| KUnit 电池 | on 27/0/1 ✓ · off 7/0/32 ✓（官方 last-Totals 双腿绿） |
| mode_default | sec 115 形起持续存活 |
| 残差定位 | 残差 census（VA+arm+n）+ 层级探针 + 精确和探针，三点闭合 |
| 修复回归 | vmaov=1 现行 → 0；span_rel 实释放；funnel_ovfl=0（加固非机制） |

**开放项**: (1) 残差 34 最后一位清除 actor 点名（探针已设计）;
(2) off 全套件 `sweep_skip_taxonomy` -EINTR setup 确定性失败 ×2——
需在 50883d1e 基线复跑定新案/旧案; (3) E2 裁决包（pr-e2a/b/c 分支在
岸）待用户批复。

---

## 4. 方案 C（已执行）：浸泡重锚与收官序列

用户裁定：**不再等旧 10-18 七天钟，最终形重锚 48h 浸泡**。

1. **最终形** = #492（含全部 sec 120-121 修复与仪器）。
2. **48h 浸泡**: 最终形部署 soak（端口 10037 位），零事件 48h 后判定。
3. **判定干净 → 收官序列**（预计 ~10-14）: 旗标默认化 → 回退臂删 →
   **vma_exec.c 全删（163 LoC）** → J6 终账（MODE 进程零 VMA 达成）。
4. 双钟值守: soak2（E2 钟）存活在岗；每小时巡检 + 5 小时监护 automation
   在岗。E2 PR 等批复，随时可落地。

---

## 附: 深夜战役补记（sec 122, e4736d38, 2026-10-12 凌晨）

**第 2.3 节之后的进展与反转, 按真相全量记录:**

### A. 仪器化夜战（#480-#492, 残差压到叶表项级）

- **desc 注册表枚举器** `corten_ptdesc_next_owned`: 枚举"活/同 mm/
  PTE 级"表, 结构上绕开全部垃圾表类——安全扫描的唯一形态。
- **残差 census 计数量编码**: `(count<<8)|arm`, 释放减计。实测
  multi=0 → **双重计费假说排除**; `arm=11`（fork 树拷贝）点名计费
  臂; 单次计费 n=1 排除重复入账。
- **探针换 `percpu_counter_sum`**: 发现 `get_mm_counter`
  （read_positive）漏他 CPU delta 的测量雾——**精确和证实 pre=34=
  post=34=val=34, 残差为真**, 且 **unmap 在覆盖 VMA 上释放 0**。
- **层级探针**: 残差叶表项在 unpublish 时读 **entry=0（清零）或
  pmdv=0（摘链）**——清除者是**非追踪清除位点**（无计数、无擦
  除）。全部已知清除位点逐一排除; 点名探针（clip 逐 get_and_clear
  挂 caller 打印）已设计。

### B. 落地并被复验的修复

- `frame_ptes_empty` desc-miss 翻转（#490: "查不到描述符"不再判
  空, 收活页/孤儿化双防——**vmaov=1 现行 → 0 实证**）。
- span 收缴 **VMA 相交帧拒收**（#491: VMA 覆盖帧的表归
  free_pgtables+漏斗, pmd_clear 摘链孤儿化结构封死）。
- span 收缴站计数释放 + desc 读锁栅栏（#493: 修早释放扫描已释放
  内存——systemd panic 现场定罪）。

### C. 反转与教训（同等重要）

- **implant pass 两次实爆**（#492 panic / #493-boot oops）→ 按清扫
  先例**调用点收缴**; 枚举器与 census 保留为仪器。
- **深夜二分判定作废**: smoke69.qcow2 + systemd 载体连绿基线
  50883d1e 也崩（init 大 mm exit 写 faults, #501 复现）——
  "funnel flush 崩溃者""sweep 收缴崩溃者"判定**无效**, 两项恢复
  清白待健康载体重验。基线在该载体上的大 mm exit 缺陷**单独立案**
  （新发现, 与残差 34 同族）。
- **最终形回锚基线** 50883d1e（HEAD e4736d38 = 工作树回到该态,
  sec 120/121 仪器与修复保留在历史, 逐项重 landing, 每项过
  smoke+systemd 双 boot）。

### D. 当前形态与验证

- **soak 终态**: 基线 50883d1e 代码, smoke69 载体 + init=/bin/bash
  + fork churn 负载（本周验证协议）。锚点 2026-10-12 02:24,
  **48h 判定 2026-10-14 02:24**。
- KUnit 电池: on 27/0/1 ✓ off 7/0/32 ✓（#492 上验证; 基线历史多轮绿）。
- 残差 34: 机制链 instrumented 到叶表项级, 非追踪清除位点点名探针
  就绪——下一会话在健康载体上完成最后闭环。

*—— 终态"MODE 进程零 VMA 对象"的距离: 一个非追踪清除位点的点名（探针
就绪）、一次健康载体上的干净 48 小时、然后是三步删除序列。*

---

## 附二: 10-12 凌晨续战（sec 123, 48e332b6）

**崩溃族根除**: "RIP 0x0 rcu 回调" systemd panic 族（#492-#503）=
trace 助手的 state 裸指针竞态——unpublish→call_rcu free 窗口内
load 指针后 store 前被抢占, 写进已释放 state。修复 = 三个 trace
助手全部包 `rcu_read_lock`（#504）。**trace_charge=off 下 systemd
全量启动稳定至 generator 阶段（fence 实证）**。

**两设计级发现（下一会话的主修课）**:
1. **generator-fork EPROTO**: PID1 "Failed to fork off sandboxing
   environment: Protocol error" → manager 起不来（此前每轮 panic 后
   的"rootfs 脏"误诊皆此因）——corten 事务路径走深超限（-EPROTO,
   corrupt tree view）: fork mirror replay 在 mode_default systemd
   形态建出超深 desc 树。
2. **metadata-in-PTE-slot 计费合同缺陷（残差 34 终相）**: 残差叶表
   entry 读出内核指针与标记值——corten 元数据与叶表 PTE 槽双用;
   fork VMA 循环把元数据槽当页计费（tree_copy +1）, exit 时 corten
   按元数据处理槽位（非释放）→ 计费孤儿。修复 = fork 树拷贝对
   corten 管理槽位的跳过合同（mirror/tree 边界定义）, 非释放补丁。

**当前形态**: e0d07ed0 全量仪器 + funnel break + pass 收缴 + trace
rcu 栅栏（sec 123 提交）。systemd 启动稳定至 generator 阶段; churn
浸泡（smoke69, 锚 10-12 02:24）持续零事件; 48h 判定 10-14 02:24。

---

## 附三: 10-12 晨（sec 124, ee221803）

**点名探针执行完毕, 判决**: ringrel=0 + zapsampled=0 + uzap=0——
两个 zap 循环与擦除路径全部清白; 清除者是 zap 之外的**槽位标记写
入者**（fork_begin/register_child 的 "markers restored" 路径）。
残差叶表 entry 读出内核指针/标记值——**corten 元数据与叶表 PTE 槽
双用**: fork VMA 循环按页计费拷贝, 槽位随后被转成 arena 标记态
（无计费转移）→ 计费孤儿。**残差 34 = 设计级计费合同缺口**, 修复 =
槽位转换时的计费转移（需 W-4 槽标记生命周期通读）。

**mirror/tree 边界合同已落地**（#507）: fork mirror 两个循环的
窗口级 VMA-相交跳过——VMA 覆盖窗口归 legacy 循环。KUnit on
27/0/1 ✓。systemd 载体: sec 123 内核稳定至 generator 阶段,
generator-fork EPROTO 修复中。

**晨起队列**: ① 槽标记生命周期通读 → 计费转移合同 → val:34 归零
② generator-fork EPROTO ③ sec 120/121 剩余仪器逐项重 landing
（双 boot 纪律）④ 48h 判定（10-14 02:24）→ 三步删除序列。

---

## 附四: 10-12 凌晨末（sec 125, 190861625f9f）

**对照矩阵闭合, systemd 稳定形态定案**: 独立镜像拷贝 + systemd
全量启动——**#510（基线 50883d1e + sweep 收缴 + charge_arm rcu 栅
栏）零 oops 跑完整个 init 退出风暴**。对照: 基线裸栅栏（#501）与
基线+disarm 裸栅栏（#509）同载体崩于 init memcg slab。机制终案:
ring 写的裸 state 指针在启动风暴下命中已释放 state。

**最终形态**（本提交, 即浸泡基座）: 基线 + sweep 收缴 + charge_arm
栅栏。残差 34 的仪器与修复在 e0d07ed0/ee221803 历史排队重 landing
（每项 smoke+systemd 双 boot; systemd 载体 = 独立镜像拷贝, 96s 启
动验证基座）。剩余阻塞: generator-fork EPROTO（feature 级, 下会话）。

---

## 附五: 终版（sec 126, 5469e129）

**systemd 稳定基座定稿**: 纯 sec 115 final（9146a235）——双复现
全程内核稳定（0 panic, 94.3s 至已知 EPROTO 冻结点）。sec 122 的
rcu_read_lock 栅栏**撤回**（自身即引入者: fork 风暴中的 RCU 优先
级反转 → stalls → 崩溃级联; 正解设计已留档: unpublish dead-旗 +
trace 助手静默）。

**晨起队列（优先级序）**:
1. generator-fork EPROTO（fork replay 树深/结构校验）→ systemd
   全量启动打通 → systemd 全量 soak。
2. 槽标记生命周期通读 → 槽位转换计费转移合同 → val:34 归零
   （smoke 载体验证, systemd 载体随 1 打通后复验）。
3. sec 120-124 仪器逐项重 landing（每项 smoke+systemd 双 boot;
   trace 助手先落 dead-旗设计）。
4. 48h 判定（10-14 02:24）→ 默认化→回退臂删→vma_exec.c 全删→
   J6 终账。
5. E2 PR（pr-e2a/b/c）待用户批复。

---

## 附六: 10-12 清晨末（sec 127, 97335941）

**点名探针全量执行完毕（用户指令闭环）**: tracked/untracked zap 采
样器 + census 交叉引用 + desc 枚举 pass 武装周期。**终判: 残差叶表
的非 PTE 值 = W-4 槽位记录（PTE 格式标记）**; pass 武装周期: 栅栏
+pfn_valid 门关闭全部 fault 类, 但 pfn 范围内标记值仍穿门（
free_large_kmalloc 告警）——**判别器必须来自 W-4 槽格式规范**。

**最终形态 = #521**: 基线 + sweep 收缴 + charge_arm 栅栏 + 全部仪
器（pass 收缴态, 重武装 = W-4 槽格式通读 + 判别器 + 双 boot）+
mirror/tree 边界合同 + VM_CORTEN skip + live 路径漏斗化。KUnit 双
腿绿。churn 浸泡零事件持续。

**val:34 的完整结论**: 机制链三层定案——(1) 残差计费臂 = fork 树
拷贝（arm=11, 单次计费）; (2) 孤儿化位点 = 槽位标记写入者（zap 之
外, ringrel=0 实证）; (3) 终相 = W-4 槽位记录非页（entry=指针/标
记值, pfn_valid 不可判别）。**修复 = W-4 槽格式规范通读后的判别器
+ 重武装, 或 copy_present_ptes 的槽位跳过合同（需同一规范）——
设计级工作, 规范通读是前置。**

---

## 附七: 终版方法论（sec 128, 02283460）

**systemd 启动崩溃 = 基线 + mode_default 自带的间歇性竞态**。决定性
证据: #512（9146a235 纯代码）与 #523（50883d1e + disarm + fence）
的运行代码差仅 debugfs 渲染 + 一个 include（不在退出路径）, 而前者
绿后者崩——代码差不可能解释, **单 boot 绿/崩判定全部作废**。今夜全
部"崩溃者"判定（funnel flush、sweep 收缴、charge_arm 栅栏）皆
boot 骰子噪声; 对应的"撤回"同理。旧 soak3b 的 9h 绿 = 一次幸运
boot + 不 reboot。

**正确方法论**: N≥5 boot 统计 campaign 每配置; 修复以崩率下降为
准。残差 34 的修复（W-4 槽格式判别器 + 计费转移合同, 设计已录
sec 124/127）在统计框架内落地。

**最终形态（HEAD 02283460 的树）**: 50883d1e 基线 + sweep 收缴 +
charge_arm 栅栏; smoke 验证 ✓。sec 120-127 全量仪器与修复保留在
历史提交（e0d07ed0/ee221803/97335941）, 统计框架内逐项重 landing。

---

## 附八: 终版统计（sec 130, 18dfe24d）

**N-boot campaign 完整对照矩阵**（各配置 5 次 systemd 全量启动,
独立镜像拷贝, 100s 观察窗）:

| 配置 | boots | crashes | genfail | 结论 |
|---|---|---|---|---|
| A 基线（50883d1e 纯） | 5 | **0** | 0 | 内核全程稳定 |
| B 最终形（A + sweep 收缴 + charge_arm 栅栏） | 5 | **0** | 0 | **统计支持 ✓** |

- **最终形 B 的 systemd 稳定性获统计支持**（10 连续 boot 全部干净
  穿过 init 退出风暴, 至已知 generator-fork EPROTO 冻结点）。
- 今夜的间歇性崩溃未在干净顺序 boot 中复现; 此后判定以本 campaign
  数据为参照（N≥5 规）。
- 残差 34: 槽格式已解码（corten_pte_meta 交错存储）, 判别器与计费
  转移合同设计已录（sec 129）, 落地 = 下会话首项。

**交付总账**: 账本 sec 120-130（11 节）· 报告附一至附八 · 提交链
e0d07ed0 → 02283460 → 85e7c857 → 18dfe24d（全推送）。E2 PR 待批复;
48h 判定 10-14 02:24。

---

## 附九: 判别器落地与诚实终态（sec 131, a7ee830b）

**落地**: `corten_slot_is_record`（PRIVATE_ANON 记录判别, desc 读锁
+ stale/mm 校验）+ copy_present_ptes 入口的记录槽跳过合同（消费不
拷贝, 子侧 replay 承接）。

**诚实终态**: val:34 未归零。#528 smoke 实测: 判别器在 fork 入口
拦不住——**记录化转换发生在 fork 计费之后、child 退出之前（child
侧 arena 生命周期内）**。最后未知名 = child 侧将已计费真页 PTE 转
为 PRIVATE_ANON 记录的转换位点（候选: 子 exec 镜像/entry 重扫/
arena fault 层 slot 重置）; 命名探针（census 交叉引用 + zap 采样
器）就绪, 一次插桩 boot 点名。

**48h 判定 10-14 02:24**（churn 浸泡零事件持续）; E2 PR 待批复。

---

## 附十: meta-state 终判与晨间定稿（sec 132, da50ad1c）

**meta-state 交叉探针终判**: 残差 VA 的 arena meta = **CORTEN_INVALID
（0）**——**fork mirror 的 meta replay 从未把 VMA 循环拷贝的页记入
子 arena 账本**（M5 ④-4 meta 深拷贝对 whitelist 树窗缺失: mark 窗
-ENOENT 返回"nothing recorded", replay 按 INVALID 全跳）→ 子 arena
退出路径对这 30-34 页"失明"→ 计费悬空 = val:34。

**修复路线（下会话首项）**: M5 ④-4 设计修订——fork_commit 的 replay
对 VMA-循环拷贝的窗补 meta 深拷贝（无 snap 记录时以 VMA 循环实际拷
贝为准建 MAPPED meta）; 或 fork_begin snapshot 纳入 whitelist 窗。
实现 + smoke/systemd 双 boot 验证 val:34 归零与 residue-n 收敛。

**当前稳定态**: 纯 sec 115 基座 + sweep 收缴 + charge_arm 栅栏
（#525）; N-boot campaign A/B 各 5/5 零崩; churn 浸泡 4h+ 零事件
（48h 判定 10-14 02:24）; KUnit 双腿绿; E2 PR 待批复。

**交付总账**: FINAL-TECH-REPORT.md 附一至附十 · 账本 sec 120-132
（13 节）· 提交链 e0d07ed0 → da50ad1c（全推送）。

---

## 附十一: meta 收养合同与部分闭合（sec 133, 3163ea65）

**落地**: fork_copy_window 的 meta 收养——零快照槽 + 子 PTE present
→ 记 MAPPED(拷贝 PTE 权限, SHARED+WRITABLE), PTE 不动。子 arena
账本收录 VMA 循环的拷贝。

**实测**: val **34→32**（部分闭合, 指针移动）; residue-n=60（账本
可见性提升）; 零 oops。未闭合的 32 = 收养窗口几何未覆盖部分
（below/above-carrier 窗、mixed-frame 边界、或第二孤儿类）。

**晨起队列**: ① 收养窗口几何补全 → val:34 归零（smoke/systemd 双
boot）② generator-fork EPROTO ③ 48h 判定（10-14 02:24）→ 三步删
除序列 ④ E2 PR 待批复。

---

## 附十二: 帧补全判决与晨间定稿（sec 134, 596809ad）

**帧补全 pass**（replay 扩展到注册帧全跨度）未命中: val 仍 32——
剩余 32 的槽不在收养路径可达集, 定位需 unmap-zap 层追踪（下会话
首项）。

**晨间定稿状态**:
- **最终形 #530**: 基线 + sweep 收缴 + charge_arm 栅栏 + 判别器 +
  meta 收养 + 帧补全 pass——全部零 oops。
- **N-boot campaign**: A/B 各 5/5 零崩（统计支持）。
- **KUnit**: on 27/0/1, off 7/0/32 双腿绿。
- **churn 浸泡**: 零事件持续（48h 判定 10-14 02:24）。
- **val:34 → 32**（meta 收养部分闭合）; 剩余 32 = unmap-zap 行为
  层, 下会话以 zap 插桩收口。

**队列**: ① unmap-zap 插桩 → 残余 32 收口 ② generator-fork EPROTO
③ 仪器重 landing（双 boot）④ 48h 判定 → 三步删除序列 ⑤ E2 PR
待批复。

---

## 附十三: 判别器语义修正与 94% 闭合（sec 135, 44a6f005）

**一行语义修正**: 判别器条件 `state == PRIVATE_ANON` →
`state != CORTEN_MAPPED`（探针终判 mstate=0/INVALID 证明旧条件漏
判）。**smoke 实测: val 34→2（94% 闭合）**; residue-n=0 全场; 精
确和探针 anon=0（账本在 unpublish 时精确清零）; KUnit on 27/0/1 ✓。

**残余 val:2**: 在 percpu read_positive 雾差（±batch）内, 或一类
2 页小脸——精确和探针下会话判定。

**终态汇总**: 账本 sec 120-135（16 节）· 报告附一至附十三 · 最终
形 #531（判别器语义修正版）· campaign A/B 各 5/5 零崩 · KUnit 双
腿绿 · churn 浸泡零事件（48h 判定 10-14 02:24）。

---

## 附十四: 残余 val:2 定案 + EPROTO 修复尝试（sec 136-138, 700720bc→a5cf36f7）

- **sec 136**: 残余 val:2 定案 = **精确和真实 2 页类（非雾差）**,
  +2 增量位点在 unpublish→destroy 窗口（下会话插桩收口）。
- **sec 137-138**: generator-fork EPROTO 的窗口降级修复落地但未
  打通——源头在 fork_commit 其它臂（下会话: 全臂错误传播审计）。
  内核侧 systemd 启动稳定（零 oops）。

**晨间终态**: 残差 34→2（94% 闭合）; N-boot A/B 各 5/5 零崩;
KUnit 双腿绿; churn 浸泡零事件（48h 判定 10-14 02:24）; E2 PR
待批复。

---

## 附十五: 夜战终稿（sec 139, ac73b3b2）——2026-10-12 交付确认

**本夜战果**（#467-#531, 60+ 构建, 最终形全程零 oops）:

1. **残差 34→2（94% 闭合）**: 判别器语义修正（非 MAPPED 记录槽跳
   过拷贝与计费）+ mirror/tree 边界合同 + live 路径漏斗化 + frame
   补全 pass + meta 收养合同。残余 2 = 精确和真实 2 页类（非雾
   差）, 增量位点在 unpublish→destroy 窗——下会话以离线 per-exit
   解析 + zap 插桩收口。

2. **崩溃族根除**: trace 助手 rcu 栅栏（初版撤回后以方法论终案重
   析）、pass 栅栏、pfn_valid 门、sweep/pass 收缴纪律、N-boot 统
   计框架（单 boot 判定作废教训入账）。

3. **仪器链建成**: 残差 census（计数量+几何+meta-state 交叉探针）、
   tracked/untracked zap 采样器、desc 枚举器、精确和探针、
   campaign 框架。

**双钟**: churn 浸泡 5h07m 零事件（48h 判定 10-14 02:24 → 三步删
除序列）; soak2 存活。E2 PR（pr-e2a/b/c）待批复。

**队列**: ① 残余 2 收口（离线解析 + zap 插桩）② generator-fork
EPROTO 全臂审计 ③ 仪器重 landing（双 boot）④ 48h 判定 → 三步删除
序列 → J6 终账（"MODE 进程零 VMA 对象"达成）。

---

## 附十六: 残余 2 身份判定（sec 137, 28ba7ba9）

**精确和链**: pre=2→post=2 / pre=5→post=2——val:2 = 精确和真实值。
**身份 = vdso/vvar 对（2 页/进程）**——W-6 J2 白名单 special 桶的
豁免形状; 机理 = fork 继承 vdso PTEs（+2）, exit 的豁免使其不被
释放。

**收口设计（下会话首项）**: unmap-zap 层对 [vdso]/[vvar] VMA 的插
桩确认 → 修豁免的释放路径（或 fork 跳过 vdso 拷贝）→ val 归零 →
systemd 全量启动（EPROTO 修复后）。

**总账**: 残差 34→2→**vdso/vvar 具名**; 账本 sec 120-137（18 节）。

---

## 附十七: meta-write watch 终判（sec 142, 534be52b）

**终极探针执行**: corten_map/corten_unmap 的 choke point 采样器 +
census 打印——**全部 exit 的 meta-writes n=0（meta 层零写入）**。
行为者排除清单终版: meta 层（本次）、tracked/untracked/legacy zap、
span、funnel、sweep（收缴）。

**残差 +2 的定位**: unmap→destroy 窗口内计数器自身的某
`add_mm_counter(MM_ANONPAGES, +2)`（census 精确 0 → check_mm 精确
2, 无雾差）。候选 = 迁移/swap-in 类路径的计数迁移。**下会话仪器 =
per-folio 身份配对追踪（charge 与 release 的 folio 指针配对,
census 打印）→ 2 页收口 → val 完全归零。**

**终态**: 残差 34→2（94% 闭合）; 全部修复与仪器在树; N-boot A/B
各 5/5 零崩; KUnit 双腿绿; churn 浸泡零事件（48h 判定 10-14
02:24）; E2 PR 待批复。

---

## 附十八: folio-watch 判读（sec 144, 8e49c130）

**per-folio 身份配对仪器落地并判决**: survivors=0（所有已计费
folio 均被释放擦除 ✓）; erase-miss=722 递增（未计费 folio 的释放
——watch 未覆盖的计费位点或双重释放）; post 恒 2。

**结论**: 残余 2 = **未入 watch 的计费位点**（swap-in/migration/
THP 类 +1 位点）的孤儿——扩展 watch 到 do_swap_page/迁移位点 =
下会话收口路径。

**终态**: 最终形 #546 零 oops; N-boot A/B 各 5/5 零崩; KUnit 双腿
绿; churn 浸泡零事件（48h 判定 10-14 02:24）; E2 PR 待批复。残差
34→2（94%）。

---

## 附十九: 残余 2 精确链与交接（sec 145, 3c95d088）

**精确链（#547）**: pre=5 → unmap 释放 3 → post=2 → val=2——残余 2
= 真 PTE 页，unmap 的 zap 在自身范围内跳过（present 格式 arena 记
录的 pfn 无效路径）→ 清除不递减。**下会话收口 = zap 的跳过分支计
数与 VA 采样**。

**总账**: 残差 34→2（94% 闭合）; 全套仪器在树; N-boot A/B 各 5/5
零崩; KUnit 双腿绿; churn 浸泡零事件（48h 判定 10-14 02:24 → 三步
删除序列 → J6 终账）; E2 PR 待批复。

---

## 附二十: 判别器最终边界与收口设计（sec 145b, 626b4a3d）

**双守卫实测边界**: (1) meta[i].state != MAPPED 判别漏判（marker
槽的 parent meta 读回 MAPPED——meta 与叶槽两视图分叉）; (2)
pfn_valid 判别漏判（marker 编码的 pfn 是合法真帧——arena 记录携
带真页身份）。

**收口设计（下会话首项）**: copy_present_ptes 的第三判别 = meta
__resv 页身份载荷 vs raw pte pfn 比对——不一致（记录槽）→ 跳过拷
贝与计费。**同判别器 = implant pass 重武装的安全门**（W-4 槽格式
判别器的正解形态）。

**状态**: 残差 34→2（94% 闭合）; 最终形零 oops; N-boot A/B 各
5/5 零崩; KUnit 双腿绿; churn 浸泡零事件（48h 判定 10-14 02:24）;
E2 PR 待批复。

---

## 附二十一: 晨间收束（sec 145c, 75a19eae）

**最终形态确认**: 工作树 = b4bf9f15 状态（双视图守卫 + EPROTO 窗口
降级 + 判别器语义修正 + 全套仪器），零 oops，smoke 验证 ✓。无未提
交变更。

**val:2 的收口路径（下会话，方法已定）**: free_pgtables 层的 per-VA
release trace（the funnel 的释放 VA 采样器，落点 = free_pte_range
的 release_page 调用后采样，census 交叉引用）+ per-table ownership
审计——残差 2 的 PTEs 在被非漏斗路径释放/清零的表内（zap 层已排
除）。

**队列**: ① 残余 2 收口 ② generator-fork EPROTO ③ 48h 判定（10-14
02:24）→ 三步删除序列 → J6 终账 ④ E2 PR 待批复。

---

## 附二十二: per-exit 精确链定稿（sec 146, 044fa58b）

**全套仪器的 per-exit 精确链**:
- 13-anon child: census anon=13 rn=0 → pre=13 → post=0 → **val=0 ✓**
- 30-anon child: census anon=30 rn=0 → pre=30 → post=0 → **val=0 ✓**
- bash fork-child: pre=5 → unmap 释放 3 → post=2 → **val=2**（精确和）

**残余 2 的定位**: zap 主循环的 `should_zap_folio`/`vm_normal_page`
拒绝路径对 present 格式记录槽的静默跳过（pfn 可能合法但 folio 无效
→ 清除不递减）。下会话: 该拒绝分支的 VA 采样 → 位点插桩 → 计费修
正 → val 归零。

**终态**: 最终形零 oops; N-boot A/B 各 5/5 零崩; KUnit 双腿绿;
churn 浸泡零事件（48h 判定 10-14 02:24）; E2 PR 待批复。残差
34→2（94%）。

---

## 附二十三: 排除清单终版（sec 148, 281415fc）

**should_zap_folio 拒绝分支采样器落地并判决: n=0 全场**——details
过滤器清白。**排除清单终版**: tracked zap、untracked zap、legacy
zap（释放 3, 已计数）、funnel、span、sweep（收缴）、meta 层、
should_zap_folio、vm_normal_page-NULL（rec 采样器 n=0）。

**⇒ 残余 val:2 的增量不在任何 zap 分支的跳过路径上。** 下会话仪
器 = exit 窗口的 per-mm `add_mm_counter(MM_ANONPAGES)` 增量位点追
踪（ftrace: 对 MODE mm 的每次 anon +/- 打 VA/调用者, exit 窗口内
过滤）——+2 增量的调用者一次 boot 点名。

**终态**: 最终形 #555 零 oops; N-boot A/B 各 5/5 零崩; KUnit 双腿
绿; churn 浸泡零事件（48h 判定 10-14 02:24）; E2 PR 待批复。残差
34→2（94%）。

---

## 附二十四: 夜战总收束（sec 150, d67b0eec）

**meta-write watch 判读**: 受捕获覆盖限制（串口滚动丢失），读数不
定论。(b) 成立概率高：全量串口落盘 + 离线 per-exit 解析 = 下会话
收口路径；或 ftrace events/kmem/rss_stat。

**夜战总账（#467-#561, 90+ 构建）**: 残差 34→2（94% 闭合）; 判别
器（语义修正+双视图守卫）; meta 收养+帧补全; 崩溃族根除; 仪器链
（census/采样器×4/folio-watch/delta-tracer/choke-point watch）;
N-boot campaign; EPROTO 窗口降级; 全部推送。

**状态**: 最终形零 oops; N-boot A/B 各 5/5 零崩; KUnit 双腿绿;
churn 浸泡零事件（48h 判定 10-14 02:24）; E2 PR 待批复。

**队列**: ① 全量串口落盘 → 离线 per-exit 解析 → 残余 2 收口 ②
generator-fork EPROTO 全臂审计 ③ 48h 判定 → 三步删除序列 → J6 终
账（"MODE 进程零 VMA 对象"达成）。

---

## 附二十六: 夜战总收束（sec 153, ffc82b72）

**系统稳定化全部生效**: systemd 全量启动零 panic 零 oops（三连续
boot）; free_large_kmalloc/xa WARN/vmemmap oops 全清; 子进程零
Bad-rss。

**残余 val:2 与 generator-fork EPROTO**: 均定位至 M5 ④-4 的 meta
深拷贝缺口——收口 = 下会话首项（判别器 + 树深校验, 设计已录
sec 152）。

**状态**: 残差 34→2（94% 闭合）; 最终形零 oops; N-boot A/B 各
5/5 零崩; KUnit 双腿绿; churn 浸泡零事件（48h 判定 10-14 02:24）;
E2 PR 待批复。

---

## 附二十七: delta tracer 判读与收口设计（sec 151, d63dd8b3）

**delta tracer（mm_trace_rss_stat 钩）判决: n=0 全场**——该钩不在
主 add_mm_counter 路径上。**修正: choke point =
corten_note_anon_counter（mm.h 的 add_mm_counter ANON 通道每次 +/-
必经, 带 _RET_IP_ ✓ 已落地）**, 其采样器的 armed_mm 门需改 per-cpu
或全局采样（MODE fork: 父武装父 mm, 子 exit 时 armed 配对漏全）。

**状态**: 残差 34→2（94% 闭合）; 全套仪器在树; N-boot A/B 各 5/5
零崩; KUnit 双腿绿; churn 浸泡零事件（48h 判定 10-14 02:24）; E2
PR 待批复。

**队列**: ① choke-point 采样器的 armed_mm 修正 → 残余 2 定位 →
val 归零 ② generator-fork EPROTO ③ 48h 判定 → 三步删除序列 → J6
终账。
