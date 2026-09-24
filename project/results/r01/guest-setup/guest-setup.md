# r01 Guest Setup Report (M0 测试件安装)

- 日期: 2026-09-13 00:10–00:4x CST (免费窗口内, timegate 放行)
- 内核: `bzImage-6.18` = `6.18.0-gb05b60d2e64d` (#15 SMP PREEMPT_DYNAMIC, 2026-08-29 构建)
- Guest: Debian GNU/Linux 13 (trixie), 8 vCPU, 3921 MB RAM (`~/vm/trixie.img`)
- 启动: KVM 加速, 约 14s 到登录提示; launch 命令含 `systemd.mask=sys-kernel-config.mount`
- **VM 保持运行中** (tmux 会话 `vm`, ssh 127.0.0.1:10022, 未 shutdown)

## 结果总览

| # | 项目 | 结果 | 关键数字/说明 |
|---|------|------|--------------|
| 1 | VM 启动 + ssh | PASS | `uname -r` = 6.18.0-gb05b60d2e64d; gssh 免密 OK |
| 2 | 9p 挂载 + 读写 | PASS | hostshare→/mnt, rw,relatime,access=client; 写读删 OK |
| 3 | bench 脚本在位 | PASS | /root/bench/bin/{setup_bench,run_once,monitor,summarize,setup_zram}.sh 全部存在(镜像内置, 2026-08-27) |
| 4 | apt update | PASS* | *首次失败: 根分区 100% 满; 清理后成功 (68.5MB, 6min @ ~187kB/s) |
| 5 | OpenJDK 21 | PASS | 包名 `openjdk-21-jdk-headless` 21.0.12.1+1-1~deb13u1; java+javac OK |
| 6 | lmbench | PASS | `lmbench 3.0-a9+debian.1-9`; lat_proc fork 冒烟 **1037.4484 us** |
| 7 | gcc/make/python3/bc | PASS | gcc 14.2.0 (已有), make 4.4.1 (已有), python3 3.13.5 + bc 1.07.1 (新装) |
| 8 | zram | **PARTIAL** | zram0 2G prio=100 工作, 但算法 **lzo-rle 非 lz4** (内核未编 ZRAM_BACKEND_LZ4, 见下) |
| 9 | MGLRU | PASS | `/sys/kernel/mm/lru_gen/enabled` = **0x0007** (非 0) |

## 各步详情

### 启动与 ssh (00/01)
- redis-server.service 启动失败 (FAILED), 与本任务无关, 不影响 ssh/9p/bench。
- boot console 存档: `00-boot-console.txt`

### 磁盘事件 (坑 #1)
guest 根分区 9.8G 初始 100% 满 → apt update 直接 `No space left on device`。
- 大头: `/var/tmp/n2` = **4.4G** (8/29 旧迭代压力测试 scratch 目录 s1..s16), 已删除
- 另清理: apt-get clean (398M), journal vacuum 到 20M (释放 152M)
- 结果: 可用空间 0 → 4.9G; 全部装完后余 4.2G (55% 用量)

### apt (02/02a)
- 源: `deb https://deb.debian.org/debian trixie main contrib non-free-firmware non-free`, DNS 10.0.2.3 (QEMU) 正常
- QEMU NAT 慢 (~187 kB/s): update 6 分钟, jdk-headless 安装约 15 分钟; 建议后续大件放后台

### OpenJDK (04)
- trixie 无 `openjdk-21-headless` (Ubuntu 命名); 对应包为 `openjdk-21-jdk-headless` + `openjdk-21-jre-headless`
- java -version: `openjdk version "21.0.12.1" 2026-08-18`, HotSpot 64-Bit Server VM, mixed mode
- javac 21.0.12.1 (后继 workload 可编译 .java)

### lmbench (05/05b)
- 包: lmbench 3.0-a9+debian.1-9 (Debian 官方源就有, 无需源码编译)
- **lat_proc 路径: `/usr/lib/lmbench/bin/x86_64-linux-gnu/lat_proc`**
- 冒烟原始输出:
  ```
  # /usr/lib/lmbench/bin/x86_64-linux-gnu/lat_proc fork
  Process fork+exit: 1037.4484 microseconds
  ```
- 同目录还有 bw_mem/bw_pipe/bw_mmap_rd/lat_unix 等全套 lmbench 3.0-a9 微基准

### zram (06) — PARTIAL, 需主 agent 决策
- `setup_zram.sh` 已存在并成功执行: zram0 disksize 2G, mkswap, `swapon -p 100`
- swapon: zram0 (partition, 2G, **prio 100**) 为主, /root/bench/swapfile (file, 2G, prio -2) 兜底
- **但算法是 lzo-rle, 不是 lz4**。脚本里 `echo lz4 > comp_algorithm` 被静默吞掉:
  - `cat comp_algorithm` → `[lzo-rle] lzo` (无 lz4 选项)
  - 内核配置: `CONFIG_ZRAM=y` 但 `# CONFIG_ZRAM_BACKEND_LZ4 is not set` 且 **`CONFIG_ZRAM_BACKEND_FORCE_LZO=y`**, `CONFIG_ZRAM_DEF_COMP="lzo-rle"`
  - 注: CRYPTO_LZ4=y 本身可用, 仅 zram 后端被 FORCE_LZO 锁死
- 影响: 当前 bzImage-6.18 上"默认 lz4"不成立; **后续内核构建需 `CONFIG_ZRAM_BACKEND_LZ4=y` + 去掉 FORCE_LZO + `CONFIG_ZRAM_DEF_COMP="lz4"`**, 否则 M1 基线与论文 zram 配置不一致
- lzo-rle vs lz4 均为 LZO 族轻量压缩, M0 冒烟阶段功能等价

### MGLRU (06-lru_gen)
- `cat /sys/kernel/mm/lru_gen/enabled` → `0x0007` (所有 mGLRU 特性开启, 非 0 = PASS)

### 9p (07)
- `mount -t 9p -o trans=virtio hostshare /mnt` 成功 (未自动挂载, 需手动)
- 读写测试: `echo test > /mnt/vm_write_test` → 读回 → 删除, OK

## 装好的包版本清单 (本 session 新装)
| 包 | 版本 |
|----|------|
| openjdk-21-jdk-headless | 21.0.12.1+1-1~deb13u1 |
| openjdk-21-jre-headless | 21.0.12.1+1-1~deb13u1 |
| ca-certificates-java | 20240118 |
| lmbench | 3.0-a9+debian.1-9 |
| python3 | 3.13.5-1 (3.13.5-2+deb13u5 stdlib) |
| bc | 1.07.1-4 |

已有: gcc 14.2.0-19, GNU Make 4.4.1, perl。

## 遗留事项 (移交主 agent)
1. zram lz4: 下次内核构建调整 3 个 config (见上), 或接受 lzo-rle 作为基线并写进 REPORT
2. redis-server.service 在 guest 内启动失败 (与本任务无关, 但每次 boot 报 FAILED)
3. 9p /mnt 未自动挂载, 后继 agent 每次开機需手动 mount (或入 rc)
4. guest 磁盘余 4.2G — 再装大件前先 `df -h` 检查; 旧 scratch 目录习惯性堆 /var/tmp
