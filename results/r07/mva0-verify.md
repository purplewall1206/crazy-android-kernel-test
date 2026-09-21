# M-V A.0: corten_region 区域记录落位 — 验证报告 (mva0-verify.md)

- 班次: r07 mva0 (2026-09-22 凌晨 CST, 时间盒内收口; 每步构建前 timegate 放行)
- worktree: /home/ppw/linux-6.18-mva, 分支 **mv-a0**, 基座 = 主树 HEAD
  **4ca6ef1048f0** (corten-r07-integrated, 零漂移), .config 自主树复制 (=y 全量)。
- 交付: 工作树补丁（**未 commit**, 按 A.0 "纯加法低风险可由 maintainer 直接提交" 口径）
  = patches/r07-mva0.diff, 3 文件 **+712/−7** (规格预估 ~+350, 实际含 KUnit +334 后
  712; 生产代码净增 ~+378):
  - include/linux/corten_arena.h  +197/−4 (region class/rflags 编码、struct 字段、
    注册表接口原型+迭代器、=n 桩、池容量注释)
  - mm/corten_arena.c             +181/−3 (register/lookup/next 实现、5 个 rclass
    写点、debugfs arenas 增列)
  - mm/corten_arena_test.c        +334/−0 (4 个新 KUnit 用例)
- 主树 / 其它 worktree 未触碰; 未 push; 密码未落盘。
- 产物: results/r07/mva0/ 全套日志 + bzImage-mva0 (sha256 74c1aba2…deab74b8b,
  guest 门与 KUnit 用的就是这个二进制; 终态树重建件 sha 95a587cf… 不同属
  kbuild 非比特可复现——源与配置逐字节同, make kernelrelease 同)。

## 1. 实现摘要（对照 SPEC §2 / §3.1.0）

| SPEC 条目 | 落位 |
|---|---|
| §2.1/§2.2 region 内嵌 1:1 | `struct corten_arena` 追加 rclass/may_prot/rflags/rfile/rpoff/npieces/rpieces/carrier 八字段 (完整签名级照抄规格); carrier 只留字段恒 NULL (语义 A.2b); FILE 类枚举+字段就位无生产者 (V-B) |
| §2.2 may_prot 上界 | `corten_region_register()` 以 `may | ar->prot` 落"may ⊇ prot"超集规则于构造时闭合 (INV-MV3-(b) 的前置) |
| §2.4 注册表=扩展现有 2M 帧 xarray | 零新结构; `corten_region_lookup` = 既有 lookup 语义别名 (sentinel/RESERVED 不算覆盖), `corten_region_next` = xa_find 升序 + 指针去重 + sentinel 跳过, `struct corten_region_iter` 帧游标; 消费者只经这两个函数 (换内部实现不动消费者的接口收敛点) |
| §3.1.0 rclass 初值映射 | 活跃=ANON (declare_locked / fork_register_child / pool_reactivate / pool_take 四写点), `idle`=RESERVED (park_locked 写点) —— 即规格风险栏点名的 park_locked/reactivate/pool_take 双写点一致性, fork 侧为规格 §2.2 "子侧深拷贝" 的 A.0 字段版 |
| §2.6 rflags 编码 | CORTEN_RF_{SOFTDIRTY,DONTCOPY,WIPEONFORK,SEQ_READ,RAND_READ}; 唯一活生产者 = SOFTDIRTY (白名单容忍), 其余为放行预留 (无生产者, 编码先行) |
| §3.1.0 debugfs | arenas 渲染增列 `cls/rflg/pcs` (rclass 名/RF 十六进制/pieces); dump 渲染未动 (规格"可选择性", 取最小) |
| 零行为变化红线 | 全部为字段写入/读取渲染/新增查询函数; 无任何既有调用点改路由; 无新锁 (§2.7: 写点全在既有 mmap_write 下写点旁) |

**留给 A.1 的接口边界** (本片只加不改): `corten_region_lookup/next` 已收敛为
A.1+ 消费者 (fault/pool/proc) 的唯一查询面——A.1 的 park 去 VMA 手术、A.2a 的
`corten_auto_validate`、A.2b 的 carrier 生产 (carrier_alloc 落位后写
`ar->carrier`)、V-B 的 FILE 生产 (`register()` 需扩 file/pgoff 参数或新附
attach 函数, 当前 register 无 FILE 入参、rfile 恒 NULL) 都不再需要新查询结构;
INV-MV3 checker (may⊇prot 逐页/rclass-idle-池三态互恰) 的数据面已就绪, KUnit
已锚 may⊇prot 与 idle⇔RESERVED 两格。J2 白名单审计 walker 未做 (任务书 A.0
KUnit 清单未含; 规格定位在 A.3)。

## 2. 验证矩阵

| Gate | 结果 |
|---|---|
| =y `make -j12` (三次: 初版/终版/=n 恢复后) | 三次 RC=0, 零新增警告 (唯一命中 = 基线 objtool cpuidle, r05 起登记口径) |
| checkpatch --strict (最终 diff) | **0E / 0W / 0C**, 845 行 (two fix 后; 初版 2W=fallthrough 字样误报+横幅注释, 已改) |
| KUnit corten* on×2 (kunit.filter_glob=corten*, 三套件口径) | run1: corten 24/0/1 + **corten_arena 52/0/0** + corten_fault 30/0/2; run2: 逐格相同 → 全绿零 not-ok |
| KUnit corten* off×1 | corten 25/0/0 + corten_arena 21 pass/0 fail/**31 skip**(52) + corten_fault 6/0/26 → 全绿 (park 用例 off 臂 kunit_skip, 规格形状) |
| 基线对账 | 集成基线口径 24+48+30 (on) / 25+48(=18p+30s)+32 (off); 本片 arena 套件 48→**52** = +4 新用例恰一次, 其余逐格一致 |
| =n 八对象 (memory/mmap/migrate/rmap/swapfile/gup/oom_kill/x86-fault) | RC=0, 0 警告, `nm` 八对象 **零 corten 符号**; 配置恢复 =y 复核 |
| lockdep 变体 (PROVE_LOCKING=y 全量构建 RC=0 零新增警告 + 无盘 KUnit) | 三套件全绿 (25/0 + 52 + 32); 见 §4 的 1 条基线 splat 归因 |
| guest: VM trixie-mva0.img (=trixie-integrated 副本, port 10033, corten=on mitigations=off, bzImage 74c1aba2) | **mva0-guest.sh PASS=10 FAIL=0** (下表) |
| guest 门明细 | ① arenas 表头含 cls/rflg/pcs ② run_mode_smoke rc=0 (T1c 契约件 share/t1c, sha f45157cf) ③ **26/26** (PASS=26 FAIL=0) ④ SMOKE-DRIVER PASS + 台账 0→0 ⑤ JThreadBench 2000×3 ×3 JVM rc=0 ⑥ metis_eq rc=0 ⑦ MODE marker 在 ⑧ metis_eq rc=0 第二遍 ⑨ checksum 自洽: `distinct_words=65073, checksum=2d383eeed4ceb73b` 两遍同值 ⑩ debugfs **live region 行观测**: `anon`/`rsvd` 类、rflags、pcs=1 列齐全 (样例行含一条 park 期 `rsvd` 预约——A.1 的 S-4 目标形态提前可观测) + dmesg corten-quiet (0 WARN/BUG) |

live region 行样例 (guest 实拍, results/r07/mva0/ 下 guest 输出):

```
              mm              vma [start,end)                prot cls  rflg pcs status
ffff91c8815a5480 0000000000000000 [100000000000,100000200000)  0b rsvd 0000 1   active
ffff91c8815a5480 ffff91c8810fa200 [100040000000,100048000000)  08 anon 0000 1   active
```

## 3. KUnit 新用例 (4 个, 全绿)

1. `corten_arena_test_region_record`: register 真值表 (类落位/MAY 超集规则吸收
   prot/RF 全位回读/单件出生/FILE+carrier 空) + DECLARE 附加反射 (prot/may/rflags
   零值) + region_lookup ≡ arena_lookup 别名; MEM_SOFT_DIRTY 开启时另有
   VM_SOFTDIRTY→CORTEN_RF_SOFTDIRTY 的白名单容忍反射臂 (本配置 =n, 编译出)。
2. `corten_arena_test_region_iter`: 合成注册表 (真实杂志钩子注入 512 sentinel 帧)
   + 双 arena 多帧 → 迭代器产出 [A, B] 恰一次 (指针去重), 升序, sentinel 全跳,
   穷尽 NULL, 游标重启重放一致; 空 registry/registry-less mm → NULL。
3. `corten_arena_test_region_park` (corten=on): park 后 visibility split——
   point lookup 不可见 (legacy 语义) ∧ 枚举仍产出且 rclass=RESERVED ∧
   idle 同步; pool take 后翻回 ANON + prot 重编。SPEC §2.4/§2.5 的双视角锚。
4. `corten_arena_test_region_fork` (gate-free): fork_commit 子侧 region 记录 =
   父侧深拷贝 (class/prot/may/rflags 逐字段, 每 arena 恰一 region)。

## 4. lockdep splat 归因 (诚实记录: 基线既有项, 非本补丁回归)

- 现象: PROVE_LOCKING=y 无盘 KUnit (off 臂) 中, corten_arena 套件运行期记录
  1 条 `WARNING: inconsistent lock state`——`xa_destroy` (corten_arena_state_free,
  A5 arena-less call_rcu 软中断释放路径) 在 softirq 取 `state->arenas` 的
  xa_lock, 与任务态 `xa_store` (declare_locked) 注册的 {SOFTIRQ-ON-W} 用法冲突;
  测试结果不受影响 (三套件全绿, splat 后套件继续跑完)。
- 归因: 以 **pristine 基座 4ca6ef1048f0** 专门构建对照件 (临时 detach worktree,
  验证后已删除), 同配置同命令复现**同一签名** (xa_destroy+0x5e /
  declare_locked 注册侧 / declare_reject_flags 触发, 仅 lock-class 编号因构建
  内含对象数偏移) → **基线既有**, 与本补丁零因果 (本补丁未触碰
  xa_store/xa_destroy/state_free 任一端)。
- 登记口径: m6t34 zapfix 的"lockdep 严格零签名"读数未覆盖 perf2a 后的基座
  (perf2a 与 r07-integrated 均无 lockdep 轮), 该项为 A5 快速退出与任务序列
  时序共同暴露的既有间歇项; 证据对存 results/r07/mva0/{lockdep-kunit.log,
  base-lk-kunit.log}。建议 maintainer 排期: `xa_destroy` 换 `__xa_destroy`
  或 state_free 走 task 上下文 (A.1+ 顺手项, 不属 A.0 最小补丁)。

## 5. 判定

**M-V A.0: PASS。** 区域记录数据面 + 注册接口 + rclass 写点 + debugfs 增列 +
4 KUnit 全部落位; 语义保持矩阵逐格复核: =y/=n/lockdep 三构建零新增警告,
KUnit on×2/off×1 全绿 (基线 +4 恰好), =n 八对象零符号, guest 十门全过且
debugfs 区域摘要在真实 MODE churn (metis 线程池含 park 池) 下可观测。
零行为变化红线维持: 补丁内无任何既有调用点改路由, 全部 guest/内核回归与
基线读数一致。未 commit——纯加法低风险, 按 D 系惯例待 maintainer 复核后
直接提交 (patches/r07-mva0.diff)。

## 附: 证据文件清单 (results/r07/mva0/)

- mva0-worktree.diff (= patches/r07-mva0.diff 副本), kunit-{run1,on1,on2}.log,
  lockdep-{build,kunit}.log, base-lk-kunit.log (基座归因对照),
  build-{n,y-final,y-restore}.log, bzImage-mva0 + sha256, serial.log,
  kunit-run.sh (runner), guest 门输出见会话记录 (guest 侧留档 /tmp/mva0/)。
- guest VM: trixie-mva0.img (port 10033, qemu pid 见 /home/ppw/vm/qemu-mva0.pid),
  留运行供 maintainer 复核; mva0-guest.sh 在 share/ (9p)。
