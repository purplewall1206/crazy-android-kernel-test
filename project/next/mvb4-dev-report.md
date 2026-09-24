# V-B.4 开发报告：FILE 区 fork 对拍 + 引用迁移 + 观测面（V-B 收口片）

切片: M-V V-B.4（H9 fork 对拍 + H10 观测面 + hugetlbfs 白名单收口 + pinned 拷贝路径）
worktree: `/home/ppw/linux-6.18-mvb`（分支 mv-b = 主树 d4badaed5b7d（V-B.3 已入库）+ 本片未提交增量）
日期: 2026-09-22/23
patch: `/home/ppw/cortenmm/patches/r07-mvb4.diff`（= git diff HEAD，3 文件 +854/−26；
生产码 mm/corten_arena.c +116/−16，测试 +738/−10）
状态: **收口达成** —— =y 构建零新增警告、corten=on 无盘 KUnit 三套件全绿（连续 5 次终码
boot：on3-on7）、corten=off 零扰动（off1/off2）、=n 折叠零符号、checkpatch --strict
0E/0W/0C。未 commit。**本片对拍揪出并修掉两个 fork×FILE 真缺陷**（rss 计数族错配 →
child exit "Bad rss-counter state" BUG；pinned/DONTCOPY 形态的 SHARED 残留 → INV7
漂移），见 §2/§3。

## 0. 结论一句话

FILE 区 fork 闭环收口：fork 镜像对拍（父子 rfile/rpoff/carrier/i_mmap 三方一致 +
pagecache folio 的 mapcount/refcount 差分）、wrprotect 后父子首写各自 COW 到私有
anon 页且三方可区分（父词/子词/文件内容）、GUP-pinned 页的 copy_present_page 拷贝
路径（6.18 基座上 file rmap dup 无失败路径，pinned 拷贝只剩 anon 形态可达——经
FILE 区 COW 私有页锚定）、hugetlbfs 显式 fd 经 corten_file_may 拒绝（-EOPNOTSUPP
降级 legacy，B.3 披露第 6 条收口）、观测面四计数器（file_mmaps/file_read_faults/
file_cow_copies/file_fork_mirrors）+ arenas 台账 rfile/poff 列。V-B 四片（B.1 结构
→ B.2 红线 → B.3 语义 → B.4 fork+观测）至此全部落地。

## 1. 改动点清单（file:line 为本片落码后实测）

### 1.1 hugetlbfs fd 白名单收口（任务书第 4 条，B.3 披露第 6 条）

| 改动 | 位置 | 说明 |
|---|---|---|
| `is_file_hugepages(file)` → -EOPNOTSUPP | mm/corten_arena.c:3235-3246（corten_file_may 内，紧随 DAX 门） | 显式 open 的 hugetlbfs fd（mmap 无 MAP_HUGETLB 位，classify 白名单看不见）不再入窗：fetch 臂会给 -EIO→BUS 而非大页服务的静默错答。errno 沿 DAX 门的降级专用 -EOPNOTSUPP；route 侧 ret 0 降级，legacy 链（hugetlbfs_mmap 的 VM_HUGETLB/hstate 语义）照常服务。`is_file_hugepages` = `f_op->fop_flags & FOP_HUGE_PAGES`，!CONFIG_HUGETLBFS 时折叠为 `false`（=n 折叠零成本） |
| 函数头注释链补行 | :3179-3186 | do_mmap 校验链清单补 hugetlbfs 门一行 |

### 1.2 fork 侧 pinned-aware SHARED 标记（任务书第 2 条落地形态）

- **基座事实修正**：brief/风险清单写的 `folio_try_dup_file_rmap_pte` 失败路径在
  6.18 基座**已不存在**——上游把 file rmap 的 fork 复制改成无条件
  `folio_dup_file_rmap_pte`（mm/memory.c:1183，rmap.h:601；git log 无该 try 变体）。
  pagecache 页 fork 恒共享（mapcount++），父子写各走 arena COW 拷贝分支——语义
  正确，无 arena 侧新代码需求。"临时拷贝"语义（copy_present_page，prealloc 重试
  舞步）只剩 **anon** folio 的 `folio_try_dup_anon_rmap_pte` -EBUSY（GUP pin）路径
  可达——FILE 区经 COW 私有页即此形态（§2 锚 3）。
- **生产修复** `corten_arena_fork_mark_window`（mm/corten_arena.c:4410-4445）：
  MAPPED 槽打 SHARED 前先读父 PTE——`present ∧ !special ∧ pte_write ∧
  PageAnonExclusive(pte_page)` 即"子并不映射本页"的私有形态（pinned 拷贝：子拿到
  自己的副本，父 PTE 未被 wrprotect、exclusive 未清；DONTCOPY 片边界同形），**跳过
  SHARED 只记 WRITABLE**。真共享形态天然可区分：copy_page_range 的 wrprotect 已把
  父 PTE 打 RO。修复前该形态在父/子两侧都留下"writable PTE 背后的 SHARED 记录"
  = INV7 walk 直接命中的漂移形态（inv7_walk 的 `MAPPED+SHARED ⇒ !pte_write` 断言），
  顺带消除一次无谓的首写 COW 绕行（F2 注释预言的残留类）。快照（子重放的输入）同
  步不带 SHARED——单一修复点，子侧无需改。未知形态（无翻译/special）保持历史行为。

### 1.3 rss 计数族对齐 mm_counter_file（fork 对拍揪出的缺陷 1）

- **症状**：fork 对拍首跑即 child exit 打出 `BUG: Bad rss-counter state
  MM_FILEPAGES val:-1 / MM_SHMEMPAGES val:1`——B.3 的 FILE 记账把一切 file 页记
  MM_FILEPAGES，而 fork 的 copy_page_range（走 carrier 对）按 `mm_counter_file()`
  记：**shmem/tmpfs 宿主 = MM_SHMEMPAGES**（include/linux/mm.h:2767）。子 mm 的
  fork 侧 +SHMEMPAGES 与 arena 侧 zap −FILEPAGES 永久错开。
- **修复**（三处生产点，计数族选择的对齐，不动臂主体逻辑）：
  read 臂安装 `add_mm_counter(mm, mm_counter_file(folio), 1)`（:6795-6801）；
  cow_write 拷贝分支旧页 `-mm_counter_file(old)`（:6107）；corten_zap_release_page
  file 分支 `-mm_counter_file(folio)`（:7726-7731）。对齐后与 legacy
  do_read_fault/wp_page_copy/zap_pte_range **逐位一致**（shmem 私有映射在 smaps 本就
  记 RssShmem）；非 shmem 真文件两族同值，行为不变。
- B.3 测试断言随族更新（§4 合规清单）：file_read/file_cow/new 三测的 filepg 族改经
  `corten_arena_test_file_rss(mapping)`（= shmem_mapping ? MM_SHMEMPAGES :
  MM_FILEPAGES，mm/corten_arena_test.c:2546-2552）。

### 1.4 观测面（H10）

| 增量 | 位置 | 说明 |
|---|---|---|
| 四计数器 | mm/corten_arena.c:218-231 定义；:2436-2445 渲染 | `file_mmaps`（file_attach 成功，:4037）/ `file_read_faults`（read 臂安装，:6808）/ `file_cow_copies`（cow_write file 分支 :6124 + file_cow fetch+copy :6997）/ `file_fork_mirrors`（register_child 发布后 :4593）——沿 :176-193 atomic_long 惯例，stats_report 列名 20 列对齐 |
| arenas 台账 rfile/poff 列 | :2331-2357 | 每行尾部 `%016lx %08lx`：region 的 file 指针 + rpoff（非 FILE 区 0）；RCU walk 惯例的竞态快照口径 |
| KUnit 读取面 | 既有 `corten_arena_test_named_counter`（渲染 debugfs 文本解析） | 新测试的计数器对账全部经渲染面读（不新增 test-only 访问器） |

### 1.5 =n 折叠

无新外显符号：计数器 static、hugetlb 门折叠（is_file_hugepages→false）、
mm_counter_file 是 mm.h 既有 inline、pinned 判定全在既有函数内。实测（§5.4）：
=n 构建 rc 0、`nm vmlinux | grep -ci corten` = 0、`ar t mm/built-in.a | grep -c
corten` = 0。

## 2. KUnit 锚（任务书第 5 条四类）

| 锚 | 位置 | 覆盖 |
|---|---|---|
| fork 镜像对拍 + 引用对账 + 计数器对账 | `corten_arena_test_file_fork_mirror`（mm/corten_arena_test.c:2987-3188） | read-install → fork：子 rfile==父 file 对象且 file_count +1（子自己的 get_file）、rpoff 同源、子 carrier vm_file/vm_pgoff 同源（INV-MV3(d) 双侧）、**i_mmap 同 pgoff 恰两成员**（interval_tree_foreach 计数=2 且恰为父子 carrier）、pagecache folio mapcount 1→2 / refcount +1（PTE 引用差分）、子 MM_SHMEMPAGES +1（fork 侧 copy_page_range 记账）、父子 PTE 同 pfn 且 RO、双侧内容==文件 pattern、双侧 INV7 walk 0 违约、file_mmaps/file_read_faults/file_fork_mirrors 计数器经 debugfs 渲染面 +1 对账；child mmput 后 ledger 全回（mapcount 1/ref 回基线/file_count 回 +1）；父 mode_exit 后归 base、drain timeout 零增长 |
| 父子首写 COW 隔离 + 三方可区分 | `corten_arena_test_file_fork_cow`（:3191-3404） | fork 后父首写（in-place cow_write file 分支）：MAPPED 迁移、+ANON/−SHMEM 拆分、folio mapcount 2→1、file_cow_copies +1；子首写同页：独立私有页 + 独立词，mapcount→0，计数器拆分；**三方对拍**：父词≠子词≠pagecache 内容（kmap 直读 folio 验证文件侧零污染）；子 never-faulted 槽首写（do_cow_fault fetch+copy 形态）：cow_copies +1、子词写入后父 read fault 重装 pagecache 页读到**文件内容**（fork 不 privatize 文件侧的反向锚）；双侧 region invariants ok；teardown ledger 归 base |
| pinned 拷贝路径 | `corten_arena_test_file_fork_pinned`（:3405-3640） | FILE 区 COW 私有页（exclusive anon）+ 模拟 FOLL_PIN（folio_ref_add(GUP_PIN_COUNTING_BIAS) + **mm_flags_set(MMF_HAS_PINNED)**——folio_needs_cow_for_dma 的快门，漏设则 dup 根本不查 pin，首跑实测教训）→ fork：子 PTE **不同 pfn** + writable + exclusive + 内容==父页（copy_present_page 形态）；父 PTE 仍 writable、仍 exclusive（GUP-pin 契约）、mapcount 1（copy 路径不 wrprotect 不清 exclusive 的逐条对拍）；**双侧 meta flags == WRITABLE only（无 SHARED）**——§1.2 修复的直接可观测；双侧 inv7 walk violated==0 ∧ **checked==0**（无 SHARED 残留；修复前此处 checked==1/violated==1）；unpin 后双侧 teardown 零泄漏零 timeout |
| hugetlbfs 拒绝 | `corten_fault_test_file_may_matrix` 新臂（mm/corten_fault_test.c:592-606）+ `corten_arena_test_file_hugetlb_route`（arena_test.c:3566-3640） | 纯函数臂：FOP_HUGE_PAGES 的 f_op → -EOPNOTSUPP（fs 无关伪造，无需 hugetlbfs mount）；route 级：伪造 hugetlb 形 fd 经真路由 → ret 0（降级）、请求参数原样交还、零窗口占用（carrier_of NULL）、file_mmaps 不动；随后真文件照常拿窗（拒绝不耗资源）+ 计数器 +1 |

## 3. 两个真缺陷的诊断记录（对拍方法论价值）

1. **rss 族错配**（§1.3）：单进程 KUnit 里 B.3 的 FILEPAGES 记账自洽（装 +1/zap
   −1），只有 fork 把上游 copy_page_range 拉进账本才爆——"父子引用对账"用例设计
   的直接战果。KUnit 首跑的 `Bad rss-counter state` BUG 行是唯一线索。
2. **SHARED 残留漂移**（§1.2）：pinned+fork 组合在 M5 时代就存在（anon 区同样
   形态），B.4 把 inv7_walk 接进 pinned 用例才可见。修复顺带覆盖 DONTCOPY 片边界
   残留类（同谓词）。两修复互不依赖，分别被 checked==0 与 rss 差分断言锚死。

## 4. 对 B.3 已验证面的触碰清单（合规性）

红线"勿动 B.3 fetch/read/cow 主体"的触碰逐条：

| 位置 | 性质 | 判定 |
|---|---|---|
| read 臂/cow_write/zap_release_page 的计数**族**（3 行） | §1.3 缺陷修复：MM_FILEPAGES 常量 → mm_counter_file(folio)；臂的逻辑/锁序/收支零改动 | 任务书第 1 条"父子引用对账"的直接产出，非镀金 |
| read 臂/cow 两处 `atomic_long_inc`（观测计数） | 纯增量，落点在既有 fault_stat 调用旁 | H10 需求 |
| file_read/file_cow 的 filepg 族断言 | 测试侧随族更新（filepg_family 变量 + helper）；断言结构与基线值语义不变（shmem 下族名换） | §1.3 的必然跟随 |

fetch（双宿主取页）、read 臂锁循环、cow 事务序、truncate 门、even_cows、FRESH
合成——逐字未动。

## 5. 验证结果（无盘 qemu，命令形态照 mva2-verify.sh run_kunit）

日志目录：`/home/ppw/cortenmm/results/r07/mvb4/`

### 5.1 构建

- `make -j8`（=y）：exit 0，**零新增警告**（仅基座 objtool cpuidle + modpost
  memblock 两条，build-y-final.log）。
- =n（CORTEN_* 五项全关）：exit 0（build-n.log）；`nm vmlinux | grep -ci corten`
  = **0**；`ar t mm/built-in.a | grep -c corten` = **0**。.config 已恢复 =y 并
  终码重构建。

### 5.2 KUnit（bzImage + `corten=on kunit.filter_glob=corten*`）

| boot | corten | corten_arena | corten_fault | 备注 |
|---|---|---|---|---|
| on1（首验） | 24/0/1 | 70/**2**/0 | 31/0/2 | 两失败=§3 两个缺陷的可观测（mirror 的子 MM 计数 + "Bad rss" BUG 行；pinned 的 dup 未拒） |
| on2 | 24/0/1 | 74/**2**/0 | 31/0/2 | 缺陷修后：cfilepg−1 断言口径 + checked==0 断言口径（均为修复后语义的正确期望） |
| on3 | 24/0/1 | **76/0/0** | 31/0/2 | 首绿（arena 72→76 = 新四用例） |
| on4 / on5 | 24/0/1 | 76/0/0 | 31/0/2 | flake 复跑绿 ×2 |
| on6-final / on7-final | 24/0/1 | 76/0/0 | 31/0/2 | checkpatch 修后 + 注释 reflow 后终码（两次） |
| off1 / off2（corten=off） | 25/0/0 | 23/0/**53** | 7/0/26 | 零扰动（skip 49→53 = 新四用例按约 skip；file_may 的 hugetlb 臂在 off boot 照跑照绿——纯函数） |

- 全部 8 次 boot：lockdep/oops 签名 grep 零命中。
- **interlock flake 判定**：`corten_test_txn_uninstall_interlock` 全部 boot 通过。
- 新用例 ok 逐条确认（on3-on7）：file_fork_mirror / file_fork_cow / file_fork_pinned /
  file_hugetlb_route。
- 噪声核对："Bad rss-counter state"（MM_SWAPENTS −1）2 处 == mvb3 基线（inv7_swapped/
  fork_drain_leak 家族）；**本片引入的 fork_mirror 版 MM_FILEPAGES/SHMEMPAGES BUG
  行已消失**（§1.3 修复的直接证据）。"non-zero pgtables_bytes on freeing mm"：mvb4
  22 处 vs mvb3 on5 基线 15 处——mm_alloc 造的 fork 子 mm 无树内 VMA 可走 exit_mmap
  的释放（carrier 是脱树的），PT 页计数不归零，**B.1 起 fork 用例族的既有形态**
  （mvb3 基线 fork_faithful/fork_unwind 后同样打出），本片 4 个新 fork 用例各贡献
  1-2 行，登记为 harness 噪声非回归（A.2b 挂账，见 §7）。

### 5.3 checkpatch

`scripts/checkpatch.pl --strict --no-signoff --ignore FILE_PATH_CHANGES
r07-mvb4.diff` → **0 errors, 0 warnings, 0 checks**（1124 行；首版 4 个 WARNING
均为节横幅注释 `*/` 未独占行，已修）。

### 5.4 改动统计

git diff HEAD：**3 文件，+854/−26**——生产码 corten_arena.c +116/−16（≈ brief 的
~230 预算内），测试 +738/−10（arena 四用例 + 计数族 helper/断言族更新 + fault
matrix 臂）。测试超预算主因：pinned 用例必须整链（COW→pin→fork→双侧 PTE/meta/
inv7/ledger）才够格当"6.18 基座唯一可达的拷贝路径"锚。

## 6. guest 门风险点（主会话判据建议，按风险排序）

1. **真 fork 路径的 dup_mmap 全链**：KUnit 的 fork 走 begin/commit 钩子 + carrier
   对 copy_page_range，真 fork 还含 dup_mmap 的树内 VMA 循环/anon_vma 链克隆与
   fork_abort 竞态——JTB 的 fork 子进程写 .so 数据段（FILE_MAPPED 槽）首次真跑。
   判据：子进程 dlopen 库的 GOT/数据段重定位正确（= 父子 COW 隔离 + 文件内容对拍）。
2. **shmem 计数族变化的观测面**：/tmp（tmpfs）映射的 rss 从 RssFile 移到 RssShmem
   ——与 legacy 一致化，但依赖 RssFile 口径的监控脚本会看到漂移（预期内，REPORT.md
   V-B 章登记）。
3. **MMF_HAS_PINNED × 真实 GUP**：KUnit 模拟 pin（bias 引用 + 手工置位）；JTB 的
   Profiler/IO 真走 pin_user_pages——folio_needs_cow_for_dma 的 seqcount 断言在
   copy_page_range 内天然满足，但真 GUP 的 unpin 时序（fork 窗口内 pin 着）是首跑。
4. **pgtables_bytes 噪声**：guest 无 mm_alloc 假 mm，不受影响；若 guest 见到同款
   BUG 行则是真回归信号（区别于 KUnit harness 形态）。
5. JVM+CDS 关闭态、dlopen 繁重 checksum、截断-重读 strace/si_code diff、perf2a
   churn——brief B.4 段原判据，归主会话 guest 门。

## 7. 后续（V-B DoD 状态 + 挂账）

- brief §5.5 的 double_fault_race 锁外中点竞争注入维持 B.3 口径（无单线程手段，
  retry 预算代码路径 + guest 覆盖）。
- mm_alloc 子 mm 的 pgtables_bytes 残留（§5.2）：A.2b fork harness 家族问题
  （carrier 脱树使 exit_mmap 无 VMA 可走），非 FILE 专属；建议 A.3 审计批处理或
  harness 侧补 PT retirement 钩子，已登记不阻塞 V-B DoD。
- V-B spec §5 切片表 B 行四锚：B.1/B.2/B.3 已入库，本片（=y 全绿 + off 零扰动 +
  =n 折叠 + checkpatch 0/0/0）收口，待主会话 guest 门（§6）后 DoD 签结。
