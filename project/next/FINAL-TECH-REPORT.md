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
