# 归档说明 (2026-09-13 由环境预制会话执行)
这三个文件 (microbench.c / run_all.sh / README.md) 是环境预制会话派 developer agent
并行开发的微基准副本。主控循环会话 (r01) 独立交付了 bench/mmbench/ 并经 guest 冒烟，
STATE.md 决策 D6 已钦定 mmbench 为 M1/M8 唯一权威基准（同二进制同脚本同参）。
本套实现仅 host 自测过、口径略异（mmap=成对计 op 等），为避免后续 agent 拿错工具而归档。
若需对照另一套口径，此处的实现与 README 中 A1-A9 假设记录仍可参考。
