# W-3 known-open 复核：窗口 futex 与换出边界（2026-09-26 修复班）

W-3 提交信息引用了本报告但未落盘；本文件补齐并按 2026-09-26 的复核更新结论。

## 1. metis_eq abort 的真实根因（已修）

W-3 提交时的归因"window futex EFAULT at the swapout boundary"是**部分错误**的。
2026-09-26 复核（strace + kprobe 实证链）：

1. metis_eq 在 W-3 终镜像上 100% Abort（rc=134），glibc 报 "The futex facility
   returned an unexpected error code"；当时 `swapped_out == 0` —— **无换出发生**，
   与换出边界无关。
2. strace 定位失败调用：`futex(0x100001000990, FUTEX_WAIT_BITSET|FUTEX_CLOCK_
   REALTIME, tid, NULL, ...) = -1 EFAULT`；mmap 布局显示该地址落在 glibc
   线程栈映射（`mmap(NULL, 8392704, PROT_NONE, MAP_STACK)` @ 窗口 + mprotect
   顶部 RW）内 —— pthread_join 在栈顶 struct pthread 的 TID futex 上等待。
3. kprobe（get_futex_key 入口/返回）实证：返回 `0xfffffff2`(-EFAULT)，调用方
   futex_wait_setup 与 futex_wake 双双命中；`gup_probe_rejects` 每次复现 +4
   —— **失败点在 corten_gup_window 的 check_vma_flags 仿真**，不在 futex 侧。
4. 根因：CHUNK mprotect 只把新 perm 提交进**槽位元数据**（pending-perm 契约），
   `ar->prot` 留在 DECLARE 界（仅整域 EXACT 重写才移动它）；GUP 探针只读
   `ar->prot` → 对 STACK region 的 FOLL_WRITE 一律 -EFAULT 拒绝。
5. 修复：探针对齐 FRESH fault 门的同一规则（`m.perm ? m.perm : ar->prot`），
   经 `corten_ptdesc_get` RCU 钉住描述符读槽位。读稳定性：perm 生产者
   （mprotect/madvise/munmap 路由）持本 mm 的 mmap_write，GUP 臂持 mmap_read
   —— 同 mm 互斥；换出驱动的槽位跃迁 perm 不变，follow/fault 腿仍是终判。
   KUnit 锚 `corten_arena_test_w3_gup_chunk_promoted_perm`（修复前红、修复后绿）。
6. 修后：metis_eq 双跑 rc=0、checksum 自洽且与无 hook 跑同值（零改动契约）；
   guest 电池 22/1（唯一 FAIL=登记的 carrier 计数退役容差）。

## 2. 换出边界形状（known-open 维持，且升级）

对真换出形状的新探针（w45/swapfault.c：窗口页 touch → debugfs evict → 读回）：

- evict 后 `swapped_out` 计数上涨、`swapins == 0`，读回进程**静默死亡**
  （bash 报 Segmentation fault，内核无 segfault 日志行，SIGSEGV handler 无输出）。
- 即：**换出后的窗口页，用户态 fault 既不换入也不给出干净裁决** —— 比 W-3
  登记的"futex EFAULT"更重（数据完整性级）。
- 既有佐证：W1.f 起各片 guest 门的 **S-3（swapoff 全量读回）持续 rc=1**，
  与本形状同族；W1.e2 时代的 driver_swapped=131072 读回完好（当时通过），
  回归窗口 = W1.f/f2 → W-3 之间，未精确二分。
- 处置：登记为 **W-6 硬阻断项**（终判据电池含 S-3 与换入路径）；修复建议
  从 W1.f2 的条目编码（type-u8+le32-offset+byte5=0）与换入事务的解码侧入手，
  kprobe `corten_arena_swap_in` 是否被 dispatch 触达为第一分叉点。

## 3. 顺带修复（同批）：exit walk 终段帧退休

复核期间发现的第二个真缺陷（pgtleak 探针族，每 MODE 退出一条 4096/8192
pgtables_bytes BUG）：exit walk phase A 终段 run 以 `arena->end`（字节粒度）
收尾，`free_ptes_span` 的整帧守卫跳过末帧 → PTE 页搁浅（heap region 跨帧
形状与所有非帧对齐 end 的窗口）。修复：zap 保持记录跨度，退休伸展到帧界
（末帧已验证 walkable=无树 VMA 且注册表槽位指向本 arena）。KUnit 锚扩展
`corten_arena_test_brk_region_exit`（fill 末帧 + mm_exit 后 pmd 条目必须
非 present）。修后全部探针形态（heap-only/auto 窗口/混合）dmesg 泄漏行=0。

## 4. 遗留登记（非本批）

- 窗口内 MAP_FIXED（植入形状）退出时其 pud/pmd 上层页 legacy 无法释放
  （free_pgd_range 的对齐守卫）→ 8192 残差 —— **W-5 的直接靶面**（植入消灭
  后形状不复存在），本批不修。
