# project/ — CortenMM 移植项目全量资料

内核代码在本树（android17-6.18 分支, corten-* tag 序列）。
本目录 = 项目仓（原 crazy-android-kernel-test master）内容并入：

- REPORT.md / REPORT-FINAL.md — 逐 gate 对账报告 + 平话终报
- STATE.md — 唯一权威状态源（决策 D1-D29 / 班次账）
- specs/ — 全部设计规格（MV/MV2/W1/PAPER 相关/ADV/ARM64…）
- docs/ — ROADMAP/DESIGN/EVAL/PAPER_SPEC
- results/ — 全部验证日志（bzImage 二进制镜像不入库, 见各片 green 条目的 SHA256SUMS）
- patches/ — 全部提交镜像（format-patch）
- log/ next/ — 班次报告 / 在制工件

master 分支 = 内核 HEAD + 本目录。后续每片入库同步更新本目录并推送 origin master。
