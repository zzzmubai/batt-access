# 第三版 MySQL 实现与验收

本记录对应 2026-10-29 的 M3/M4 工作。最终通过结果与原始日志核对后填写；完整 Qt 客户端属于 M5。

## 实现

根工程使用 MySQL 8.4 C API，不再链接 SQLite 或生成 batt-db。`Store` 保留设备登记、告警变更、查询创建/结束、HTTP 去重和历史读取入口，以 PImpl 隔离连接/语句。所有业务值通过明确长度绑定。

主线程取得固定绝对 runtime_dir 下按端口和 schema 命名的 flock 后初始化客户端库；短生命周期启动连接先核验结构，再事务结束旧 pending；Worker 在所属线程创建 TLS、连接和语句。全部 Worker ready 后才开放监听，退出先 join/关闭连接，再释放客户端库和实例锁。

登录、告警和查询仍采用候选状态 → MySQL 事务提交 → 内存发布/应答。关键写事务由进程内公共锁串行协调。MySQL 连接使用 READ COMMITTED、严格模式、明确连接/网络/行锁期限和 CLIENT_FOUND_ROWS，避免把值未改变的 UPDATE 错判为记录丢失。

公共锁等待预算 500ms，通过普通 mutex 的 try_lock 配合 5ms 休眠检查 steady_clock 和取消标记。它不是 SQL 全操作的 500ms 硬期限。此写法避开实测 GCC13/TSan 的 timed_mutex clocklock 获取路径，未增加抑制规则。

启动结构清单来自已审查的 DDL/manifest，编译期生成头文件。运行账号核验表/引擎/字段/NULL/生成列/索引/CHECK 表达式/外键/完成标记后才恢复。第一屏分页内部用 0 表示无游标，避免漏掉 INT64_MAX；HTTP 显式 beforeId 仍只接受正数。

## 验收映射

| 场景 | 独立证据入口 |
| --- | --- |
| 新库、未知库、未完成结构、错误版本、缺索引 | mysql_schema_test；http_test 的 uninitialized_and_wrong_schema |
| 登录/重复登录、first_seen 保留、ACK 后已提交 | storage_unit；persistence 网络断言与独立读库 |
| 新登录与旧 pending 同事务 | persistence 的 replacement_login，提交前/后崩溃及 SQL 故障 |
| 告警持续/恢复/重触发、整帧原子性 | persistence 的 alarm_ack、alarm_crash、failed_alarm |
| pending 先于 910、终态先于展示 | storage_unit、persistence、http_test 的 post_commit |
| 同号重试优先 busy、全局冲突、拒绝不占号 | api_unit 和真实 HTTP 同号并发/跨分片测试 |
| HTTP 丢响应/应用超时、重启找回原号 | http_test 与 demo-http |
| 旧 pending 恢复、offline/属性不 fresh、迟到不改终态 | persistence、demo-final |
| 行锁等待、运行期连接被杀、失败退出 | persistence 的 external_writer_lock、runtime_connection_kill |
| COMMIT 连接丢失分类 | mysql_resources：先杀受控事务连接，再 COMMIT，要求 unknown 且不重放 |
| 真正的 InnoDB 死锁 | mysql_resources：两线程反序持有两行，一方 1213、一方提交，无重试 |
| 64 字节/大小写/尾随空格/NULL/JSON | mysql_schema、mysql_resources |
| 64 位任务/告警游标 | http_network 的 int64_task_and_first_page_cursor |
| 同库不同 cwd 双实例 | persistence 的 same_database_different_cwd，确认旧 pending 未被恢复结束 |
| 分页过滤、空页、并发插入 | http_network 的 alarm_cursor |
| 退出排空超时及恢复失败 | persistence 的 shutdown_deadline、recovery_failure |

杀连接后的测试可以证明本次受控事务未持久化，不能推导任意 COMMIT 断网都回滚。连接断在提交应答附近时，客户端依然按结果未知处理；独立的 after_commit 进程崩溃测试证明已提交记录保留。没有进行物理断电或跨主机网络隔离验收。

SQLite DDL 原子迁移用例改为 MySQL 初始化完整性和服务启动拒绝；不保留“DDL 全部回滚”的旧保证。Python 测试夹具从独立 MySQL 连接断言，保留原业务网络断言。模拟数据导出为 JSON 证据，不称为数据库备份。

## 检测边界与原始失败

Debug 首轮 12/13：PyMySQL 将 3819 CHECK 错误归为 OperationalError，测试原先期望 IntegrityError。改为逐条检查 1062/1452/3819，保留数据库约束，复跑通过。

TSan 默认启动报告 unexpected memory mapping。进程级 `setarch x86_64 -R` 可进入程序，不修改系统全局地址随机化。随后实测 timed_mutex 的获取未被该运行时识别，出现 unlock of an unlocked mutex。改为普通 mutex 的有期限轮询后复验，未关闭锁检测或添加 suppression。GCC 对该路径的说明见 [上游记录](https://gcc.gnu.org/pipermail/gcc-cvs/2023-May/382895.html)。

ASan/UBSan、TSan 插桩的是项目 C++ 代码；MySQL Server 是外部进程，系统发行的 libmysqlclient/OpenSSL 没有重新插桩。不能宣称覆盖数据库进程内部或预编译客户端库全部行为。

## 资源基线

原始数据见 [baseline.json](test-results/v3-m3/baseline.json)。2 vCPU 虚拟机，Debug，1 秒上报，1/32 台模拟终端、1/4/16 个 HTTP 客户端。时延按创建、重放、业务响应、传输错误分别记录；RSS/CPU 是单次短时样本。MySQL 配置 flush=1/binlog=1/sync_binlog=1，未为了测试数字放宽持久化。

该基线与旧 V2 使用了不同系统、编译器和分配的 CPU，不据此声称性能提升或生产容量。
