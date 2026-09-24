# M2 DoD — 冒烟 2: corten=on — VERDICT: FAIL (确定性早期启动挂死)

- 日期: 2026-09-13 07:35–07:49 CST (r01 夜)
- bzImage / append: 同 off 冒烟, 唯一差异 = append 追加 `corten=on`
- 串口日志: boot-m2-on-console.log (空 — 见下); 现场证据: m2-on-panic.log

## 现象

两次独立启动 (重试 1 次以排除偶发) 完全一致的挂死:

- 串口 **零输出** (连 `[ 0.000000] Linux version` 都没有): 挂点在 console_init
  刷 printk 缓冲之前, 故无 oops 文本可存 (m2-on-panic.log 以 qemu monitor 取证代替)
- qemu 进程 (KVM) 单 vCPU **100% CPU** 持续, guest 卡死在内核态 CPL=0
- monitor 取证: `VM status: running`; RIP (KASLR) `ffffffffab25d322` →
  `ffffffffab25d33a` (爬行/小环), 反汇编见 nop sled + shl/or/mov/sub/cmp
- 对照组: **同一 bzImage 同一镜像文件** off 路径两次启动均 ~25s 到登录

## 逐项结果

| # | 检查项 | 结果 | 说明 |
|---|--------|------|------|
| 1 | 启动到登录 ≤180s | **FAIL** | 2/2 次 100s+ 无任何串口输出, 100% vCPU 挂死 |
| 2 | dmesg corten/kunit (KUnit 16/16) | N/A(未达) | 永远到不了 console init; KUnit autorun 的 16/16 证据已在 off 启动实证 (m2-dod-off.md) |
| 3 | debugfs dump/txn 可读 | N/A(未达) | 同上 |
| 4 | mmbench mmap-pf low 2 / unmap high 4 / pf high 8 | N/A(未达) | 同上 |
| 5 | /proc/<pid>/maps 正常 | N/A(未达) | 同上 |

## 处置 (按预案)

- 证据落盘: m2-on-panic.log (monitor RIP/反汇编/复现记录), boot-m2-on-console.log (空串口留档)
- relaunch 不带 corten=on 复验 off 路径 → PASS (~25s), VM 常驻 off 状态
- **不修代码, VERDICT = FAIL 并停**

## 假设 (供 M3 修复方向参考, 未动代码验证)

`corten=` 由 `__setup("corten=", corten_setup_param)` 处理 (mm/corten.c),
在 start_kernel 早期 parse_args 阶段即翻转 `corten_enabled_key`; 而唯一外挂 hook
`corten_on_pte_free()` (include/asm-generic/pgalloc.h:127, pte_free 路径) 由此在
mm 尚未初始化完成时就对早期 pte_free 生效 → 挂死/活锁。修复方向 (M3): 把启用点
推迟到 mm 就绪之后 (late_initcall/或首次 arena 注册时再 enable), 或让 hook 对
early-boot 页表安全。
