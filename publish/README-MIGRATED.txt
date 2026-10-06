2026-09-22 项目 git 重构说明：
publish/ 目录已并入项目根 git 仓库（/home/ppw/cortenmm，remote=crazy-android-kernel-test）。
本目录仅保留 REPORT.md 符号链接（→ ../REPORT.md）供 08:10 定时班次兼容。
定时班次执行注意：
1. 编辑 REPORT.md 实际作用于 /home/ppw/cortenmm/REPORT.md。
2. commit+push 在项目根执行: cd /home/ppw/cortenmm && git add -A && git commit && git push origin master。
3. 其余历史内容见 git legacy-publish 分支（旧 publish 独立历史已保全）。
