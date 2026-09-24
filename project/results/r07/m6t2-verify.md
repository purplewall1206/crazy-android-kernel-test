# M6.T2 · swap out/in 事务 · 验证报告

日期: 2026-09-20 (通宵班, ~20:25 起)
基座: 主树 HEAD `0e469cd9d055` (M6.T1) → worktree `/home/ppw/linux-6.18-m6t2` 分支 `m6-t2`
**未 commit**（review/maintainer 后续）; 快照 diff = `patches/r07-m6t2.diff`（与 worktree
`git diff` 字节一致, 8 文件 +1817/−93）
构建件: worktree 验证构建 #24（普通内核, 配置已还原）; guest 镜像副本
`/home/ppw/vm/trixie-m6t2.img`, VM tmux `m6t2-vm` (port 10029) 留运行。

---

## 1. 落地内容（按合同 T2 五条）

1. **swap-out 事务（rmap true 臂）**
   - `corten_rmap_unmap_one()` 变为**纯预过滤**（+`bool migrate` 形参区分
     try_to_migrate_one——ttu flags 无法区分两 caller, migrate.c 传
     TTU_BATCH_FLUSH/0 与 unmap 相同）: 拒绝 TTU_HWPOISON / migration caller /
     VM_LOCKED(无 IGNORE_MLOCK) / DMA-pinned / order!=0 / **无 swap entry**
     （reclaim 无 entry 会毁内容——T1 语义保留）。全部计数 rmap_rejects。
   - 新完成臂 `corten_rmap_swap_out(folio, vma, addr, defer, &old_pte)`（mm/corten_arena.c）:
     由 try_to_unmap_one 在**其 mmu_notifier 窗口内**调用（R6-2; 预过滤仍在窗口前——
     拒绝形状零写入零 notifier 流量）。事务 = `corten_lock_range > ptl`（DEV-13 同向）,
     逐位镜像上游 rmap.c:2120-2201 的 swap-install 链（swap_duplicate →
     arch_unmap_one → folio_try_share_anon_rmap_pte → mmlist → ANONPAGES−1/
     SWAPENTS+1 → exclusive/soft-dirty/uffd-wp 编码 → set_pte_at）+ 同临界区
     `corten_swap_out()` 元数据（MAPPED→SWAPPED, perm 保留, COW flags 清零,
     entry 编码进 __resv）。**该 set_pte_at = corten_glue_pte_write 白名单第 2 处**
     （DESIGN §3 已登记, ≤3 已用 3: ②ttu 换出 ③unuse_pte）。
   - R6-1 defer: 事务内 defer=true 时跳过 flush_tlb_range, 由 rmap.c 在事务返回后
     `set_tlb_ubc_flush_pending()`（rmap.c static; 事后登记=clear-after 的 happens-after
     强化, 两种 flush 均按地址, 中间安装的 swap PTE 只被事务持有的 ptl 读取）。
     非 defer 形状 flush_tlb_range 在 get_and_clear 后（上游同位）。
   - abort 路径全部 `set_pte_at` 原样恢复 + 计数, folio 驻留（T1 姿态）。
2. **swap-in 反向（D5, 全事务形态——论证**）: dispatch 新增 `CORTEN_DISP_SWAPIN`
   （纯分类器）; 处理器 `corten_arena_swap_in()` **自持锁循环**: 释放 covering 锁
   （I/O 睡眠, INV3 禁止锁内睡）→ 直读页回 → 重锁 → re-query（entry 变化/zap/他 fault
   一律 −EAGAIN 走 fault_once 重派, 图 7; 计数 swapin_retries）→ 单事务内 PTE+rmap+
   计数+corten_map(Swapped→Mapped) 提交 → 解锁后 swap_free。
   **拒绝"放行 legacy + 事后同步"方案的理由**: ①legacy do_swap_page 裸写 PTE 需要在
   handle_mm_fault 中段新增 glue 点; ②meta=Swapped 落后于 legacy PTE 安装, 若 legacy
   走 do_anonymous_page 分支（PTE none 形状）= 换出内容上盖零页的静默数据丢失; 全事务
   形态在**每个锁边界**都保持 meta/PTE 一致对（PS-B2）, 由构造保证 INV7。
   **换入 folio 来源 = do_swap_page 的 SWP_SYNCHRONOUS_IO 直读形态**（zram 即同步设备,
   全量回收后 __swap_count==1 恰为常态）: `vma_alloc_folio + mem_cgroup_swapin_charge_folio
   + swapcache_prepare 序列化 + memcg1_swapin + swap_read_folio`。
   **不用 swap-cache/readahead 形态的硬理由**: `__read_swap_cache_async()` 把新 folio
   `folio_add_lru()`（swap_state.c:498）——arena 页 DEV-10 红线（永不上 LRU）+ 回收守卫
   拒绝使其成为"回收不可见但 MGLRU 仍在 aging"的僵尸（首次 guest 跑的 lru_gen/freelist
   毒化 panic 即此, 见 §5 缺陷修复）。独占判定: 直读 folio 必独占（fresh, 上游同语）;
   非独占读形状不存在（无 cache 命中路径）。**不 folio_add_lru**（DEV-10, 与 map_anon
   同款省略）。exclusive=假 时 meta 重铸 SHARED(+WRITABLE)（M5 COW 事务接管后续写）
   ——本形态恒 exclusive, 该分支为护栏。**不 folio_put 于成功路径**: 分配引用即 PTE
   引用（do_swap_page 记账 nr_pages−1==0; 第一次 guest 跑的 freelist 毒化 panic 即
   此处多放一次）。
   **换入自愈臂**: meta=Swapped 而 PTE present 且 pfn==回读页（swapoff unuse_pte 竞态
   形状）→ 只修 metadata（写方已搬计数/rmap）→ 计数 swapin_heals, −EAGAIN 重派。
3. **消费面扩展（D7）**
   - `corten_arena_zap_window()`: 非 present 非 none 的 swap PTE 分支 →
     `free_swap_and_cache()`（上游原语持有 entry 计数对称性, R6-4; 同时回收不再共享的
     cache copy）+ SWAPENTS−1 + zap_swap_frees 计数; 旧"M3 无 swap entry"注记删除;
     non_swap_entry（migration/hwpoison 标记, 守卫拒绝形状不可达）→ WARN。
   - `corten_arena_protect_window()`: recorded 白名单 + CORTEN_SWAPPED——perm 改写纯
     metadata（swap PTE 无硬件 perm 位）; pending perm 于换入时经 m2.perm 生效
     （mk_pte 用 corten_arena_perm_pgprot）。!present 分支注记更新。
   - fork: `corten_swap_out` 新 op（include/linux/corten.h, mm/corten.c; 唯一合法迁移
     MAPPED→SWAPPED, SHARED/flags 拒绝）; fork_mirror 的 default 分支整槽拷贝 __resv
     ✓（child 与 parent 各持 swap PTE, copy_nonpresent_pte 处理 exclusive 位分裂）。
   - swapoff（P12）: `unuse_pte()` 挂 `corten_swapin_sync_meta()`（glue 白名单第 3 处;
     meta 先行、在 unuse 的 ptl 之前取 desc 写锁, DEV-13 同向; 中毒形状跳过同步——
     swap-in 路径将其重派进 legacy 的 poison 应答）。
   - GUP: fast 天然跳过 !present; slow→fault 门→SWAPIN（无改动, spec D7 预期形状）。
4. **INV7 扩展**: `corten_arena_test_inv7_walk()` 增 Swapped 半边——
   `meta==SWAPPED ⇔ PTE 非 present 非 none 且 pte_to_swp_entry==__resv 解码`;
   none-PTE-behind-Swapped = 丢内容形状记违例（spec D6 双向断言）。
5. **T2 最小 shrink 桩**: debugfs `/sys/kernel/debug/corten/evict`（0200,
   写 "<pid> <nr>"）→ `corten_arena_evict_pid()` → 逐 arena 逐窗拾取
   （MAPPED+!SHARED+!pinned+present）→ **`__reclaim_pages(&list, private)`**:
   复用上游 madvise-PAGEOUT 机制（folio_alloc_swap → try_to_unmap → 守卫事务 →
   swap_writeout → __remove_mapping/memcg1_swapout/put_swap_folio 全在上游）;
   private 非 NULL 绕过 `folio_putback_lru()`（red line 7: kept folio 绝不入 LRU,
   由驱动自持引用收尾）; frame 级短 RCU 段 + cond_resched（首轮 guest 跑的
   RCU GP 饥饿修复）; `arena->frozen` 门镜像 lookup_get（DEV-15 fork 快照窗静态性）。
6. **计数**: swapped_out / swapins / swapin_retries / swapin_heals / zap_swap_frees
   （arena_stats 渲染）; rmap_rejects 语义重定义为"逐形状拒绝计数"（原 tripwire
   语义随 T2 完成臂失效, 注记更新）。

## 2. 验证矩阵

- **=y 全量构建**: 零新增警告（全 log 唯一 warning = 既有 cpuidle objtool 条目,
  基线同在）; 最终恢复配置重编 #24 通过。
- **KUnit**（无盘 qemu, `kunit.filter_glob=corten*`）:
  - corten=on ×2（最终内核）: 24/0/1 + 44/0/0 + 30/0/2 全绿;
    另一次 run 出现 `corten_test_txn_uninstall_interlock` violations==1（23/0/1）——
    **既有 M7 登记 flake**（同轮基线内核 4 跑亦 1 次同签名失败; 机理 = 该 20s CPU 摆放
    压测的协调线程遭 host vCPU 饥饿, 协议代码与本 diff 无交集; 复跑即绿）。
  - corten=off ×1: 25/0/0 + 18/0/26 + 6/0/26 全绿（新增用例按设计 skip）。
  - **lockdep 变体（最终内核, PROVE_LOCKING）**: 24/0/1 + 44/0/0 + 30/0/2 全绿,
    零 lockdep/oops 签名。
  - 新用例: fault 套件 +4（swap_encode 纯 roundtrip / swap_roundtrip[无 swap skip,
    guest 真盘跑] / swap_zap_free[同] / swap_mprotect_pending——合成 entry 位操作,
    kunit action 清理保证 abort 安全）; arena 套件 +1（inv7_swapped: 健全形状 0 违例
    + 破坏形状恰 1 违例）; dispatch 表 swapped 行 STUB→SWAPIN。
- **=n（CORTEN_MM=n）八对象**: memory/mmap/migrate/rmap/swapfile/gup/oom_kill/
  arch(x86)fault 零错误零警告。
- **checkpatch --strict（8 改动文件）**: 新增 0E/0W（rmap 12W/swapfile 17W/
  arena 1W 均为基线既有计数, 逐文件对账通过）。

## 3. Guest 判据（M6 验收核心）—— `results/r07/m6t2-guest/`

环境: trixie VM 4G/8vCPU KVM, zram 2G lz4 prio100, cgroup memory.max=512M,
arena_stress 4 线程 512M mixed seed42（自动 DECLARE, 数据 magic 校验）。
脚本 `bench/share/m6t2-swap-test.sh`, 最终跑 `t2run-final.log`: **fails=0**。

- **arena 页真实进 zram**: evict（SIGSTOP 冻结后采样）→ **swapped_out=69164**
  （arena_stats）, zram mm_stat orig=276MB/used≈3.2MB, **RSS 276MB→1.8MB**,
  `/proc/pid/smaps` Swap=276656kB SwapPss 同值。zramctl USE 同步增长。
- **读回校验和一致**: SIGCONT 后工具 90s 跑完 `errors=0 op_errors=0 arena_ok=true`
  （mixed 模式对 live 页持续 magic 校验）, checksum 跨跑格式一致; **swapins=69164**
  与换出精确相等。
- **退出零泄漏（swap entry 全释放）**: 账目闭合 **69164 = 69164 + 0**（swapins +
  zap_swap_frees）; 退出后 zram used 回基线 20480 页; MM_SWAPENTS/mm_stat 对账;
  这轮 zap_swap_frees=0 属期望（换出页全部被读回, 无 chunk 丢弃）; 更早一轮
  跑出 63133 = 63122 + 11 同样闭合。
- **非 arena 进程 swap 行为不变**: legacy anon hog（700MB 写+读回校验）rc=0;
  dedup_eq(8thr/20s/glibc) rc=0、psearchy_eq(4thr/64M) rc=0 checksum 一致、
  JThreadBench(500thr×3 JVM) rc=0（639ms 中位, 量级正常）。
- **OOM 回归（swap PTE 生存期内的守卫）**: memory.max=256M 压 arena_stress →
  干净 SIGKILL, **audit=0（零 WARNING/BUG/Oops）, rmap_rejects=0, reap_skips=0**——
  T1 守卫姿态在 swap PTE 存在下不变。
- **run_mode_smoke**: 24/26 PASS; `released-arena-unmapped/gone` 2 例失败
  **在 T1 基线内核（5c545359e856）同环境逐字复现**（对照跑已存档）→ 既有问题
  （疑似共享 smoke 二进制与 T1 后语义漂移）, 非 T2 回归, 已登记。

## 4. 缺陷修复（本轮guest 驱动的真 bug, 全部带根因）

1. **evict 拾取 RCU 饥饿**（首轮 guest）: 全 arena 扫描（512M=256 窗）在单一
   `rcu_read_lock()`（=preempt off）内 → rcu_preempt GP 饥饿 636s + 多 CPU
   lruvec/IPI 自旋堆叠。修 = frame 级短 RCU 段 + cond_resched。
2. **frame 别名重复拾取**（二轮 guest）: arena 描述符被 256 个 frame 别名,
   每 frame 全量重扫同一 arena → 同一 folio 重复 list_add → 拾取链表成环 →
   `__reclaim_pages` 节点分桶 100% CPU 死循环（9 分钟零进度; nokaslr+vmlinux
   符号化定位于 +0x96 节点循环）。修 = 每 frame 只扫本 2M 窗。
3. **swap-in 多余 folio_put**（三轮 guest panic）: cache 形态换入在成功路径
   `folio_put` 多放一次（分配/lookup 引用即 PTE 引用）→ 提前释放仍映射的 folio →
   `free_pages_and_swap_cache` 路径 freelist 毒化（LIST_POISON2, ___rmqueue_pcplist
   GPF + clear_page_erms 二次 GPF panic）。修 = 成功路径不放（do_swap_page 记账）。
4. **readahead 缓存形态与 DEV-10 冲突**（结构性, 上述 1-3 的共同土壤）:
   `__read_swap_cache_async` 的 `folio_add_lru()`（swap_state.c:498）使 arena 换入页
   进入 LRU pending 批 → MGLRU aging 与回收守卫拒绝对抗 → lru_gen_del 毒化。
   修 = 换入改 SWP_SYNCHRONOUS 直读形态（无 cache 插入、无 LRU、独占恒真、
   free-try 块整体消失）。§1.2 已述。
5. 测试侧（KUnit, 首轮 on-boot 即暴露）: 合成 Swapped 形状用例的
   `struct corten_pte_meta` 未清零（flags 垃圾 → corten_swap_out −EINVAL →
   ASSERT 在 desc 写锁下中止 = 锁泄漏 → 收尾自死锁）; 事务区内 ASSERT 改 EXPECT;
   mprotect-pending 用例缺 mode_enter（−95）+ kunit action 注册顺序（LIFO）
   导致清理跑在 mm 销毁后（ft_pte GPF）。

## 5. 遗留 / 登记

- **R6-1 defer 深化**: defer 形状已按上游 bookkeeping 打通, 但大 IPI 域下的
  吞吐未调（T3/T5 性能化范围）; 首 guest 跑的 65536 页 evict ≈ 分钟级
  （zram 同步写逐页 + 逐页事务）, T3 shrinker 批量化（2M 窗一次 desc 锁摊 N 页,
  spec §2.2）是正确杠杆。
- **P12 swapoff**: 同步钩子 + 自愈臂落地, 但 guest 未跑真 swapoff 流程
  （kunit 无设备、脚本无 swapoff 用例）——机制级覆盖（KUnit heal 分支未单测,
  swapin_heals 计数在册）, 建议 T5 矩阵补 swapoff 注入。
- **OQ-M6-2 维持**: SHARED 页不换出（guard 拒绝, 计数可见）; fork 后换出需
  COW/swapin 合流, Stage3。
- **evict 驱动为 T2 桩**: pid 寻址（无 mm registry, T3）、串行单写者、
  nr 上限 INT_MAX; kswapd/MGLRU 对 arena 页仍结构性不可达（无 LRU + 无 shrinker）,
  压力通道 = T3 shrinker。
- **swapin_retries/swapin_heals 本轮 guest 均为 0**——竞态臂未被自然激发;
  KUnit heal 用例覆盖自愈逻辑（inv7_swapped 的破坏形状 + mprotect-pending 的
  合成形状）, 真实竞态依赖并发换出/换入注入, 建议 T5 并存矩阵。
- **run_mode_smoke 2 例既有失败**（released-arena-unmapped/gone）+ smoke 二进制
  一次性 stack-smash abort（flaky, 复跑消失）——基线内核复现, 归 T1/工具面,
  非本片; 维护者裁决归属。
- **interlock flake 失败率**: 本内核 3/6, 基线 1/4——同签名（20s 死等级联）,
  判定为 host 负载敏感的既有 M7 项; 若维护者认为比率差异超噪声, 建议在
  静默宿主上做 10× 对照（本轮未做, 时间盒）。

## 6. 产物清单

- `patches/r07-m6t2.diff`（快照 diff, 8 文件 +1817/−93; 未 commit）
- `results/r07/m6t2-kunit-{on1,on2,off1,lockdep}-{,.final}.log`（无盘 KUnit;
  on3/on4/off2/lockdep-final = 最终内核）
- `results/r07/m6t2-guest/{t2run-final.log,swap-test*.log,smoke.log,oom-test.log,
  guest-boot.log}`（最终判定 = t2run-final.log fails=0）
- `docs/DESIGN.md` §3 白名单账更新（≤3 已用 3: ②ttu 换出 ③unuse_pte）
- 验证内核: worktree 构建计数 #24（普通）/ lockdep 变体各一轮
