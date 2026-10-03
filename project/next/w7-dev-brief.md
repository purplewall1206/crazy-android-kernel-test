# W-7 开发任务书 · multi-record registry（frame-sharing 收编完备, MV2 树归零兑现）

授权: STATE D34（W-6b 判定 J4→W-7 承接）+ D28 字面树归零目标。
基座: worktree /home/ppw/linux-6.18-mva @ d26ffcd780ae（HEAD）。**设计先行切片**:
先交设计稿（next/w7-design.md）经主会话认可后再实现——本片是 registry 数据形状
重设计, 不是补丁堆。

## 问题（D34 定界）
registry xarray 按帧（PMD）单记录: INV2/magazine/frame 表全挂其上。同帧多 VMA
（ELF 五段常态）→ sweep 只能收编首段, 邻段 declare 撞 C1/overlap fail-open 留树
（skip_declare=288/电池）。J4 字面"MODE 树条目==0"在此架构下不可达。

## 设计要求（设计稿必须覆盖）
1. 数据形状提案（至少两案对比: 帧内记录链 vs 跨帧 span 记录 vs 其它）, 择一给
   完整论证。
2. **消费面全走查**（每面给改动点清单）: fault 快路径+慢路径、GUP 探针、fork
   镜像+commit、exit walk 两相位、sweep adopt/pick、j2/wl 走查、magazine/
   magazine recycle、pool park/reactivate、punch 路由、implant 交互（D33 形）、
   debugfs arenas 渲染、KUnit 125 锚的夹具假设。
3. 不变量保持: INV1-9/INV-MV2/INV6/INV7 逐条对esign 复核; C1（over-content
   declare 拒绝）在新形状下的等价物。
4. 记账与统计兼容（ptdescs/meta_arrays/sweep 计数族/j1/j2 族语义不变或迁移表）。
5. 迁移路径: 旧单记录形状→新形状的兼容策略（一次性切换+全锚重验 vs 双读期）。

## 实现红线（设计认可后）
- 每消费面改动带锚（红绿）; checkpatch 0E/0W; =n 折叠; fail-open 全臂保持。
- 验证链同 W-6b（三套件/=n/guest 门全套）+ **tree_entries 白名单外==0 终判据**
  （动态 ELF 负载: smoke 进程族+metis, 多段二进制形状必须收编到位）。
- 工件 results/r07/w7/; 报告 next/w7-dev-report.md。

## 坑清单
同 W-6b（pidfile/-j12 串行/glob 星号/9p 手挂/interlock flake 复跑绿）。
