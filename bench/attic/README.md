# CortenMM Table 3 微基准 — Linux 移植版 (`microbench.c` + `run_all.sh`)

忠实实现 CortenMM (SOSP'25) 论文 Table 3 的五项内存管理微基准，用于 M1 基线采集与
M8 性能对照（CortenMM arena vs 基线 Linux）。单文件 C99，仅依赖 pthread + POSIX/Linux
syscall。**论文未公开其 harness 细节**，本文记录本实现的全部操作化（operationalization）
选择，供 M8 REPORT.md 直接引用。

## 1. 与论文 Table 3 的对应

论文 Table 3 原文（"Repeated operations on each thread"，每线程操作 16KB region）：
每线程操作自己 slot 网格中的 16KB 槽位（4 × 4KB 页）。

| bench        | 论文定义                                        | 本实现：被计时 = timed            | 本实现：不计时 = untimed |
|--------------|-----------------------------------------------|----------------------------------|--------------------------|
| `mmap`       | mmap() 一个 16KB region                        | 对未映射槽位 `mmap(16KB, MAP_PRIVATE\|MAP_ANONYMOUS)` | 批末 munmap 清扫（清理） |
| `mmap-PF`    | mmap() 一个 16KB region **然后访问它**          | **整个循环体** = mmap + 写触全部 4 页 + munmap | —（无额外准备） |
| `unmap-virt` | munmap() 一个**无物理页背书**的 16KB region     | munmap                            | prep 中 mmap 建立未触碰映射 |
| `unmap`      | munmap() 一个**有物理页背书**的 16KB region     | munmap                            | prep 中 mmap + 写触 4 页（populated） |
| `PF`         | 访问一个无物理页背书的 16KB region              | 写触 4 页（纯页错误成本）          | prep 中 mmap（前）、批末 munmap（后） |

## 2. 计时口径（关键假设，M8 引用处）

- **时钟**：`clock_gettime(CLOCK_MONOTONIC)`（vDSO，无 syscall）。
- **批窗口计时**：每 `batch` 次被测操作夹一对时钟读数（batch=1024；`mmap` bench 用 256，
  见 §3-A5）。时钟开销摊薄到 ~0.1% 量级。
- **准备/清理严格在计时窗外**：prep（claim/填充物理页）与 cleanup（munmap 清扫）从不计入
  `duration_s`。特别注意 `unmap` bench 的"写触 4 页"属于 prep（论文口径：unmap 测的是
  对 populated region 的 munmap，填充不是被测操作）。
- **`duration_s` = 各线程"被测窗口"时长之和**（非 wall time）。因此 claim 类 bench 的
  wall time ≈ 1.3–3.5 × duration_s（prep 占比），**吞吐数字不受影响**：
  `ops_per_sec = Σops / Σtimed_seconds`。
- **每线程独立 ops 计数，结束聚合**：`ops = Σ per-thread ops`；
  `ops_per_sec = Σops / Σtime_s`（聚合吞吐，与论文 Fig 13/14 的 ops/µs 总口径一致）。
- **恰好达到时长**：worker 以批为单位循环直到自身累计被测时长 ≥ `--duration`，
  末批允许少量超出（包含在 duration_s 内，吞吐无偏）。

## 3. 操作化假设清单（论文未公开、由本 harness 决定的选择）

- **A1 线程模型**：pthreads，同进程同 `mm`（与论文"Unix 兼容接口 + 多线程共享地址空间"
  语义一致；这正是 Linux `mmap_lock` 竞争的来源）。
- **A2 槽位放置**：`mmap(MAP_FIXED_NOREPLACE)` 把 16KB 映射精确落在槽位地址上；
  `EEXIST`（他线程 in-flight 占用）则按槽序列前进重试（见 A6）。探测到内核不支持该
  flag 时回退 `MAP_FIXED`（本项目内核 6.18 / host 5.15+ 均支持，回退仅为保底）。
- **A3 竞争模式（对应 §6.3 / Fig 14）**：
  - `low`：每线程私有 256MB 槽位网格（16384 槽，顺序循环复用），互不重叠；
  - `high`：全部线程共享 512MB 网格（32768 槽），每批每线程随机起点 + 奇数步长
    （对槽数取模为置换，线程内批内不重复），跨线程独立 xorshift64* 种子
    （**不用 `rand()`**：libc 锁会污染测量）。
- **A4 地址空间防护**：私有网格上限 256MB/线程、共享网格 512MB，循环复用，杜绝
  长跑地址空间耗尽；批内瞬时映射有界（≤ batch × 16KB × threads）。
- **A5 batch 不对称的原因**：`mmap` bench 的映射要存活到批末清理，8 线程 high 下
  in-flight 可达 8×batch；batch=1024 时与 32768 槽碰撞率 ~20%，故用 256（碰撞 ~6%）。
  claim 类 bench 的 claim 在 prep 完成（不计时），计时窗内零重试，可用 1024。
  `EEXIST` 重试若发生在计时窗内（仅 `mmap`/`mmap-PF` high 模式），按设计计入被测
  成本——它是竞争协议的一部分，README 明示不做剔除。
- **A6 页错误方式**：每 4KB 页 1 次 volatile store（触发缺页 + 零页安装），共 4 次/槽，
  对应论文 "accesses it"（访问即足额触发 4 次 page fault）。
- **A7 THP 抑制**：启动时 `prctl(PR_SET_THP_DISABLE, 1)`（best effort），防止
  khugepaged 把连续满触的批区域塌缩成 2MB 页、扭曲 `unmap`/`PF` 成本；16KB 槽位
  因此始终保持 4KB 页语义。
- **A8 不做 CPU 绑核、不关抢占**：论文 harness 未公开其调度设置；默认调度策略使
  guest（8 vCPU）与 host 复跑条件一致。注意计时窗为 wall clock：若机器被其它负载
  超订，线程在计时窗内被抢占的时间会计入 `duration_s`（吞吐偏低）。正式数据必须
  在独占的 guest（8 vCPU）+ 夜间空闲窗口采集；host 冒烟仅验证逻辑。
- **A9 输出**：每 (bench, contention, threads) 配置向 stdout 打一行 JSON：
  `{"bench":..,"contention":..,"threads":..,"duration_s":..,"ops":..,"ops_per_sec":..}`；
  `--bench mmap|mmap-PF|unmap-virt|unmap|PF|all --contention low|high|both
  --threads "1,2,4,8" --duration 5`。

## 4. `run_all.sh`（guest 内运行）

```bash
# guest 内（编译产物假定 /root/bench/microbench）
OUT=/root/bench/results/m1 bash run_all.sh            # 全矩阵: 5x2x4x3 reps x5s
OUT=/root/bench/results/smoke bash run_all.sh --smoke # 自检: 1s x 1 rep, threads 1,4
# 覆盖: BIN=<path> DUR=<s> REPS=<n> THREADS="1,2,4,8"
```

- 产物：`$OUT/raw.jsonl`（microbench 原始 JSON 逐行）+ `$OUT/summary.csv`
  （列：bench, contention, threads, median/min/max ops_per_sec, reps；同配置 reps 取**中位数**）。
- 全矩阵 wall time 估计：claim 类 bench 因 prep 占比约 20–40 分钟（duration=5s × 120 配置），
  适合放进夜间实验窗口（经 `timegate.sh`）。

## 5. 与 will-it-scale / lmbench 的差异

- **lmbench `mmap`/`lat_pages`**：测的是文件映射或单区域延迟/带宽，无"16KB 槽位网格 +
  mapped/virtual 区分 + 多线程竞争"语义，也没有把 prep/cleanup 与被测操作分离的口径；
  其 `fork`/`exec` 子项本另一条线（M1 全地址空间操作）单独跑。
- **will-it-scale**：以"进程/线程争用同一 syscall/资源"测扩展性（如同一文件、NULL 页错误），
  操作粒度与地址布局不可控，无法表达 Table 3 的五种操作组合与 low/high 两种**地址空间级**
  竞争模式（共享大区域 vs 私有区域），更无法对齐 16KB/4 槽口径。
- 本实现以论文 Table 3 逐字定义为规格，自建 harness；所有自由度（§2、§3）显式记录。
  与论文数字对比时应作"同规格不同 harness"解读（见 D4：目标为方向性验证）。

## 6. 构建与自检

```bash
gcc -O2 -Wall -Wextra -pthread -o /tmp/microbench microbench.c   # 零警告
/tmp/microbench --bench all --contention both --threads 1,4 --duration 1   # host 冒烟
```
