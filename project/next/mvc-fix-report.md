# M-V V-C 两缺陷修复报告（r07-mvc）

worktree: /home/ppw/linux-6.18-mvb（分支 mv-b = f4ffec5e0005 + V-C 未提交增量 + 本修复）
日期: 2026-09-23　状态: 内核侧+workload 侧已修，KUnit 全绿；guest oracle 复验归主会话

---

## 缺陷 1（内核真 bug）: /proc/<pid>/maps 双源渲染丢首个窗口行

### 现象

oracle 负载（6 个窗口 region：8M RW@0x100000000000 / 2M RW / 2M PROT_NONE /
2M file / 2M park(已 munmap) / 2M magic@回收帧）实测 maps 只渲染 3 个 anon 行
（2M rw@800000、2M ---@a00000、2M rw@e00000）——地址最低的 8M RW region
（恰好是 maple 树最后一条委托 VMA 之后的第一条窗口 region）整行缺失，且每个
已渲染行显示的都是"下一个 region"的跨度。

### 根因：m_start/proc_get_vma 归并游标发射路径的三处复合缺陷

全部集中在 fs/proc/task_mmu.c 的 proc_get_vma 行胜出分支（及 corten_maps_prime）。
oracle 的地址布局：委托树 VMA 在窗口域（16T–64T）下方（exe/heap）与上方
（vvar/vdso/stack @~140T），窗口行夹在中间。

**(1) 渲染 off-by-one（丢首行的直接根因）。** 行胜出分支把要发射的行拷到局部
`produced`，先把 `priv->corten_row` 推进为 lookahead 的**下一行**，再返回
carrier；而 show_map/show_smap/show_numa_map 渲染的是 `priv->corten_row` ——
即渲染"下一行"的边界。于是：

- 首行（produced=rw8）从未被渲染——它的跨度只存在于局部 `produced` 里；
- 每个已渲染行错位成下一 region 的 start/end（@800000/@a00000/@e00000 全部
  是"下一行"的跨度）；
- 最后一行渲染两次（流耗尽时 `corten_row_next` 返回 false 不改写
  corten_row，残留的最后一行被再次打印）；
- **单 region 时不可见**：false 返回留下 prime 写入的同一行，渲染恰好正确
  ——这正是 mva1 探针（单区域）通过、oracle（多区域）必丢首行的原因。

**(2) 行胜出吞树头（委托行整行消失）。** 判定前 `get_next_vma()` 已经用
`vma_next()` 推进了 maple 迭代器；行赢时刚取出的树 VMA（oracle 里依次是
vvar、vdso、stack——第一批窗口域之上的委托行）被直接丢弃，迭代器已越过，
该委托行从输出中消失。

**(3) last_pos 提前推进（seq 重填时跳行）。** 行分支写
`priv->last_pos = produced.end`（本行自己的 end），破坏了 vanilla 语义
（last_pos = 进入时 *ppos = **上一条已渲染记录**的 end）。seq_file 的 Fill
循环每轮无条件多预取一条记录；按字节预算切断时，下轮 m_start 用 last_pos
回卷重取。被预取而未渲染的条目若是窗口行，回卷点已被 (3) 推到该行自身
end 之后——该行被永久跳过（maps ~1.3KB 单轮读不触发；smaps >4KB 必触发）。

### 修复（只动游标发射路径，V-C 已验证主体不动）

`fs/proc/internal.h`：`proc_maps_private` 增加 `corten_row_peek`（lookahead
头）；`corten_row` 语义收窄为"当前条目正在渲染的行"。

`fs/proc/task_mmu.c` `proc_get_vma` 行胜出分支（修复后）：

```c
	if (priv->corten_row_valid &&
	    (!vma || priv->corten_row_peek.start < vma->vm_start)) {
		priv->corten_row = priv->corten_row_peek;          /* promote: .show 渲染这行 */
		priv->corten_row_valid =
			corten_row_next(mm, &priv->corten_rows,
					&priv->corten_row_peek);   /* 只推进 lookahead */
		priv->corten_row_active = true;
		priv->last_pos = *ppos;                            /* vanilla 回卷不变量 */
		*ppos = priv->corten_row.end;
		vma_iter_set(&priv->iter, priv->corten_row.end);   /* 归还被挤掉的树头 */
		return priv->corten_row.ar->carrier;
	}
```

`corten_maps_prime` 改写 `corten_row_peek`（去重规则不变：产出第一条
end > pos 的行）。三处缺陷一对一消除：promote-then-peek 修 (1)；
vma_iter_set 修 (2)；last_pos=进入 *ppos 修 (3)。

### 游标时序图（oracle 场景，窗口行夹在 heap 与 vvar 之间）

修复前（丢首行 + 吞树头 + 错位渲染，R1=rw8、R2=rw2、R3=none、R4=file、R5=magic）：

```
proc_get_vma            tree iter        corten_row(渲染源)    输出
--------------------    --------------   ------------------   ----------------------
fetch vvar → row wins   vvar 被越过 ✗    produced=R1(丢弃) ✗   [渲染 R2 跨度]@800000
fetch vdso → row wins   vdso 被越过 ✗    produced=R2           [渲染 R3 跨度]@a00000
fetch stack→ row wins   stack 被越过 ✗    produced=R3           [渲染 R4 跨度]@c00000(file)
fetch NULL → row wins   (树已耗尽)        produced=R4           [渲染 R5 跨度]@e00000
fetch NULL → row wins                     produced=R5           [渲染残留 R5]@e00000 ✗
fetch NULL → tree path  SENTINEL          —                     [vsyscall]  (vvar/vdso/stack 全丢 ✗)
```

修复后：

```
proc_get_vma            tree iter            corten_row/peek       输出
--------------------    ------------------   -------------------   ----------------------
heap 之前全部委托行      逐条产出             peek=R1 保持          [exe 段][heap] 等委托行
fetch vvar → R1 wins    iter 重钉到 R1.end    row=R1, peek→R2       [R1] 16T-16T+8M ✓ 首行
fetch vvar → R2 wins    iter 重钉到 R2.end    row=R2, peek→R3       [R2] @800000 ✓
fetch vvar → R3 wins    iter 重钉到 R3.end    row=R3, peek→R4       [R3] @a00000 ✓
fetch vvar → R4 wins    iter 重钉到 R4.end    row=R4, peek→R5       [R4] @c00000 file ✓
fetch vvar → R5 wins    iter 重钉到 R5.end    row=R5, peek→尽(残)    [R5] @e00000 ✓ 无重复
fetch vvar → tree path  vvar 正常产出         valid=false           [vvar][vdso][stack] ✓
fetch NULL              SENTINEL+gate                              [vsyscall]
seq 重填(如 smaps)      m_start 回卷 last_pos=上一已渲染条目 end    prime 产出该待发行 ✓
```

### KUnit 归并序锚（新增用例）

`mm/corten_arena_test.c` 新增 `corten_arena_test_mvc_merge_first_row`
（列于 corten_arena_test_mvc_row_stream 之后）：mm 内造一条**委托域 VMA 结束
于窗口基址**（CORTEN_MODE_WINDOW_START-2M .. WIN，紧邻第一个窗口 region），
第一个窗口 region 4 帧（oracle 的 8M RW 形状）+ 第二 region 1 帧，锚定游标
依赖的三条流契约：

1. 全新 prime(pos=0) 头部必须是**第一个**窗口 region 的行；
2. 用 proc_get_vma 的精确纪律驱动双流归并（树头取出、行 start 更小则赢、
   行发射时 vma_iter_set 归还树头），产出恰为 [基址 VMA, 委托 VMA, R1, R2]，
   无重复无遗漏（吞树头/错位推进在此形状下即失败）；
3. 重启/回卷去重（corten_maps_prime 同形）：在委托 VMA end（= 首 region
   start，正是 oracle 丢 8M 行的回卷位）re-prime 必须复现 R1；每行 end
   re-prime 恰产出剩余后缀；流尽处 false。

---

## 缺陷 2（接口错配）: PROCMAP_QUERY ioctl ENOTTY

### 根因

workload 本地拷贝与内核真源（include/uapi/linux/fs.h）三重不匹配，cmd 值
完全不同，procfs_procmap_ioctl 的 `case PROCMAP_QUERY` 不命中 → ENOIOCTLCMD
（用户态见 ENOTTY）：

| | 内核真源 | workload 本地（错） |
|---|---|---|
| cmd | `_IOWR('f', 17, struct procmap_query)` = 0xC0686611 | `_IOC(_IOC_NONE, 'P', 1, sizeof(pmq))` |
| 方向/魔数/nr | _IOWR / 'f' / 17 | _IOC_NONE / 'P' / 1 |
| 结构 | size 字段；vma_name_addr/build_id_addr 为 __u64@88/96；104B | usize 字段；两者为 __u32@84/88 + _tail[4]；112B |

内核侧 `fs/proc/task_mmu.c` 的 dispatch 与 uapi 宏一致（`case PROCMAP_QUERY` →
do_procmap_query，size 校验 copy_struct_from_user）——**内核无需改**，错全在
workload 侧。

### 修法

`/home/ppw/bench/share/mvc-oracle/mvc_j3_workload.c`：本地结构逐字段对齐
uapi（`size`/`query_flags`/…/`vma_name_addr`(__u64)/`build_id_addr`(__u64)，
104B），宏改 `_IOWR('f', 17, struct procmap_query)`，调用点 `.usize` →
`.size`。`gcc -static -O2 -Wall -Wextra` 重编译零告警，产物已放回
share/mvc-oracle/mvc_j3_workload。

---

## 附带：run_mvc_oracle.sh 三处 bash 移植复核

1. **排序检查**：`sort -uC` 对十六进制按字典序排（混合长度必错：降序对
   "100000000000"→"800000" 它假通过）→ 改纯 bash `$((16#start))` 非降循环；
   bash 算术是有符号 64 位，[vsyscall] 行（最高位置位）读为负 → 按无符号
   语义比较（低半 < 高半，半内比较有符号）。
2. **2M rw 计数**：移植时丢了长度过滤（8M rw8 也被计入 → 恒 3 触发假
   park 告警，已实测）→ 恢复"start/end 差恰 2M"（$((16#e)-16#s) = 2<<20）
   且 perms 恰为 `rw-p`；合成 maps 上计数=2 正确。
3. **audit 判据**：j1_probes 保持登记残差非硬线（已对齐），去掉重复的
   hits/violations fail 行，硬线不变。

---

## 验证

- `make -j8` exit 0（含修复后全量）。
- 无盘 qemu KUnit `kunit.filter_glob=corten*` 三套件：
  - corten=on（终件 bzImage #39，kunit-final.log）：corten 24/0/1、
    **corten_arena 94/0/0（含新用例 ok 52）**、corten_fault 31/0/2，零 not-ok
    （另 on2.log 同形全绿；on1.log 为新用例断言校正常宽前的 1 fail 过程记录）；
  - plain ×1：25/0/0、23/0/71、7/0/26（新用例按设计 SKIP），零 not-ok。
- =n 折叠：defconfig（CORTEN 关）下 task_mmu.o/base.o/memory.o/gup.o 全
  RC=0，task_mmu.o 零 corten 符号；.config 已复原（=/y 全开）。
- checkpatch --strict 全量 diff（git diff HEAD，2089 行）：**0 errors /
  0 warnings** / 3 checks（3 处均为 V-C 已验证主体既有文本：corten_arena.c
  两处 ptl 注释 CHECK + corten_arena_test.c mvc_attach 既有对齐 CHECK，
  红线不许动，非本修复增量）。导出：/home/ppw/cortenmm/patches/r07-mvc.diff。
- 未 commit；INV6 路径未触碰；V-C 主体（mm/corten_arena.c 等）零改动。

## 待主会话（guest 复验）

- 重跑 `bash /mnt/hostshare/mvc-oracle/run_mvc_oracle.sh`：预期 maps 5 窗口行
  全渲染（8M rw@0、2M rw@800000、2M ---@a00000、file@c00000、2M rw@e00000）
  + park 不渲染 + 委托行（exe/heap/vvar/vdso/stack）全在且有序；
  procmap-first 输出 start/end/flags（ENOTTY 消除）；
- cmp_j3.sh 跨引导字节对比。
