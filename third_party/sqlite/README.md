# SQLite 3.51.3

来源：SQLite官方amalgamation，固定版本3510300，构建时不联网。

- 源码包：https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip
- 发行说明：https://www.sqlite.org/releaselog/3_51_3.html
- 许可：public domain，https://www.sqlite.org/copyright.html；源码头部保留原声明。
- 本项目编译sqlite3.c为静态库（SQLITE_THREADSAFE=1），shell.c编译为本地检查工具batt-db。未修改三个上游文件。
- GCC7的TSan会报告WAL头部双读/校验相关访问；源码tag-20200519-1注明可能误报。CTest仅使用tests/tsan.supp中的race:walIndexTryHdr抑制这一个已确认位置，保留其余SQLite和项目代码检测；原始未抑制失败日志保留在第四步验收记录中。上游说明：https://github.com/sqlite/sqlite/blob/master/src/wal.c 。该位置不属于本项目TSan可独立验证的范围。

SHA-256：

```text
acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4  sqlite-amalgamation-3510300.zip
9512509b1bccb7461f79bea8aad6280ae4699e925fa4804381b71f59e7efb0c5  sqlite3.c
19585c8b5230e9d4f223bf31b709ece7b6a0bb3faf00d8310625d8e58cda1b1d  sqlite3.h
4a5bde1b3e00f873caa7e0e7a4def2ea656b7dfae6cad5fb1e9c0d8f44f4397a  shell.c
```
