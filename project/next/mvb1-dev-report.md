# V-B.1 开发报告：FILE 区 classify + attach + 引用生命周期

切片: M-V V-B.1（FILE 区第一片）
worktree: `/home/ppw/linux-6.18-mvb`（分支 mv-b，基座 = 217a9922a7d3 = A.2a/A.2b 已入库态）
日期: 2026-09-22
patch: `/home/ppw/cortenmm/patches/r07-mvb1.diff`（1872 行 diff，6 文件）
状态: **收口达成** —— =y 构建零新增警告、corten=on 无盘 KUnit 全矩阵全绿（连续 3 次）、
corten=off 零扰动、=n 折叠零符号、checkpatch --strict 0E/0W/0C。未 commit。

## 0. 结论一句话

FILE 区第一个生产者落地：`mmap(NULL, len, prot, MAP_PRIVATE, fd, off)` 在 MODE 进程内
经路由分类器（新 `CORTEN_MMAP_AUTO_FILE` 判定）→ `corten_file_may` 校验链 → 窗口放置 →
`corten_arena_file_attach`（region FILE 载荷 + FILE 形态 carrier + i_mmap 挂钩 + 全区段
FILE_MAPPED 虚拟分配）；引用计数在 release/park/mm_exit/mode_exit/fork 五个拆解点对称
回收（KUnit file_count 差分对账全过）。truncate/unmap_mapping 路由门（H7）按红线留给
B.2，region 登记的 rfile(→f_mapping)/rpoff/arena 界足以让 B.2 反查。

**发现并处置了一个基座自带的坏测试**（`corten_arena_test_fork_redeclare_content`，
A.2 commit 217a9922a7d3 引入即坏；详见 §5，含基座失败证据与 1 行修复）。

## 1. 改动点清单（file:line 为本片落码后实测）

### 1.1 数据结构 / 记录（spec §2.2，零新字段——字段 A.0 已建，本片给生产者）

| 改动 | 位置 | 说明 |
|---|---|---|
| `CORTEN_REGION_FILE` 判定臂（INV-MV3 规则 (d)） | mm/corten_arena.c:1011-1023（`corten_region_record_ok`） | FILE 记录三断言：rfile 非空 ∧ carrier->vm_file == rfile ∧ carrier->vm_pgoff == rpoff；非 FILE 类携带 rfile 即报告撕裂 |
| `corten_region_register` 残留 payload 响铃 | mm/corten_arena.c:1049-1057 | 非 FILE 重盖戳前若 rfile 仍在（漏掉 teardown 的调用方 bug）→ WARN 而非静默清空（静默清 = 引用泄漏） |
| `corten_region_register_file`（新，注册表 FILE 写侧） | mm/corten_arena.c:1093-1123；声明 include/linux/corten_arena.h:604-627 | payload 先落、类位后翻（读者只见完整 FILE 记录或无）；`get_file` 由记录持有，carrier 借用同一引用（同源同放）；不在此挂 i_mmap（发布最后一步才挂）。=n 内联折叠 include/linux/corten_arena.h:759-765 |

### 1.2 路由分类器（H1）与 file 校验链

| 改动 | 位置 | 说明 |
|---|---|---|
| 枚举 `CORTEN_MMAP_AUTO_FILE` | mm/corten_arena.h:231-234 | classify 第三判定 |
| classify file 臂 | mm/corten_arena.c:2861-2906 | file ∧ (flags & MAP_TYPE)==MAP_PRIVATE ∧ 余位 ⊆ {MAP_NORESERVE} → AUTO_FILE；MAP_SHARED/VALIDATE、MAP_FIXED（OQ-MV-2 植入例外维持 D-G''）、POPULATE/DENYWRITE/… 全部 LEGACY；file+MAP_ANONYMOUS 防御性 LEGACY；匿名臂逐字不变 |
| `corten_file_may`（新，纯函数） | mm/corten_arena.c:2913-3010 | do_mmap file 链逐臂重放（同序同 errno）：file_mmap_ok 两条式重推导（-EOVERFLOW，原函数 static 于 mm/mmap.c，逐字对拍注释标明）；`file_is_dax` 拒 DAX/fsdax/dev-dax（-EOPNOTSUPP）；FMODE_READ（-EACCES）；path_noexec ∧ PROT_EXEC（-EPERM）；`can_mmap_file`（-ENODEV）；`memfd_check_seals_mmap`（errno 透传；私有映射恒过 check_write_seal 本体）。may 全三位、仅 noexec 收 EXEC（§1.3 边的口径） |
| `corten_file_may_bound` | mm/corten_arena.c:2921-2935 | may 界单源（route 校验与 declare 重推导共用） |
| do_mmap 门放宽 | mm/mmap.c:425（`!file && addr == 0` → `addr == 0`）+ :426-427（file/pgoff 传入 route）+ :473-476（cret==1 完成：file 走 `corten_arena_file_attach`，否则 A.2a `auto_attach`） | 只动 auto 路由门区域，未触碰放置守卫区域（NOREPLACE 检查/fence/get_unmapped_area 均原样）——与 mva 并行轨 A.3a 无交集 |

### 1.3 路由 file 臂（H2）与 attach（H3 前半 + i_mmap）

| 改动 | 位置 | 说明 |
|---|---|---|
| route file 臂 | mm/corten_arena.c:3555-3700 | 签名增 `struct file *file, unsigned long pgoff`；classify==AUTO_FILE 时先过 `corten_file_may`（失败计数降级 legacy——errno 由 legacy 链原样给出）；**pool_take 跳过**（复用窗是 ANON 语义），window_place 照旧；OVERCOMMIT_NEVER 保守沿用（brief H2 注记的后续微调位） |
| `corten_arena_file_attach`（新） | mm/corten_arena.c:3751-3791 | do_mmap 完成体（auto_attach 的 FILE 对应物）：declare_locked(FILE) + 失败/成功计数同 A.2a 口径 |
| declare_locked FILE 臂 | mm/corten_arena.c:1486-1740 | @file/@pgoff 新参。pool_prepare 传 no_reuse（见下行）；carrier FILE 形态（vm_file/pgoff，get_file 由 register_file 持有）；register_file 戳记；**发布前**全区段 FILE_MAPPED 虚拟分配（见 file_mark）；xa_store/记账(vm_stat_account total_vm，RLIMIT 门在 route 的 corten_auto_validate 已跑——A.2a 口径)/obs_add 之后、ctl_unlock 之前做 i_mmap 挂钩（所有失败路径身后）；out_unwind 用 `corten_region_file_disarm`（见 1.4）回收半发布 payload |
| `corten_arena_pool_prepare_locked` no_reuse 参 | mm/corten_arena.c:7524-7541 | FILE 声明禁用"整窗重激活"半（ANON 语义），保留"逐出域内 parked 残留"半（oversized 全局放置路径会踩到 parked 帧） |
| `corten_arena_file_mark`（新） | mm/corten_arena.c:1419-1506 | fill_upper 全窗（mmap_route :8030 循环同型）+ 每窗一事务 `corten_mark(INVALID→FILE_MAPPED, perm)`（事务层零改动——mm/corten.c 状态机本就合法）。**这是防静默数据损坏的命门**：不预标则 FILE 区首 fault 落 FRESH 合成匿名零页；预标后 dispatch 走 STUB→SEGV_MAPERR（B.1 的诚实"尚无语义"判决，fault 双臂是 B.3）。失败回滚：mark 每窗全有或全无（validate-then-write），已标前缀逐窗 `corten_unmap` 洗回 INVALID |
| `corten_file_i_mmap_insert`/`_remove`（新） | mm/corten_arena.c:1128-1160 | `__vma_link_file` 逐字（i_mmap_lock_write → flush_dcache_mmap_lock → vma_interval_tree_insert/remove → 双解锁），不调 mapping_allow_writable（无 VM_SHARED）。锁序 = 内核既有 `mmap_write > i_mmap_rwsem`（vma_link 序），无新边 |

### 1.4 引用生命周期：teardown 五点 + fork 镜像（H8 put 面 / H9 子引用）

| 改动 | 位置 | 说明 |
|---|---|---|
| `corten_region_file_teardown`（新） | mm/corten_arena.c:1163-1192 | i_mmap 摘除 + carrier 归零形态（vm_file=NULL, vm_pgoff=0——carrier 跨 park 存活、复活恒 ANON，不得悬挂）+ `fput(rfile)`。非 FILE 记录 no-op |
| 收敛点 1：`corten_arena_free` | mm/corten_arena.c:568 | RELEASE / mode_exit / mm_exit 三路全部经此（free 前先 teardown——teardown 读 carrier 找树节点） |
| 收敛点 2：`corten_arena_pool_park_locked` | mm/corten_arena.c:7799-7805 | park(FILE)→RESERVED 同时 file put（INV-MV3 规则 (c)：复用窗 reactivate 恒 ANON，不得带 rfile 复活）；在 register(RESERVED) 戳记之前（否则 register 的残留 WARN 会响） |
| mm_exit / mode_exit | — | 不加新点：mm_exit 的 drain+free 与 mode_exit 的逐 arena RELEASE 分别收敛于 corten_arena_free / release_arena_locked→corten_arena_free |
| `corten_region_file_disarm`（新，发布前错误路径专用） | mm/corten_arena.c:1194-1215 | declare out_unwind（mm/corten_arena.c:1724）与 fork out_put_carrier（:4326）用：与 teardown 同-drop 但**不摘 i_mmap**（插入在一切失败路径身后，未插不可摘） |
| fork 子镜像 | mm/corten_arena.c:4185-4340（`corten_arena_fork_register_child`） | rclass==FILE 的父：子 carrier 生于 FILE 形态（父 rfile 指针借用 + 父 rpoff）→ `register_file` 取子自己的 get_file（同对象，INV-MV3(d) 指针等值成立）→ 发布尾部 i_mmap insert。fork PTE 复制零新码（B.1 无驻留页；copy_page_range 空窗 no-op，fork_mark_window 只动 MAPPED 槽，child 回放的 default 臂 mark 状态自迁移合法） |

### 1.5 =n 折叠

- mm/corten_arena.h 内部接口：`corten_arena_file_attach`/`corten_file_may` =n 内联桩（:675-703 区域），`auto_mmap_route` 桩随新签名同步；
- include/linux/corten_arena.h：`corten_region_register_file` =n 空桩（:759-765）。
- 实测（§4.3）：=n 构建 0 corten 符号、mm/built-in.a 0 corten 对象。

## 2. 测试锚（对照任务书第 5 条四类）

| 锚 | 位置 | 覆盖 |
|---|---|---|
| classify 真值表（file 全表 + 负向锚翻转） | mm/corten_arena_test.c:1243-1248（既有用例内 `classify(good,true)` LEGACY→AUTO_FILE 翻转，file 形口径 MAP_PRIVATE\|MAP_NORESERVE）+ 新用例 `corten_arena_test_auto_classify_file` :1292-1364 | 干净命中两形态；SHARED/VALIDATE/DROPPABLE 别名拒；file+ANONYMOUS 防御拒；14 个坏位逐位拒（FIXED=OQ-MV-2 植入例外在内）；匿名臂不受扰 |
| attach 三态真值 | `corten_arena_test_file_lifecycle`（mm/corten_arena_test.c:2037-2310）route 段 :2087-2105 | 非白名单 file 形 → 0 且请求字不动；白名单 → 1 + addr/len/flags 重写（MAP_FIXED\|NORESERVE 强制，2M 圆整）；attach 后 query/record/carrier/INV-MV3/FILE_MAPPED 槽位/dispatch STUB 逐项断言 |
| 引用计数收支（file_count 差分对账） | 同上用例 | 六态全链：attach(+1) → park(-1) → reactivate(0，恒 ANON) → 真 RELEASE(-1) → mode_exit(-1) → fork(+1)/子 mm_exit(-1)；强制 fork_commit 失败（stage 2）→ 子经 mm_exit 丢弃 → 零泄漏；终态 i_mmap 空树 + drain_timeouts 零增长 |
| i_mmap 可见性 | 同上用例 :2193-2276 | attach 后 `vma_interval_tree_iter_first([pgoff, pgoff+pages))` 命中 carrier；park 后全树查询为空 |
| INV-MV3(d) 负样本 | 同上用例 :2160-2188 | 三种撕裂（rfile 清空 / vm_pgoff 错位 / 类位改 ANON）逐个被 `corten_region_invariants_ok` 报告，复原后恢复 true |
| `corten_file_may` 矩阵（纯函数） | mm/corten_fault_test.c:460-560 `corten_fault_test_file_may_matrix` | 伪造 file/inode/mapping/mount/dentry（f_path const 经初始化器接线）：happy→may 三位；无 FMODE_READ→-EACCES；noexec∧EXEC→-EPERM 且 may 无 EXEC；无 mmap 钩子→-ENODEV；pgoff 越MAX_LFS_FILESIZE→-EOVERFLOW；DAX inode→-EOPNOTSUPP（CONFIG_FS_DAX=n 时 S_DAX 折叠为 0，臂内 IS_ENABLED 门控） |
| 既有负向锚维持 | corten_fault_test dispatch 表 "file" 行仍 STUB（B.3 才翻）；region_record 用例的 synth.rfile 断言未被触碰 | B.1 语义边界：FILE 区 fault 无语义，恒 MAPERR |

新用例已登记进两套件的 KUNIT_CASE 表（mm/corten_arena_test.c:7312/7315）。

## 3. 设计要点与红线核对

1. **INV6（任何 arena PTE 写必经事务）**：attach 只写元数据（每窗一 `corten_mark`/失败回滚 `corten_unmap` 事务）+ fill_upper 分配空 PT 页（非 PTE 写）；无任何驻留页 → i_mmap 里的 carrier 对 truncate walker 是空窗，B.1 中间态不产生裸写窗口（B.2 的路由门必须在 B.3 驻留页之前落——依赖序保持）。
2. **INV-MV3 (c)/(d)**：(c) park(FILE) 同时 file put + carrier 归零形态；(d) record_ok 新臂 + register 残留响铃 + KUnit 负样本。
3. **锁序不扩边**：唯一新嵌套 `mmap_write > i_mmap_rwsem`（内核既有 vma_link 序）；ctl_lock 内睡 rwsem 合法（DEV-13 域内既有形态）。
4. **引用收支单源**：get_file 仅在 register_file（region 持有）；carrier->vm_file 为借用指针；fput 仅在 teardown/disarm；carrier_free 不碰 vm_file（vm_area_free 本就不 drop 它）。指针等值（carrier->vm_file == rfile）使 INV-MV3(d) 同时成为泄漏/双放的行程线。
5. **错误路径零泄漏**：declare 的 mark/store 失败 → disarm（未插 i_mmap 不摘）；fork 的 percpu_ref/xa_store 失败 → disarm 于 carrier_free 前；强制 fork 失败 → 子 mm_exit 全量回收；KUnit 对账六态终值恒 == base。
6. **语义披露（S 系待 B.3/B.4 登记）**：B.1 中间态 FILE 区访问 = SEGV_MAPERR（非 SIGBUS；EOF/BUS 语义在 B.3）；FILE 区驻留不计 nr_mapped/shrinker（B.3 行为，文档化在 B.4）。file_may 对 sealed memfd 只透传 errno，F_SEAL_FUTURE_WRITE 的 VM_MAYWRITE 收紧未镜像进 may_prot（protect_range 尚未消费 may_prot——V-A.3 接线时补，登记为已知空档）。

## 4. 验证结果（全部无盘 qemu，命令形态照 mva2-verify.sh run_kunit）

日志目录：`/home/ppw/cortenmm/results/r07/mvb1/`

### 4.1 构建

- `make -j8`（=y）：exit 0，**零新增警告**（仅基座自带的 objtool cpuidle 与 modpost memblock 两条，与本片无关；build-y/…/build-final 日志）。
- =n（CORTEN_* 全关）：exit 0，同两条基座警告；`nm vmlinux | grep -ci corten` = **0**；`ar t mm/built-in.a | grep -c corten` = **0**（=n 折叠完整）。

### 4.2 KUnit（bzImage + `corten=on kunit.filter_glob=corten*`）

| boot | corten | corten_arena | corten_fault |
|---|---|---|---|
| on6（终码首验） | pass:24 fail:0 skip:1 | pass:64 fail:0 skip:0 | pass:31 fail:0 skip:2 |
| on7（复跑） | pass:24 fail:0 skip:1 | pass:64 fail:0 skip:0 | pass:31 fail:0 skip:2 |
| on8（checkpatch 修正后终版） | pass:24 fail:0 skip:1 | pass:64 fail:0 skip:0 | pass:31 fail:0 skip:2 |

skip 均为既有口径（=on boot 的 layout 用例、DAS 门控两例）。无 lockdep/死锁/oops 签名
（on8 逐项 grep 判定）。
- **interlock 单例复跑判定**：`corten_test_txn_uninstall_interlock` 在早期中间镜像上
  失败过 1 次（kunit-on2.log），终码镜像连续 3 次 boot 全过（on4/on6/on7/on8）——按既
  知 flake 口径判为偶发，非回归。
- **corten=off（=y 配置默认 off）**：corten 25/25、arena 24 pass+40 skip、fault 7
  pass+26 skip，fail 全 0（零扰动成立；kunit-off1.log）。
- 注：任务书引用的 `kunit.filter_glob=corten`（无 `*`，mva2-verify.sh 现状）只跑到
  基础 corten 套件——这正是一条**流程缺陷**（见 §5：A.2 团队的夜间验证因此从未执行
  arena/fault 套件）。本片按 r06 口径用 `corten*` 跑全矩阵（上表），同时该字面命令
  形态下基础套件亦全绿。

### 4.3 引用收支自证（file_lifecycle 用例实测断言链）

shmem_file_setup 建档 base=file_count(file)：
`route(0/1 两态) → attach=base+1 → park=base(且 carrier->vm_file=NULL、i_mmap 空树) →
ANON reactivate=base → 真 RELEASE=base → mode_exit=base → fork=base+2(父+子镜像，
子 rfile/rpoff/vm_pgoff 三一致 + 子 INV-MV3 walk true) → 子 mm_exit=base+1 → 强制
fork 失败+丢弃=base+1 → 父 mode_exit=base + i_mmap 空树 + drain_timeouts 恒定 → fput`。
每一跳都有 KUNIT_EXPECT_EQ 锚定（mm/corten_arena_test.c:2037-2310）。

## 5. 基座坏测试的发现与处置（需要 A.2 轨知晓）

- **现象**：`corten_arena_test_fork_redeclare_content` 在本 worktree **基座本身**
  （git stash 全部本片改动后构建，kunit-truebase.log）即确定性失败：park 后 re-declare
  返回 -EEXIST。
- **根因**：该用例在 217a9922a7d3（A.2 commit）新引入即坏——它对 4×PMD 的 arena munmap
  `PAGE_SIZE`，注释期望走"EXACT munmap → park"，但按 release_classify 该形状是 CHUNK
  （尾 8M-4K ≥ 2M），arena 保持存活，re-declare 的 -EEXIST 是当前代码的正确行为。正形
  应为 `arena_extent - PAGE_SIZE`（尾 < 2M → EXACT）。A.2 团队没抓到是因为
  mva2-verify.sh 的 `kunit.filter_glob=corten` 丢了 `*`（r06 及之前是 `corten*`），
  arena/fault 两套件自 A.2 起从未在夜验中执行过。
- **处置**：1 行测试修复（munmap 长度改 `CORTEN_ARENA_TEST_LEN - PAGE_SIZE`，注释记
  录事故），随本片 diff 携带；**判定证据链**：基座失败日志 kunit-truebase.log /
  kunit-notests.log（本片生产码在、新用例摘除后仍失败）→ 修复后基座+本片全绿。建议
  A.2/夜验轨把 mva2-verify.sh 的 glob 恢复为 `corten*`。
- 另一基座噪声：`BUG: non-zero pgtables_bytes on freeing mm` 行在基座即有 7 处/次
  （测试自管 teardown 的残留上表，既有口径）；本片新增用例的子 mm 产生 +2 同形行
  （fork 子继承 PT 上表、free_ptes_novma 留暖上层的既有形状），已在报告披露，非本片
  生产码引入。

## 6. 与并行轨的边界确认

- 本片触碰 mm/mmap.c 仅 :412-494 的 do_mmap auto 路由门区（classify/file 区域）；
  放置守卫区域（NOREPLACE 检查 :476-486、`corten_fence_unmapped_area`/
  `corten_addr_in_window` 调用点 :816-896、J1 probe、corten_vma_find）零改动。
- 未触碰 mm/vma.c。
- mm/corten_arena.c 的 classify/auto-route/declare/pool/fork/park 区域为本片任务书
  指定区域。

## 7. 后续（B.2/B.3 接口已备）

- B.2 可直接使用：region 的 `rfile->f_mapping` + `rpoff` + arena 界（truncate 反查全量）；
  i_mmap insert/remove 已在位且被 KUnit 锚定（B.2 只需落 H7 两处门 + KEEP_PERM 对拍）。
- B.3 前置已锁：dispatch STUB 行、FRESH 合成臂均未动（负向锚维持），FILE_MAPPED 全区
  预标保证中间态 fault 恒 MAPERR 而非静默匿名页。
