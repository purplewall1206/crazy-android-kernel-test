# CortenMM → Linux 6.18 移植进展

SOSP'25 CortenMM（消除 VMA 软件层抽象，页表页级事务接口）移植进 android17-6.18 内核树的项目进展仓库。

## 目录
- `log/` — 每夜报告（人类可读的内核更新轨迹）
- `patches/` — 内核补丁序列（format-patch 产物，与主树 tag 一一对应）
- `baseline/` — M1 性能基线（论文同款微基准/lat_proc/JVM/应用等价件，全部 ≥3 次取中位 + perfetto trace）
- `M3B_DESIGN.md` — M3b arena fault 路径集成设计（r2，含 reviewer 修正）
- `ARM64_PORTING.md` — M9 ARM64 移植性设计（504 行，107 处源码引用）
- `STATE-snapshot-*.md` — 项目状态快照

## 内核提交（/home/ppw/linux-6.18, 分支 android17-6.18）
| tag | commit | 内容 |
|---|---|---|
| corten-r01-m0 | 93d907a5ff2e | M0: android17-6.18 boots |
| corten-r01-m2a | b8386e4e2467 | PT 页描述符骨架 + per-PTE metadata + KUnit |
| corten-r01-m2b | 2fd4070e745c | covering-PT-page 锁协议 + 事务 API |
| corten-r01-m2c-fix1 | 1284a235f751 | corten=on 启动修复 + review 修正 |
| corten-r02-m3a-f1 | e911b31adb9c | BH 对称页表页锁（softirq 死锁修复） |
| corten-r02-m3b-s123 | d040b61051af | arena: prctl 注册 + frame xarray + shadow-VMA |

当前进度: M0 ✓ M1 ✓ M2 ✓ M3a ✓ M3b(80%) · M4-M8 未开始 · M9 设计完成+arm64 corten.o 交叉编译零错误
