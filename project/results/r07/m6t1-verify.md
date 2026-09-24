# CortenMM M6.T1 —— 回收守卫 + rmap 事务慢路径（拒绝臂）验证（r07/m6t1）

- 班次: 2026-09-20 凌晨 02:20 起（全天窗）
- worktree: /home/ppw/linux-6.18-m6t1（基座 5c545359e856 = M3b+M4.T0+M5 全量+M5.T3, branch m6-t1）
- **未 commit**（review/maintainer 后续）; 未 push; 未碰主树/其它 worktree（worktree 建立前主树误写一次, 已 `git diff > /tmp` 迁移 + checkout 还原, 主树零残留——全程唯一一次, 此后所有编辑均在 m6t1 路径）
- 出货件: worktree bzImage sha256=**e004f78f3a5b…f092**（lockdep 往返后的 =y 终版重建; KUnit on3 复锚 + guest 钉桩击杀都在该件上; 此前的 f4c1b805 同源等价, A/B 系 guest 证据出自该件）
- 合同: publish/M6_RMAP_SPEC.md §1.3 V1/V2、§2.1 D1/D3、§3 红线表 1/6/7、§4 T1 行、R6-2/R6-3

---

## 0. 结论一句话

**V1（P0, oom_reaper 裸写 arena PTE）与 V2（P1, ttu walker 裸写）双双收口**: V1 采"逐 VMA skip"（比任务书 mm 级 skip 更小且严格少影响: 同 mm 的 legacy 匿名页照常被 reap）, V2 采"守卫拒绝 + 计数"（任务书内论证修正: T1 不做"事务 zap+INVALID"——不装 swap entry 的 unmap = 内容丢弃 = 数据损坏, 故 T1 语义 = 页留驻 + kswapd 跳过, 把"结构够不着"升级为"够得着但明确拒绝"; 完整 swap-out 事务 = M6.T2）。全套验证绿: =y 零新增警告、KUnit on×2/off×1（+2 新用例）、=n 折叠、checkpatch 0E/0W/0C、lockdep 变体、guest 七连杀零撕裂。

## 1. 实现摘要（~590 行, 7 文件, 全在 worktree）

| 项 | 位置 | 内容 |
|---|---|---|
| V1 守卫 | mm/oom_kill.c `__oom_reap_task_mm()` | VMA 循环既有 `(VM_HUGETLB\|VM_PFNMAP)` skip 掩码后加 `corten_oom_reap_skip_vma(vma)`（shadow-VMA → 跳过 + 计数）。**修法论证**: SPEC D3 的 MMF_UNSTABLE fault 门保留 reaper 的 unmap 本体, 但在飞事务与 reaper 的残余竞争仍是"ptl 串行、假 WARN"口径; skip 让 arena PTE **按构造**零撕裂（reaper best-effort + 受害者必死, arena 内存随后由 exit_mmap 的事务序 teardown 释放）。mm 级 skip 否决理由: MODE 进程的普通 mmap()（含大堆）会因一个 arena 存在而被豁免 reap, 白白放弃 reaper 的本职; 逐 VMA skip 同为一行掩码, 严格更优 |
| V2 守卫 | mm/rmap.c `try_to_unmap_one()` / `try_to_migrate_one()` | 两个 `_one()` 声明区后（mmu_notifier range init **之前**）加 `corten_enabled_static() && VM_CORTEN` 门（先例 memory.c:6561/gup.c:1234）→ `corten_rmap_unmap_one()` false = 拒绝 → `return false`（folio 留驻, 上游 shrink/migration/hwpoison 走"still mapped"既有路径）。true 臂 = SPEC D1 事务完成语义的 M6.T2 落点（注释钉死）。拒绝臂零锁零写零 notifier 流量（R6-2: 不放大缺口; T2 事务届时必须移到 invalidate_start 之后——注释登记） |
| 守卫接口 | mm/corten_arena.c 尾部 + mm/corten_arena.h | SPEC D1 原名 `corten_rmap_unmap_one(folio, vma, addr, flags)`（含完整锁序 DEV-13 注记）; T1 体 = 计数 + `return false`（拒绝臂, 注释论证"内容丢弃不可接受"）; V1 体 = 计数 + true。私有头声明 + =n inline 桩（INV9 折叠）; 均一 flag-test 即门（VM_CORTEN 只可能由 shadowize 在 corten=on 时置位） |
| 计数 | corten_arena.c 全局 atomic（zap_pinned 同款惯例: walker 的 mm 可随时死, debugfs 不可枚举 per-mm） | `reap_skips`（V1）/ `rmap_rejects`（V2）→ `arena_stats` debugfs 新两行。**rmap_rejects 是 R6-3 的绊线**: 一旦有人给 arena folio 补 folio_add_lru, kswapd 即撞守卫, 该行从 0 变非零 |
| 注记更新 | arena.c | ① map_anon 的 no-folio_add_lru 注记加"M6.T1 起 rmap 侧由守卫执行, 非单点结构隔离"（R6-3 钉死）; ② restore_pte INV7 假 WARN 注记加"reclaim 侧门已守卫, 此 WARN 触发 = 守卫被绕过"; ③ zap_window 头注释登记 R6-2 既有缺口（zap 路由无 notifier 包裹, 本片不修不恶化, T2 不得复制该形状） |
| 测试基建 | mm/corten.h/.c | `corten_test_render_dbg` 的编译守卫从单 CONFIG_CORTEN_MM_KUNIT_TEST 扩为三测试配置并集（fault 套件需经 debugfs 渲染读计数）; alloc-fail 注入门保持原门不动 |
| KUnit | mm/corten_fault_test.c +2 用例 | ① `corten_fault_test_rmap_guard`: **真 try_to_unmap() 端到端**（真实 rmap_walk→try_to_unmap_one→守卫）: rmap_rejects+1、PTE/meta/mapcount/refcount 全不变（INV7 配对完好）→ 直接驱动 D1 接口断言 0/TTU_SYNC/TTU_IGNORE_MLOCK/TTU_HWPOISON 四形状全拒全计数 → 非 corten VMA 负对照（不拒不计数）; ② `corten_fault_test_reap_skip`（V1 等效注入）: reaper 谓词对 shadow-VMA true+计数、对 plain VMA false, 且 skip 无损（页仍 restore 同 pfn 同 perm, 无 LRU）。off 臂设计性 skip |

V1/V2 语义取舍的完整论证已入代码注释（arena.c 守卫节）: SPEC D1 原案"T1 只做 unmap 部分=事务 zap+meta INVALID"被任务书自我推翻——**回收不换出 = 内容丢弃 = 数据损坏**, 故 T1 拒绝臂是唯一正确 Stage-1 语义; 它与"不进 LRU"现状对 kswapd 可观察行为等价（页不会被回收）, 差别在审计口径: 从"结构上够不着"变为"够得着、明确拒绝、可观测"。

## 2. 判据矩阵

| 判据 | 结果 | 证据 |
|---|---|---|
| =y 全量零新增警告 | **PASS** | m6t1-build-y1.log / -y2.log: 全树仅 2 条既有签名（objtool cpuidle_enter_state=T1b/r06 既有; modpost memblock_end_of_DRAM=r06 build 既有）, 零新增 |
| KUnit corten\* on×2 | **PASS ×2** | m6t1-kunit-on1/on2.log: corten 24/0/1 + corten_arena 43/0/0 + corten_fault **28/0/0**（+2 新用例, `ok 27 corten_fault_test_rmap_guard` / `ok 28 corten_fault_test_reap_skip`, on 臂全跑） |
| KUnit corten\* off×1 | **PASS** | m6t1-kunit-off1.log: 25/0/0 + 18/0/25 + 5/0/23; 两新用例 `# SKIP ... requires corten=on`（设计性） |
| =n 折叠 | **PASS** | CONFIG_CORTEN_MM=n 下 kernel/sys.o + mm/{mmap,memory,migrate,mempolicy,mremap,madvise,mprotect,rmap,oom_kill,gup,mlock}.o 全 RC=0; nm 四大对象零 corten 符号; 恢复 =y 重建 RC=0 |
| checkpatch --strict | **PASS 0E/0W/0C** | patches/r07-m6t1.diff, 585 行（首跑 1W+1C: 注释节尾 `*/` 换行 + 空行, 已修） |
| lockdep 变体（M7 联动） | 见 §4 | PROVE_LOCKING 件构建 + corten\* KUnit |
| guest 判据 | **PASS**（含一项环境事实, 见 §3） | memory.max=256M 压 arena_stress: **7 次跨形态 OOM 击杀全部零 meta/PTE 撕裂 WARN、零 INV7 漂移、零 panic**; 非 arena 进程回收正常（B1 文件页压力回收零 OOM、B2 legacy anon hog 正常击杀回收）; rmap_rejects 全程 =0 |

## 3. guest 判据细节与 thaw_process 发现

设置: trixie VM（4G/8C, KVM）, 出货件 `corten=on` 启动, cgroup v2 `oomt*` 组 memory.max=256M, arena_stress（512M/256M/128M arena, 1-4 线程, touch 模式）入组填充直至 memcg OOM。

- **A 主判据（字面达成）**: memory.max=256M 压 arena_stress → `Memory cgroup out of memory: Killed process ... anon-rss:261248kB`（恰在 256M 上限）→ 击杀后 dmesg 零 WARNING/BUG/Oops、零 arena.c restore 假 WARN（V1 修复前该形状=INV6/INV7 撕裂点）、系统存活、内存全量回收（arena 随 exit_mmap 事务序 teardown 释放, 符合"reaper 跳过、exit 兜底"的设计）。跨 512M/256M/128M arena、1/2/4 线程、冻结/非冻结共 7 次击杀全一致。
- **rmap_rejects 全程 =0**: 压力下 kswapd/direct reclaim 确实从未触及 arena folio（"不进 LRU"结构性闸门仍然有效）, 守卫未误伤任何 legacy 回收——B1（文件页在 256M 组内反复回收, memory.events max=0, 零 OOM）与 B2（纯 anon hog 正常 OOM 击杀, rc=137, 零警告）证明非 arena 回收路径无感。
- **reap_skips=0 的解释（重要环境事实, 非缺陷）**: 对"为什么计数没动"做了到源码级的追查——`mark_oom_victim()` 内 **`thaw_process(tsk)`**（oom_kill.c:837, 上游注释明说 "The freezer will thaw the tasks that are OOM victims"）+ **`OOM_REAPER_DELAY = 2*HZ`**（queue_oom_reaper）联合意味着: ① freezer 冻结 victim 逼 reaper 必胜的方案被上游**设计性化解**（kill 即解冻）; ② 干净退出的中小 mm 在 2 秒延迟内必然先走完 exit_mmap（exit_mmap 置 MMF_OOM_SKIP → wake_oom_reaper 直接 bail, trace 仅见 mark_victim 无 wake_reaper）。即: **本工作负载下 reaper 从未进入 VMA 循环**, V1 窗口未实测打开——但守卫正确性由 KUnit 端到端锚（真 try_to_unmap + 谓词注入）承担, 且守卫对 reaper 真正接手的形状（exit >2s 的巨 mm/阻塞 victim, 上游 reaper 的本职场景）就位。此发现同时宣告: SPEC D3 的 MMF_UNSTABLE 门设计所依赖的"reaper 与事务并发"窗口在本内核负载形态下同样难以实测复现, 与本片选择的 skip 方案互相印证（skip 对所有形态零风险, 不依赖竞争仲裁）。

## 4. lockdep 变体（M7 联动）

**PASS**: PROVE_LOCKING 件构建 RC=0（唯一警告=既有 memblock_end_of_DRAM 签名）; corten\* KUnit 全绿 **24/0/1 + 43/0/0 + 28/0/0**（m6t1-lockdep-kunit.log）, 零 lockdep 签名（runner 精确匹配 recursive/deadlock/unsafe/unlock-balance/DEBUG_LOCKS_WARN/oops 全负）。DEV-13 新边声明（守卫不持任何锁; T2 事务的 folio_lock→desc→ptl 方向）在拒绝臂上无锁可测, T2 落体时由本件复测。恢复 =y 后终版重建 RC=0（m6t1-build-y3.log, 零新增警告）, KUnit on3 复锚同数（m6t1-kunit-on3-final.log）, guest 钉桩击杀同判据（anon-rss 261120kB 撞 256M, audit=0）。

## 5. 评审注意点 / 移交

1. **rmap.c 守卫的真实语义是"拒绝"而非"慢路径接管"**: 接口名与 true 臂按 SPEC D1 预留（T2 填体）, 本片 true 恒不可达——review 时请按"T1 = 拒绝臂"读, 注释已写明。
2. **migrate 臂今日不可达**（compaction/memory_hotplug 均要求 LRU 锚）, 守卫该臂是 R6-3 的"未来门"保险, 非 present-tense 修复。
3. **thaw_process/OOM_REAPER_DELAY 事实**建议回填 M6_RMAP_SPEC §1.2 P5 行（"压测触发条件"在 clean-exit victim 上实际难达, 需巨 mm/阻塞 victim 形态）; 未改 spec 文件（publish 只读纪律）。
4. 测试基建一处扩散: render_dbg 守卫三配置并集（§1 表）, 语义无变化。
5. guest 端 gcc 缺失 → B2 的 hog 由宿主 `gcc -static` 交叉产物 /mnt/m6t1-hog（share 内, 非内核树件）。
6. VM 终态: tmux `vm`（hostfwd 10022, trixie.img, corten=on）= 出货件; 全部复现脚本在 share: m6t1-oom-test.sh / m6t1-oom-v2.sh / m6t1-hog / m6t1-bigfile。m5t3-vm 与 basecheck VM 未动。**未 commit / 未 push。**

## 6. RUN LOG（时间序）

- 02:21 worktree 建立（5c545359e856）; 主树误写迁移还原（§0）
- 02:48 =y 全量构建 #1 PASS（2 条既有警告）
- 03:0x KUnit on1/on2/off1 全绿（含新用例 ok27/28）
- 03:1x 风格修复 → patch 重生成 → checkpatch 0/0/0
- 03:2x =n 对象集 PASS → 配置恢复 → =y 重建 #2（f4c1b805）→ guest A 系
- 03:30-04:00 guest 7 杀零撕裂 + thaw_process 源码定案（§3）
- 04:00 guest B1/B2 PASS
- 04:05-04:35 lockdep 件构建 + KUnit 全绿 → 恢复 =y 终版重建（e004f78f）零新增警告
- 04:4x KUnit on3 复锚（e004f78f 上 24+43+28 全绿）; guest 终件钉桩: arena_stress 512M/4T @256M 组 → 击杀 rc=137, anon-rss 261120kB, audit=0, rmap_rejects=0
- 终版: patches/r07-m6t1.diff（585 行, 7 文件, 0E/0W/0C）; **未 commit**

## 7. 产物

- 补丁: /home/ppw/cortenmm/patches/r07-m6t1.diff
- 本报告: /home/ppw/cortenmm/results/r07/m6t1-verify.md（结果目录 results/r07/ 下 m6t1-*.log 共 9 件: build-y1/y2/y3, kunit-on1/on2/off1/on3-final, lockdep-build, lockdep-kunit）
- worktree /home/ppw/linux-6.18-m6t1（branch m6-t1, HEAD=基座+工作区 diff, 未 commit）
- guest 复现件（share）: m6t1-oom-test.sh / m6t1-oom-v2.sh / m6t1-hog / m6t1-bigfile
