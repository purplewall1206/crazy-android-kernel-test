# run_t5_compare.sh r06-t5 修复记录 (2026-09-18 06:1x, T5 班)

触发: QUICK=1 /mnt/t5quick 首跑 jvm t0 run1 腿挂死 —— JVM 子进程 D 态于
v9fs_evict_inode（SIGKILL 免疫, /proc/11165/stack 抓证）, driver 的 run_arm
无腿级超时, 套件级 deadline 只在组间判定 → driver 无限阻塞, QUick 中止。

修复两处（宿主 selftest 19/0 PASS 复验; bash -n 过）:
1. 全部计时腿包 `timeout -k 10 $T5_LEG_TIMEOUT`（默认 480s, env 可覆盖）,
   rc=124/137 如实记为数据（FAIL 行 + raw JSON rc 字段）。
   依据: dedup_eq_tcmalloc t0 实测 308s 合法耗时, 480s 给足余量。
2. JVM 腿本地化（M1 对齐: M1 的 run_jvm.sh 本就跑 guest 本地 /root/bench/apps）:
   - class 目录: APPS_DIR 在 /mnt(9p) 时先 cp 到 /root/t5-jvm 再 cd 执行
     （jvm_suite + run_arm 的 JVM_DIR）;
   - stdout/stderr 重定向: mktemp /root(/tmp 回退) 本地临时文件, 跑完 mv 进
     $OUT —— JVM 退出时不再持有任何 9p fd（v9fs_evict_inode 楔死规避）;
   - trace_pair 的 RUN_IN_DIR 同步用 JVM_DIR。
   env.txt params 行追加 T5_LEG_TIMEOUT=$T5_LEG_TIMEOUT 供 D6 审计。

注: bench/ 不在 git 内 → 本文件即修复记录（无 diff 文件）; 原 QUICK 产物
保留于 share t5quick/（挂死现场 + mmbench/apps 全部数据）。
