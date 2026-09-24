# V-B.3 开发报告：FILE 区 fault 双臂 + 翻 D26 暗门（FILE 区激活片）

切片: M-V V-B.3（H4 dispatch + H5 read 臂 + H6 COW 写臂 + H7 follow-through + D26 暗门翻除）
worktree: `/home/ppw/linux-6.18-mvc`（分支 mv-c = 主树 c8260c0487d4 + B.2 未提交增量 + 本片）
日期: 2026-09-22/23
patch: `/home/ppw/cortenmm/patches/r07-mvb3.diff`（= git diff HEAD，含 B.2 预铺增量；
本片自有增量约 +1132/−45）
状态: **收口达成** —— =y 构建零新增警告、corten=on 无盘 KUnit 三套件全绿（连续 5 次终码
boot）、corten=off 零扰动、=n 折叠零符号、checkpatch --strict 0E/0W/0C。未 commit
（B.2 部分原样保留未动，除任务书指定改语义的两处，见 §4）。

## 0. 结论一句话

FILE 区真正激活：FILE_MAPPED 槽 fault 走 `corten_arena_file_read`（锁外 filemap 取页
→ 截断复查 → 重锁 re-query → 事务装页，EOF→`CORTEN_FAULT_BUS`→arch 快钩 force_sig
SIGBUS/BUS_ADRERR）与 `corten_arena_file_cow`（首写 do_cow_fault fetch+copy / 已读页
in-place cow_write file 分支，FILE_MAPPED→MAPPED 迁移）；FRESH 合成臂 rclass-aware
（FILE 区 Invalid 槽重合成 FILE_MAPPED 重读文件，H4 红线）；D26 暗门
（`corten_file_route_test_override` + AUTO_FILE 早退）删除，dlopen 形态真走 FILE 轨。
另完成 B.2 预埋的 `even_cows` 接线：!even_cows（invalidation/reclaim 族）事件
**只 demote FILE_MAPPED 槽**，COW 私有页（MAPPED/SWAPPED）按 should_zap_cows 语义保留
——B.3 造出第一批 FILE 区 MAPPED 槽后这条从"注释承诺"变成可达语义，必须在同片落。

## 1. 改动点清单（file:line 为本片落码后实测）

### 1.1 枚举/交付增量（§2.3）

| 改动 | 位置 | 说明 |
|---|---|---|
| `CORTEN_FAULT_BUS` | include/linux/corten_arena.h:482-486 | EOF 的 SIGBUS BUS_ADRERR 交付 action（非 MAPERR：legacy beyond-EOF 是 SIGBUS，S-FILE-1 登记） |
| `CORTEN_F_BUS` | mm/corten_arena.c:5484-5489 | 内部 status；快钩 user_fault 交付 CORTEN_FAULT_BUS（:7302-7308），慢钩 handle_mm_fault 映射 VM_FAULT_SIGBUS（:7454-7459） |
| `CORTEN_DISP_FILE_READ` | mm/corten_arena.h:86-94 | read 臂判定（COW 复用 COW_MAYBE，写无权 ACCERR） |
| arch 快钩 case | arch/x86/mm/fault.c:1378-1386 | `force_sig_fault(SIGBUS, BUS_ADRERR, addr)`，ACCERR 臂同款 no-lock 交付形态 |

### 1.2 dispatch（H4）

mm/corten_arena.c:5294-5313：FILE_MAPPED 独立 case（不再与 SHARED_ANON 共享 STUB）：
`!perm_ok → ACCERR`（legacy access_error 对 !VM_WRITE file VMA 的口径，fork-shared 与
否不分岔——FILE 的写答案恒 COW）；`write → COW_MAYBE`；`read/instr → FILE_READ`。

### 1.3 FRESH 合成 rclass-aware（H4 命门，静默内容损坏级红线）

mm/corten_arena.c:7010-7024：rclass==FILE 时 `fresh.state = CORTEN_FILE_MAPPED`
（替换 B.2 的 MAPERR 拒绝臂）。perm 门沿既有 gate.perm（KEEP_PERM 残留优先，其次
ar->prot）。效果：DONTNEED/截断后 re-fault = FRESH 重合成 FILE_MAPPED → dispatch →
read 臂重读文件（EOF 外 BUS），**绝不**合成 PRIVATE_ANON 匿名零页。

### 1.4 取页 `corten_arena_file_fetch`（H5 核心，锁外）

mm/corten_arena.c:6437-6591。纯锁外（INV3），arena 由 active ref + rfile 引用钉住：

- **EOF 门**：`pgoff >= DIV_ROUND_UP(i_size_read, PAGE_SIZE)` → -ENODATA（BUS）。
- **shmem/tmpfs 宿主**（`shmem_mapping()`）：`shmem_get_folio(SGP_CACHE)`（include/
  linux/shmem_fs.h）——真 hole 分配器。**开发中发现并规避了一个会出静默账目损坏的
  陷阱**：对 shmem mapping 直接 `FGP_CREAT` 会绕过 `shmem_inode_acct_blocks`（tmpfs
  配额泄漏、info->alloced 漂移、后续 recalc 把 used_blocks 打负）、不会回读被换出的
  entry、且给出未清零内存当 hole 内容。SGP_CACHE 三者全对；其自身 EOF 竞态答 -EINVAL
  （映射回 -ENODATA）。
- **常规文件宿主**：`filemap_invalidate_lock_shared` → `__filemap_get_folio
  (FGP_CREAT|FGP_FOR_MMAP)`（miss=同步读入，PGMAJFAULT 计数）→ folio_lock →
  截断复查 `folio->mapping != mapping`（→-EAGAIN 有界重试）→ !uptodate 走 filler
  （filemap_read_folio 本体逐字重放：read 解锁 folio、killable 等待、**重新上锁**后
  复查——首次实现曾犯"filler 后未持锁即 unlock"的锁权错误，构建前自纠）。
- **i_size 复查在 folio lock 下**（filemap_fault 的 "We must recheck i_size under
  page lock"）——两条宿主路径都有。
- 无 read_folio 且非 shmem 的残留形态 → -EIO（→BUS，响亮拒绝而非造零页）。

### 1.5 read 臂 `corten_arena_file_read`（H5，自管锁循环）

mm/corten_arena.c:6614-6769。fault_once 释放 desc 写锁后进入（swapin 同款形态）：
fetch → `corten_lock_range` 重锁 → re-query（state 必须 FILE_MAPPED ∧ perm 不变，否则
-EAGAIN 进 Fig.7 有界预算）→ ptl 内 PTE 复查（none 才装；present 同 folio =
竞态赢家，drop 冗余引用返 0；present 异页 = re-dispatch）→ `mk_pte(page,
perm_pgprot)`（x86 私有映射保护表 PAGE_COPY 天生无 RW——RW 契约的读安装只读，写必
经 fault，与 legacy vma_wants_writenotify 机制效果一致，KUnit 有 !pte_write 锚）→
`folio_add_file_rmap_pte`（rmap 挂 i_mmap 可见 carrier）→ `add_mm_counter(
MM_FILEPAGES,1)` → set_ptes（PTE 原为 none，无 TLB flush）。**meta 不写**：
FILE_MAPPED 即驻留形态（页身份可由 rpoff+页内偏移推导，无 __resv 负载）。folio 引用
收支三出口：成功转 PTE / 冗余 put / 失败路径 put。

### 1.6 COW 写臂（H6）

- **首写-空 PTE（do_cow_fault 形态）** `corten_arena_file_cow` mm/corten_arena.c:
  6772-6955：第一腿持事务查 PTE——present 即转 `corten_arena_cow_write`（同事务）；
  none 则解锁 → fetch（锁外）→ 重锁 re-query（meta/PTE 双复查）→
  `copy_user_highpage` 到投机 folio → 新匿名页 rmap(EXCLUSIVE) +
  `corten_map(FORCE)` FILE_MAPPED→MAPPED（"any→MAPPED"合法 COW 边）→ fetch 引用
  put。EOF on fetch → BUS（写越 EOF = legacy do_cow_fault 的 SIGBUS，不是零页私有页）。
- **已读页 in-place COW**：`corten_arena_cow_write` file 增量（:5895-5919,
  :6006-6016）：`old_is_file = !folio_test_anon(old)`；**reuse 恒否**（pagecache
  mapcount 是全机业务，PageAnonExclusive 对它是谎言）；counter 拆分
  +MM_ANONPAGES/−MM_FILEPAGES（与 corten_zap_release_page 同口径）；file rmap 走
  同一 `folio_remove_rmap_pte`；meta `corten_map(FORCE)` 迁移。二次写（此时 anon+
  MAPPED）走既有 reuse 臂。
- **retry 预算**：ctx 新增 `fileio`，`swapin || fileio ? 4 : 2`（2+2，fetch 重锁
  re-validation 输给竞态truncate 时多花一轮）——快慢两钩同步（:7263, :7405）。

### 1.7 H7 follow-through：`even_cows` 接线（B.2 预埋消费）

- mm/corten_arena.c:210-218 新内部 zflag `CORTEN_UNMAP_FILE_EVENT`（BIT(2)，zap
  driver 私有，`corten_unmap` 调用点以 `zflags & CORTEN_UNMAP_ALL` 掩蔽——事务层
  零改动）。
- `corten_arena_unmap_file_event`（:1310-1325）：`!even_cows` 时置位，经
  `unmap_chunk_flags` 下发。
- `corten_arena_zap_window` 槽循环（:7787-7803）：FILE_EVENT 下 state==MAPPED 或
  SWAPPED 的槽整体跳过（PTE/meta/页全不动）——legacy `should_zap_cows` 对 COW 页
  **和** swapped COW entry 的保留语义（mm/memory.c zap_pte_range 1753/1784 实证）。
  FILE_MAPPED/无记录 PTE 照旧 demote。truncate 族（even_cows）行为不变（全 demote）。

### 1.8 翻 D26 暗门

- 删 `bool corten_file_route_test_override`（原 :151）+ mm/corten_arena.h 的
  `#ifdef CONFIG_CORTEN_MM_ARENA_KUNIT_TEST extern` 声明 + route 里 AUTO_FILE 的
  override 早退（原 :3823-3825）。保留 `corten_file_may` 拒绝降级门（errno 语义链）。
  全树 grep 零残留。**FILE 轨自此刻对 dlopen 形态真接管**。

### 1.9 B.1/B.2 依赖暗门语义的断言更新（任务书第 4 条）

| 位置 | 旧 | 新 |
|---|---|---|
| file_lifecycle 的 dispatch 锚 | `CORTEN_DISP_STUB` | `CORTEN_DISP_FILE_READ` |
| truncate_route 的 re-fault 断言 | `CORTEN_FAULT_MAPERR` + meta 停 Invalid | kernel_write 新内容 → `CORTEN_FAULT_HANDLED` + meta 回 FILE_MAPPED + page_word 内容 == 新 pattern（截断-重读全链锚） |
| `corten_arena_test_file_route` 包装 | 置/清 override | 直跑真路由（成为接管回归锚；三态断言原样通过） |
| corten_fault_test dispatch 表 "file" STUB 行 | 1 行 | 4 行：file-read→FILE_READ / file-exec-instr→FILE_READ / file-write-writable→COW_MAYBE / file-write-ro→ACCERR |
| `corten_arena_test_file_attach` | 固定 R\|X | 拆 `_prot` 变体（COW 用例需 R\|W），原 4 参名保持（6 个调用点零扰动） |

### 1.10 =n 折叠

无新外显符号：BUS 枚举值在既有 enum 内；fetch/read/cow 全 static；FILE_EVENT 是 .c
内 #define；x86 case 在既有 `CONFIG_CORTEN_MM_ARENA` 块内。实测（§5.4）：=n 构建
rc 0、nm 0 corten 符号、mm/built-in.a 0 corten 对象（两次，含 spare 接线后终码）。

## 2. KUnit 锚（任务书第 5 条四类 + 补充）

| 锚 | 位置 | 覆盖 |
|---|---|---|
| 读路径 attach→fault→内容 | `corten_arena_test_file_read`（mm/corten_arena_test.c:2537-2700） | HANDLED + meta 不写（仍 FILE_MAPPED）+ PTE present∧clean∧**!pte_write**（PAGE_COPY 私有 COW 形）+ 页即 pgoff 的 pagecache folio + `folio_mapcount==1`（rmap 挂 carrier）+ page_word==文件 pattern + hole 页读零（shmem clear 形）+ 重 fault 幂等（冗余引用臂）+ MM_FILEPAGES 精确 +1/+2（`percpu_counter_sum_positive` 直读——get_mm_counter 的 batch 不可见是首跑失败根因，见 §5.2）+ teardown 后计数归零 |
| EOF→BUS | 同上 | pgoff==max_idx（3 页文件第 4 页）→ `CORTEN_FAULT_BUS` + meta 保持 FILE_MAPPED + 无杂散翻译/记账 |
| COW 首写迁移+隔离 | `corten_arena_test_file_cow`（:2704-2944） | 已读页首写（in-place）：meta FILE_MAPPED→MAPPED、计数拆分（+ANON/−FILE）、私有页内容==文件内容、**文件侧隔离**（写穿私有页后 pagecache folio 内容不变 + mapcount 回 0）；二次写 reuse 臂；**空槽首写**（do_cow_fault fetch+copy）：同迁移 + 内容==文件 + 隔离；EOF 写→BUS（非零页）；R-only 区写→ACCERR |
| 截断后 re-fault 重读 | truncate_route 改造段（:2408-2443） | demote → kernel_write 新内容 → re-fault HANDLED + FILE_MAPPED 重合成 + 内容==新 pattern |
| 路由接管回归 | file_lifecycle（无 override 直跑） | 非白名单→0；白名单→1 + MAP_FIXED\|NORESERVE 重写——三态断言原样通过 |
| invalidation 保留私有页 | file_cow 尾段（:2880-2916） | `unmap_mapping_pages(…,false)`：MAPPED 槽+内容原样存活、邻 FILE_MAPPED 槽 demote+KEEP_PERM（should_zap_cows 对拍） |

## 3. 红线核对（§6 表逐条）

1. **INV6**：read 臂/COW 的 set_ptes 全部在 desc 写锁+ptl 内（事务内）；无第 4 白名单
   写点；zap backstop 计数全程 0（truncate_route 负样本照旧断言）。
2. **=n 折叠**：§1.10，两次实测。
3. **H4 红线（静默损坏）**：FRESH rclass-aware + KUnit 截断-重读/EOF 双锚 +
   file_lifecycle 的 STUB 翻正向；read 臂 fetch 对 shmem 用真分配器（规避配额漂移——
   另一类静默损坏，§1.4）。
4. **EOF 语义**：SIGBUS BUS_ADRERR 与 legacy do_read_fault 一致，经快钩交付是新路径
   （S-FILE-1 已在代码注释与 B.2 报告口径登记；guest strace/si_code diff 属 B.4 全量
   判据，本片 KUnit 以 CORTEN_FAULT_BUS 动作值锚定）。
5. **锁序**：新嵌套仅 `filemap_invalidate_lock_shared > folio_lock`（内核既有序），
   完全在 desc 锁外；shmem_get_folio 同理。fetch 先释放、后重锁 corten txn——与
   swapin 同型，无 i_mmap/desc 交叉。folio 引用三出口收支全 put。
6. **retry 有界**：fileio 预算 2+2；re-query 拒绝全部 -EAGAIN 收敛（单线程 KUnit 无法
   注入锁外窗口中点的竞争——真竞态覆盖记为 guest 口径，见 §6 风险）。

## 4. 对 B.2 未提交增量的触碰清单（合规性）

仅两处任务书指定 + 一处测试基建：① FRESH 臂的 MAPERR 拒绝块 → rclass 重合成（任务
书第 3 条）；② truncate_route 的 re-fault 断言族 → 重读语义（任务书第 4 条）；
③ file_attach 测试包装拆 prot 变体（COW 用例需要）。B.2 其余（route gate/backstop/
计数器/mm/memory.c 双门）逐字未动；even_cows 接线是把 B.2 自己注释里"B.3 消费"的
参数接上，gate 主体逻辑未变。

## 5. 验证结果（无盘 qemu，命令形态照 mva2-verify.sh run_kunit）

日志目录：`/home/ppw/cortenmm/results/r07/mvb3/`

### 5.1 构建

- `make -j8`（=y）：exit 0，**零新增警告**（仅基座两条：objtool cpuidle、modpost
  memblock；build-y1…y5 五次全同）。
- =n（CORTEN_* 全关）：exit 0；`nm vmlinux | grep -ci corten` = **0**；
  `ar t mm/built-in.a | grep -c corten` = **0**（build-n.log / build-n2.log，后者为
  spare 接线后终码）。

### 5.2 KUnit（bzImage + `corten=on kunit.filter_glob=corten*`）

| boot | corten | corten_arena | corten_fault | 备注 |
|---|---|---|---|---|
| on1（首验） | 24/0/1 | 70/**2**/0 | 31/0/2 | 两失败=新用例的 mm 计数断言（get_mm_counter percpu batch 不可见），改 percpu_counter_sum_positive |
| on2 | 24/0/1 | **72/0/0** | 31/0/2 | 修复后首绿 |
| on3 | 24/0/1 | 72/0/0 | 31/0/2 | 复跑绿 |
| on4-final | 24/0/1 | 72/0/0 | 31/0/2 | checkpatch 修后终码 |
| on5-final | 24/0/1 | 72/0/0 | 31/0/2 | 0 lockdep/oops 签名 |
| on6-final | 24/0/1 | 72/0/0 | 31/0/2 | even_cows spare 接线后终码 |
| off1/off2-final（corten=off） | 25/0/0 | 23/0/49 | 7/0/26 | 零扰动 |

- **interlock flake 判定**：`corten_test_txn_uninstall_interlock` 在本片全部 7 次
  boot（on1-on6 + off）全过，无复现；B.2 遗留的单例失败维持"偶发"口径。
- 新用例 ok 确认：`file_read` / `file_cow` / `truncate_route`（新语义）/ `file_lifecycle`
  （无 override 直跑）逐条 ok。
- 既有噪声核对："Bad rss-counter state"（MM_SWAPENTS −1）与 pgtables_bytes BUG 行在
  B.1 truebase/B.2 日志即有，本片 on2/on3 各 2 处 == mvb2 on2/on3 基线（非回归）。

### 5.3 checkpatch

`scripts/checkpatch.pl --strict --no-signoff --ignore FILE_PATH_CHANGES
r07-mvb3.diff` → **0 errors, 0 warnings, 0 checks**（1883 行；首版 5 个 CHECK——
2 处 spinlock 注释、1 处前向声明对齐、2 处生产码 spinlock 注释——全部修正后归零）。

### 5.4 改动统计

git diff HEAD（B.2 预铺 + B.3）：**7 文件，+1552/−46**。B.2 预铺为 +420/−1，故 B.3
自有 ≈ **+1132/−45**，其中生产码（corten_arena.c 双臂+spare+route+dispatch+快慢钩+
x86+头）≈ +590，测试 ≈ +540。超 brief 的 ~300 行预算主因：fetch 的双宿主形态
（shmem 真分配器路线）与 even_cows spare 接线都是正确性必需，非镀金。

## 6. guest 门风险点（JTB/java 首次真跑 FILE 轨，主会话判据建议）

按风险从高到低：

1. **GUP-slow/fork 交织**（最高）：JTB 的 JIT/Profiler 会 get_user 或 fork。FILE 区
   GUP 落 shadow 载体走慢钩——本片慢钩路径（FILE_READ/COW 经 handle_mm_fault 变体）
   KUnit 只经快钩直驱验证，慢钩与 GUP 的组合是首跑盲区；fork 的 FILE_MAPPED PTE
   复制（copy_page_range 对 pagecache folio 的 folio_try_dup_file_rmap_pte + 子区
   SHARED 标记 → 父子双写各自 COW）是 B.4 对拍面，JTB 若在 fork 前后写 .so 数据段
   会踩到未经 KUnit 的路径。
2. **真文件宿主（ext4/overlayfs）的 read_folio 臂**：KUnit 只覆盖 shmem 宿主；
   FGP_CREAT+filler+killable wait+重上锁的重放路径首次真跑。EIO/中断（jiotest
   killable）分支零覆盖。
3. **并发 truncate/reclaim vs fetch 的重锁 re-query**：单线程 KUnit 无法注入锁外窗口
   中点的 demote；JTB 若有 jar 替换/tmp 清理（/tmp 常为 tmpfs → shmem 宿主 +
   unmap_mapping 族）会真打 EAGAIN 重试预算（2+2）。若见非零 retry 计数伴随卡顿，
   先查这里。
4. **大 folio**：shmem THP（mTHP）开启时 SGP_CACHE 可能给 >0 阶 folio；read 臂按
   folio_file_page 子页装单 PTE 是对的，但 KUnit 未开 THP 验证。建议 guest 判据含
   `thp_shmem=never` 与默认两态。
5. **perf2a churn 回归**：翻门后 MODE 进程的 dlopen 全走窗口放置（不再回落 legacy
   mmap_base）——VA 布局变化会改变 JTB 的地址形态与 perf2a 的 churn 计数基线，属
   预期内漂移，需与 B.4 的 strace/checksum 判据区分。
6. **hugetlbfs fd 边角（B.1 遗留披露）**：显式 open 的 hugetlbfs fd（无 MAP_HUGETLB
   位）能过 corten_file_may；fetch 会 -EIO→BUS 而非 legacy 的正确大页服务。JTB 不
   消费此形态，登记为 B.4/A.3 白名单审计项。

## 7. 后续（B.4 接口已备）

- fork 对拍（父子 FILE_MAPPED 三方一致 + 双写隔离）、debugfs rfile/rpoff 列与
  file_read_faults/file_cow_copies 计数、guest 全量判据（JVM+CDS off、dlopen checksum、
  截断-重读 strace/si_code diff、perf2a）。
- double_fault_race（brief §5.5）的锁外中点竞争注入无单线程 KUnit 手段，收敛性由
  retry 预算代码路径保证 + guest 覆盖；如需强锚可加 fail-injection 钩子（本片未做，
  避免生产码增测试后门）。
