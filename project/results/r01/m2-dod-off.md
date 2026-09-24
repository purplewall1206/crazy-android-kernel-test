# M2 DoD — 冒烟 1: corten=off (默认) — VERDICT: PASS

- 日期: 2026-09-13 07:33–07:52 CST (r01 夜)
- bzImage: /home/ppw/cortenmm/bzimg/r01-m2-corten-skeleton
  sha256 = 080443702d6a11da47cfeb360a7f03aea875da4bc82efb2bc69329744fd00f0e
  (dev agent #8 构建: CORTEN_MM=y + CORTEN_MM_KUNIT_TEST=y; 构建先于 M2b commit,
   commit 未改文件内容, bzImage 仍有效)
- 串口日志: boot-m2-off-console.log (最终常驻 off VM 为同一构建的第 3 次 off 启动)

| # | 检查项 | 结果 | 证据 |
|---|--------|------|------|
| 1 | 启动到登录 ≤180s | PASS | ~25s (KVM; 共 3 次 off 启动均 ~25s) |
| 2 | uname -r | PASS(记录) | `6.18.32-gb8386e4e2467-dirty` — -g 后缀取构建时 HEAD=b8386e4e, `-dirty` 因构建时工作树有 M2b 未提交改动; 均为预期, M2b commit 后未重建 |
| 3 | dmesg 无意外 corten 输出 | PASS | 仅 KUnit autorun 输出 (CONFIG_KUNIT=y 预期行为, 见下); runtime 未启用: debugfs `enabled 0` |
| 4 | zram lz4 2G | PASS | `zramctl`: `/dev/zram0 lz4 2G ... [SWAP]` (setup_zram.sh) |
| 5 | 9p hostshare 挂载 | PASS | `mount -t 9p -o trans=virtio hostshare /mnt` → ok |
| 6 | debugfs corten/stats 可读 | PASS | `enabled 0, ptdescs 0, meta_arrays 1, meta_bytes 4096, desc_alloc_fail 1, meta_alloc_fail 1, free_untracked 3` (fail 计数来自 KUnit fail-injection case, 预期) |
| 7 | mmbench 冒烟 (基线路径未坏) | PASS | `./mmbench mmap-pf low 2 1 42` → ops=8666, elapsed 1.000s, rc=0 (第 1 次启动) / ops=8004 (常驻 VM 复验) |

### KUnit 16/16 guest 证据行原文 (off 启动 dmesg | grep -i corten)

```
[    4.157130]     # Subtest: corten
[    4.167471]     # module: corten_test
[    4.179694]     ok 1 corten_test_layout
[    4.183916]     ok 2 corten_test_ptdesc_get_put
[    4.190524]     ok 3 corten_test_meta_roundtrip
[    4.205008]     ok 4 corten_test_meta_reinit_cleared
[    4.220528]     ok 5 corten_test_covering_level
[    4.238596]     ok 6 corten_test_covering_base_index
[    4.248108]     ok 7 corten_test_txn_covering
[    4.256390]     ok 8 corten_test_txn_hole
[    4.269251]     ok 9 corten_test_txn_stale_eagain
[    4.281632]     ok 10 corten_test_lock_range_args
[    4.291525]     ok 11 corten_test_txn_state_machine
[    4.304890]     ok 12 corten_test_txn_atomic_validate
[    4.317456]     ok 13 corten_test_txn_real_glue
[    4.379423]     ok 14 corten_test_txn_mutex_overlap
[    4.429662]     ok 15 corten_test_txn_mutex_disjoint
[    4.432878]     ok 16 corten_test_fail_alloc
[    4.438747] # corten: pass:16 fail:0 skip:0 total:16
[    4.448949] ok 1 corten
```

注: KUnit autorun 与 corten= 参数无关 (测试直接驱动协议核心, 不依赖 enabled key),
off/on 两种启动都会跑; 16/16 在 off 启动即已实证。
