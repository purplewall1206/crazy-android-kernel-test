# W1.b 开发报告 — per-inode corten region 注册表 + 失效枚举翻转（MV2 W1.b）

日期: 2026-09-22 · 树: `/home/ppw/linux-6.18-mva`（分支 mv-a0 @ 3cd07f2351f4，未 commit，工作区即交付物）
补丁: `/home/ppw/cortenmm/patches/r07-w1b.diff` · 日志: `/home/ppw/cortenmm/results/r07/w1b/`
规格: `specs/W1_NATIVE_RMAP_SPEC.md` §4.1/§4.2、§3.1 R11/R12、§5 W1.b 行、R-W1-2

---

## 1. 落了什么

### 1.1 per-inode 注册表（规格 §4.1，R11 翻转）

- **`struct corten_inode_regions`**（include/linux/corten_arena.h）：`regions` 链表头 +
  `nr` 计数。全局索引 = `static DEFINE_XARRAY(corten_inode_regions)`，key 为
  `(unsigned long)mapping`。零核心结构侵入（`struct address_space` 未动）。
- **region 侧**：`struct corten_arena` 增 `rfile_node`（链表节点，内嵌零分配）与
  `rinodes`（所属 head 指针，兼任 attach 期的预分配载体）。
- **attach 挂入**：`corten_file_i_mmap_insert()` → `corten_file_registry_insert()`。
  调用点不变（declare 末步 ：1984、fork 子侧 ：5908），仍是 "past every failure path"
  的最后发布步。关键工程点：**head 在 FILE arm 可回退窗口预分配**
  （`corten_inode_regions_new()`，declare 在 carrier_alloc 之前、fork 在 register_file
  之前），使最后的 link 步**零分配、不可失败**——原 i_mmap insert 不可失败的发布契约
  在注册表形状下由预分配保住；attach 失败经 `corten_region_file_disarm()` 归还 head，
  不留空壳条目。首 region 经 `xa_cmpxchg(key, NULL, ours)` 发布（无分配、败者 loud，
  实际不可达：全部写者持同一 i_mmap_write）。
- **detach 摘除**：`corten_file_i_mmap_remove()` → `corten_file_registry_remove()`。
  调用点不变（`corten_region_file_teardown()`，即 corten_arena_free / pool_park 两收敛
  点），与 insert 同锁窗口对称；`nr==0` 时 xa_erase + kfree（写锁临界区内摘除索引，
  读侧被 rwsem 互斥，出临界区后释放即安全——interval-tree 节点同款生存期纪律）。
- **锁序（与既有 attach 合并论证，F8 边序原样）**：
  `mmap_lock(W) > i_mmap_rwsem(W) > [xa_lock]`；xa_lock 只在 i_mmap_write 临界区内被
  xa_store/xa_erase/xa_cmpxchg 内部获取，单向边 `i_mmap > xa_lock`，无反向路径，无环
  （INV2 / DEV-13 不变，零新锁类）。i_mmap 临界区内无 descriptor 工作，与 B.2 论证逐字
  同构。锁的**读写语义与 interval-tree 完全同位**：attach/teardown 持写、枚举持读。
- 文件引用账本不变：region 持 rfile 引用 ⇒ 注册表非空 ⇒ mapping 必活（head 的生存期
  被 file 引用钉死，无悬空 key）。

### 1.2 失效枚举翻转（规格 §4.2，R12 翻转）

- **`corten_arena_unmap_file_range(mapping, first, last, even_cows)`**：由
  `unmap_mapping_pages()` 与 `unmap_mapping_folio()` 在各自 `i_mmap_lock_read()` 段内
  调用（mm/memory.c，`corten_enabled_static()` 门控）。xa 空探针快门：无 corten region
  的 mapping（整个 legacy 世界）一条 xarray probe 返回。
- 逐 region O(1) 推导交叠（walker 的 zba/zea 算术反演）：
  `zba = max(first, rpoff)`、`zea = min(last, rpoff+npages-1)`，VA = `start + (zba-rpoff)<<PAGE_SHIFT`；
  复杂度 O(#region/inode)（dlopen 场景个位数）。命中 region 走 **B.2 事务体原样**：
  `corten_arena_unmap_chunk_flags(KEEP_PERM | (!even_cows ? FILE_EVENT : 0))`，
  `truncate_routes` 计数点原样（成功事务 +1/region）。冻结/park/`tryget_live` 失败的
  region 按既有门语义跳过（自己的 teardown 拥有内容丢弃）。unmap_mapping_folio 的
  单页形状由 `folio->index` 推导，与旧 gate 一致不复查 `single_folio`（推导地址即该
  folio 偏移；语义保真注记在函数头）。
- **锁序声明不变**：`i_mmap_rwsem(read) > desc->lock(W) > ptl`，chunk 驱动可在读段内
  睡眠（tlb flush）——与 B.2 gate 当时完全一致，唯一新增边仍是 `i_mmap_read > desc`。
- **生存期（旧 i_mmap 成员 pin 的移植）**：枚举持 i_mmap_read ⇒ teardown/park 必须
  持写才能摘链 ⇒ 走查永不触碰死描述符；`tryget_live` 延伸 pin 过读段
  （[FAIL-2] 纪律）。
- **INV6**：零新增 PTE 写点——枚举只是给既有 B.2 chunk 事务换枚举源；两个降级 backstop
  均为**拒绝**语义（下）。

### 1.3 旧 gate 降级永零断言

- **vma-keyed route gate 摘除**：`corten_arena_unmap_file_event()` 删除（~65 行含注释），
  替换为 **`corten_arena_imap_stale_guard(vma)`**：翻转后 carrier 不再是 i_mmap 成员，
  walk 产出 VM_CORTEN VMA = teardown 漏摘的 stale 节点（R-W1-2 的精确形状经唯一未封
  的门到达）→ WARN_ONCE + `corten_nr_imap_stale_refuses` 计数（新计数器，恒零断言）+
  **拒绝** legacy zap（INV6 不因降级失守）。memory.c 侧 `unmap_mapping_range_vma` 只剩
  该断言（-45/+32 行）。
- **zap_single backstop 不动**（本就是断言形状），翻转后与 imap_stale_refuses 一同
  进入"恒零"断言组；debugfs 渲染加 `imap_stale_refuses` 行（S8）。
- 语义边序注记：旧 gate 的 `details ? details->even_cows : true` 折叠随之消失——枚举
  直接消费 `unmap_mapping_pages` 的 `even_cows` 参数与 folio 形状的 `false`，无行为差。

### 1.4 顺手修复（编辑面内的潜伏 bug）

`corten_region_file_disarm()` 原尾序 `ar->rfile = NULL; …; fput(ar->rfile)` =
**fput(NULL)**（任何 FILE attach/fork 的发布期 unwind 必炸；此前测试只 force 了 ANON
臂的 unwind 故未暴露）。改为局部持有后 fput；该函数同时补 `kfree(ar->rinodes)`
（W1.b 的 head 归还路径）。

### 1.5 边界与红线

- **未动**：W1.a 的四个 novma wrapper（rmap.c 零改动）、V-C 渲染主体、fork 复制路径、
  rmap.c:1895 M6 守卫（file 臂随 i_mmap 摘除后对窗口页不可达——W1.d 的 file 真路由
  接管；过渡态安全性行：窗口 file PTE 的 mapcount 继续钉住 pagecache folio，回收/
  迁移对该 folio 干净失败，无 UAF/无泄漏）。
- **=n 折叠**：`corten_arena_unmap_file_range` 的 no-op stub + 调用点 `corten_enabled_static()`
  static-key 双保险；注册表实体只在 mm/corten_arena.c（=n 不编译）。十四对象门实测
  零符号（下表）。
- **范围核对**：task 四锚全覆盖（§3）；规格 W1.b 行的 KUnit ③"并发 attach/teardown
  lockdep 用例"未单列成用例（task 范围未列；rwsem 写/读互斥与对称锚已由
  lifecycle/inval 两用例覆盖串行正确性，并发矩阵留主会话 guest 门或 W1.d 补）。

## 2. KUnit 锚（corten_arena 101 = 100+1）

| 锚（task §4） | 落点 |
|---|---|
| 注册/摘除对称 | `file_lifecycle`（attach=1 → park=0 → 全局 index empty）+ `file_fork_mirror`（fork=2 → 子 exit=1）+ `registry_inval`（park A=1 → mode_exit=0+empty）；file_count 账本全程配对 |
| 枚举命中正确 region 集 | `registry_inval`：A(rpoff=2) 全窗 truncate → 两端 INVALID+perm 保持；事件 [0,1] 对 A 纯 miss（zba>zea 推导）不计数不惊动 |
| 多 region 交叉 | `registry_inval`：A+B 同 mapping；事件 [2,514] 双命中 → routes+2，A 全窗降、B 的 [2,511] 降、B 页 2（上一事件 spare 的对照槽）随 even_cows 翻转降级 |
| 翻转后旧行为零命中 | `truncate_route` + `registry_inval`：全部事件族 `imap_stale_refuses` 恒零 + carrier 不在 i_mmap（结构断言）；直呼 `imap_stale_guard(carrier)` → true+计数+内容不动；zap_single backstop 拒绝样本保留 |
| 既有 B.2 语义回归 | `truncate_route` 全绿（KEEP_PERM/even_cows 两形状、refault 重读新内容、INV-MV3 clean）——仅枚举源断言由 interval-tree 改为注册表 |

新用例：`corten_arena_test_registry_inval`（含无 corten mapping 快门负样本）。

## 3. 改动统计

```
 include/linux/corten_arena.h |  27 +   （corten_inode_regions + region 字段 + 3 个 test 访问器）
 mm/corten_arena.c            | 315+/145-（注册表 xarray/ops、枚举、降级 guard、计数器/渲染/访问器、fput 修复）
 mm/corten_arena.h            |  31+/ 17-（声明翻转 + =n stub）
 mm/corten_arena_test.c       | 251+/ 73-（新用例 + 三个 V-B 用例的 i_mmap 断言翻转）
 mm/memory.c                  |  32+/ 13-（两枚举挂点 + vma gate 降级）
 合计 5 文件 +656/-248（净 +408；旧 gate 删除 ~77 行已抵扣在内；超出 ~280 估算的部分
 主要在注释论证块与 165 行新用例——本系列每片注释密度即此风格）
```

## 4. 验证结果（无盘 qemu，mva2-verify.sh；日志 `/home/ppw/cortenmm/results/r07/w1b/`）

| 项 | 结果 |
|---|---|
| `make -j8` | 零新增警告（全量仅 objtool cpuidle + modpost memblock 两条既有基线噪声） |
| KUnit corten=on（on1） | corten 23/1/1 · **corten_arena 101/0/0** · corten_fault 31/0/2；唯一 fail = `corten_test_txn_uninstall_interlock`（mm/corten_test.c，本片未触碰），**已登记宿主噪声 flake**（STATE.md:548、a5-fix.md、m9p2/m5t3-verify 同址同签名 "a_locked/violations/a_err"，用例写死 10s/20s 窗） |
| KUnit corten=on 复跑（on2，flake 判定） | **24/0/1 · 101/0/0 · 31/0/2 全绿**——两连跑零本片 flake；`ok 19 corten_arena_test_registry_inval` |
| max_t 修复后三跑（on2'） | 24/0/1 · 101/0/0 · 31/0/2 再绿（修复后终态验证） |
| KUnit corten=off（off1） | 25/0/0 · 24/0/77 · 7/0/26 全绿（既有 mode-dependent skip 形态） |
| =n 十四对象门 | RC 0、零警告、`nm` 零 corten 符号（含 mm/rmap.o、mm/memory.o）；.config 已恢复 =y |
| checkpatch（r07-w1b.diff） | **0E/0W**（1318 行；max() 告警已改 `max_t/min_t(pgoff_t,…)` 后复检归零） |

日志附注：on1/on2 中 `truncate_route` 附近的 2 条 WARN 栈 = 新旧两个 backstop 的
**设计内 WARN_ONCE 探针**（测试故意投喂 carrier），与既有 zap_single 探针同款、每次
boot 各至多一条；verify 脚本的 lockdep/oops 精确签名匹配零命中。

## 5. 移交与开放项

- **guest 门留主会话**（规格 W1.b 行：dlopen 后 truncate 库文件 → smaps 归零 +
  checksum + J1-J4 维持，B.2 门复跑）；bzImage 需以 `mva2-verify.sh bzimage` 重出。
- 过渡态（W1.b→W1.d 之间）窗口 file 页的 rmap 可达性缺口为**规格内时序**（§3.2 file
  真路由在 W1.d；mapcount pin 兜底），已在上文 §1.5 论证，评审若要求过渡期更强保证，
  可在 W1.d 前合入 ttu 钩子前置片。
- `imap_stale_refuses`/`zap_single_refuses` 恒零断言组建议进 guest 门脚本常读项；
  并发 attach/teardown 压测矩阵（规格 KUnit ③）未落，见 §1.5。
