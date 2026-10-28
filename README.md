# 电池终端接入与运维服务（batt-access）

个人 Linux/C++ 服务端项目。业务报文参考原实习项目，本项目独立实现服务端和模拟终端，不连接企业平台，不修改原终端。

**当前版本为V2/2.0.0：HTTP运维与可靠请求。** 在第一版的TCP设备接入、固定分片worker、SQLite恢复基础上，增加本机HTTP接口、异步查询请求去重、告警游标分页和工程验证。HTTP通过--http-port显式启用，仅监听127.0.0.1；可通过SSH隧道访问。这是模拟环境验证。

## 先看这里

- [第三版开发计划：MySQL迁移与Qt运维客户端](docs/第三版开发计划_MySQL迁移与Qt运维客户端.md)：M0环境准入已通过，进入数据库结构验证；可运行业务版本仍为V2。
- [第三版操作与测试教程](docs/第三版操作与测试教程.md)、[第三版验收记录](docs/第三版验收记录.md)：独立环境准备、准入程序、实测范围与待完成项。
- [第二版测试教程](docs/第二版测试教程.md)：先读这份；自动演示、HTTP调用、SSH转发与源码学习。
- [第二版验收记录](docs/第二版验收记录.md)：依赖准入、故障、迁移、检测构建及测量原始证据。
- [第二版实施方案与验收清单](docs/第二版实施方案与验收清单.md)：范围、接口、线程及12项验收映射。
- [最终演示与项目讲解](docs/最终演示与项目讲解.md)：一键整体验收、保存证据、讲解与学习入口，先读这份。
- [最终验收记录](docs/最终验收记录.md)：F01—F06、最后一次检测构建和原始结果。
- [固定报文与手工预期](docs/fixtures/固定报文说明.md)：直接核对字段、单位、告警集合与查询状态。
- [第四步测试教程](docs/第四步测试教程.md)：保留数据库阶段的操作步骤，当前测试组数以本README为准。
- [第四步验收记录](docs/第四步验收记录.md)：事务、故障、恢复与检测构建证据。
- [第三步测试教程](docs/第三步测试教程.md)：成功查询、超时迟到、CLI和线程对照，保留历史步骤。
- [第三步验收记录](docs/第三步验收记录.md)：M3基础测试与三类构建证据。
- [交互显示修复记录](docs/交互显示修复记录.md)：安静模式、中文结果与新增控制台测试。
- [第二步测试教程](docs/第二步测试教程.md)：历史32终端、重复登录与数据过期演示。
- [第二步验收记录](docs/第二步验收记录.md)：本轮实际测试与B01—B05映射。
- [Ubuntu联调与第一步测试教程](docs/Ubuntu联调与第一步测试教程.md)：连接、路径及历史单设备演示。
- [第一步验收记录](docs/第一步验收记录.md)：本次实际环境、测试与限制。
- [第一版实施方案与验收清单](docs/第一版实施方案与验收清单.md)：四步范围和全部验收项。
- [完整实现规格](docs/BMS主机侧协议网关实现规格.md)：详细参考；明确降级的项目以第一版方案为准。

## 当前开发环境

| 项目 | 当前值 |
| --- | --- |
| Windows源码 | `E:\4G模块\batt-access` |
| VirtualBox虚拟机 | `ubuntu`，Ubuntu18.04.2 |
| SSH连接 | Windows `127.0.0.1:2222` → 虚拟机SSH22，用户book |
| Linux第二版源码和运行目录 | `/home/book/batt-access-v2` |
| 工具 | GCC7.4、CMake3.10.2、Python3.6.9 |
| 业务连接 | Linux内部 `127.0.0.1:19001` |
| 可选HTTP接口 | Linux内部 `127.0.0.1:19002`，必须显式启用 |

Windows是源码主副本，Linux是同步后的编译/运行副本，避免在两边同时修改同一个文件。SSH22/转发2222用于开发连接；19001用于项目程序间通信，是两条不同连接。

## 在Ubuntu中一键构建和测试

```sh
cd /home/book/batt-access-v2
sh scripts/test-linux.sh
```

脚本兼容当前CMake3.10。成功时10/10测试组通过；第一版原有7组保留，新增api_unit、http_network、http_demo。测试使用自己的临时库和独立端口。

第二版HTTP演示：`python3 scripts/demo-http.py`。可加`--output-dir artifacts/http-001`保存日志和SQLite备份，目录必须是新的；脚本只终止自己启动的服务。

四类接口：GET /v1/devices/{devId}、POST /v1/queries、GET /v1/queries/{txnNo}、GET /v1/alarms。POST必须提供devId和clientRequestId；新建201，同请求号/同设备重放200，同号不同设备409。201只表示任务资源提交成功；设备结果需随后GET读取。

只运行最终演示：

```sh
python3 scripts/demo-final.py
```

看到七个阶段和最终result=PASS。数据过期环节明确使用2000ms测试阈值（正常服务90秒），查询仍为默认3秒，迟到应答安排在5秒。不据此宣称并发吞吐。完整操作和可保留数据库的参数见最终演示教程。

检测构建也运行全部10组，复现：

```sh
BATT_BUILD_DIR=build-asan BATT_SANITIZE=ON sh scripts/test-linux.sh
BATT_BUILD_DIR=build-tsan BATT_THREAD_SANITIZE=ON sh scripts/test-linux.sh
```

TSan仅抑制SQLite上游注明可能误报的walIndexTryHdr共享内存访问，其余项目与SQLite代码继续检测；不能称为未抑制全量零告警。依据和初始失败日志见第四步及最终验收记录。

单独复现崩溃恢复：`python3 scripts/demo-persistence.py`。脚本使用独立端口和临时数据库，只终止自己创建的服务，不影响手动演示数据。

## 在两个MobaXterm终端里演示

终端A（保持运行）：

```sh
cd /home/book/batt-access-v2
./build/batt-access --db access-v2.db --http-port 19002 --quiet
```

终端B（连接同一台Ubuntu）：

```sh
cd /home/book/batt-access-v2
./build/batt-sim --demo serve --devices 1
```

B显示serving并持续上报。在正在运行服务的A中输入 `status BT000`、`query BT000`，应看到pending随后success。用 `task 实际txnNo` 查看结果。命令输入服务程序，不是新开Linux shell执行query。

B按Ctrl+C后，改用 `./build/batt-sim --demo serve --devices 1 --query-delay-ms 5000`，可演示3秒超时及5秒迟到。32设备普通回归仍可运行 `./build/batt-sim --devices 32`，旧relogin演示仍保留。

交互终端默认安静模式，隐藏持续属性/告警输出；status显示[STATUS]，query显示[QUERY]和[RESULT]。用quiet on/off切换，--verbose保留详细JSON。服务内还支持help、devices、alarms、stats、quit。正常结束建议先停B，再在A输入quit；使用同一路径的数据库重启后，task可查看旧结果，alarms可查看最近100条历史。

SQLite源码随项目编译，不需要sudo、apt或单独数据库服务器。构建还生成batt-db工具；在另一个Linux shell中可执行：

```sh
./build/batt-db -readonly -header -column access-v2.db "SELECT txn_no,dev_id,status,reason,client_request_id FROM query_task ORDER BY txn_no;"
```

111成功表示设备登记已提交；311仅表示属性已在内存接纳；411成功表示本帧告警变化已提交。任务pending提交后才发910，成功结果提交后才展示success。所有SQL用户字段使用参数绑定，每个worker使用自己的连接；公共写锁串行化事务，两个worker不会并行写SQLite。

## 源码导航

```text
src/json_slice.hpp    跨recv的JSON对象边界；不是完整JSON解析器
src/protocol.hpp      单设备业务状态、属性校验、数据年龄、当前告警集合
src/session.hpp       设备档案、当前连接绑定、合法替换、旧关闭通知隔离
src/bounded_queue.hpp 有界队列、条件变量、关闭/排空与等待者唤醒
src/query_task.hpp    查询状态、响应关联、截止点、提交回调与缓存
src/api.hpp           带类型命令、有界入口、一次性结果上下文与应用期限
src/http_api.hpp/.cpp HTTP适配、参数校验、有界库线程、慢连接停止与计数
src/store.hpp/.cpp    SQLite连接、参数绑定、事务、编号与启动恢复
src/worker.hpp/.cpp   固定设备分片、业务所有权、命令和定时检查
src/send_buffer.hpp   有界发送缓存、短写、临时不可写及查询发送期限
src/net.hpp           socket地址与fd析构释放
src/access.cpp        多连接epoll LT服务端，boot_id+递增token，独占socket
src/sim.cpp           默认32终端、重复登录、持续服务及延迟911
tests/core_test.cpp   不依赖真实socket的确定性测试
tests/network_test.py 独立Python客户端的真实TCP测试
tests/query_test.cpp  查询、队列、发送期限的确定性断言
tests/query_network_test.py CLI、业务与线程协作端到端测试
tests/store_test.cpp  数据库及关键缓存失败回滚的独立断言
tests/persistence_test.py 提交边界崩溃、重启、SQL约束及有期限失败
scripts/test-linux.sh 一键构建和测试
scripts/demo-persistence.py 独立端口/临时库的崩溃恢复演示
scripts/demo-final.py 32终端七段综合演示，可导出步骤/日志/SQLite备份
scripts/demo-http.py  响应丢失、同号重试、重启及冲突的HTTP演示
scripts/measure-http.py 轻负载/持续上报/短时突发的测量基线
tests/api_test.cpp    请求上下文、容量、超时晚到及跨分片去重断言
tests/http_test.py    独立HTTP/TCP/SQLite客户端的接口与故障验证
scripts/compare-workers.py 单独复现1/2个worker慢业务对照
scripts/sync-to-vm.ps1 Windows到虚拟机的源码同步
```

JSON固定v3.11.3、SQLite固定3.51.3、cpp-httplib固定v0.63.0随源码提供，构建不联网。HTTP库有一处允许生命周期接管的访问权限补丁，补丁、许可及哈希见third_party/httplib。HTTP使用库的阻塞I/O和固定2条处理线程，不称为自研epoll HTTP框架或通用业务线程池。

设备默认最多128条活跃或等待关闭通知的连接，每分片输入64条、返回合计128条；两个worker时每分片最多512个设备档案、1024条任务缓存。HTTP另有8条待处理连接、16条应用命令入口、最多160个仍被业务持有的请求上下文；队列满可拒绝/断开。单头部行1024字节、最多16个字段、body4KiB、响应256KiB。旧终态任务可逐出缓存，仍能从数据库查到；告警内存只保留open，SQL历史没有自动归档。

数据库使用本地文件、WAL、synchronous=FULL、foreign_keys=ON；同一库只允许本服务一个进程持有。锁等待和SQLite busy使用500ms总预算；失败回滚、不发关键成功/未提交910，并报错非零退出。正常退出预算5秒，超时输出unfinished统计；操作系统不可中断磁盘I/O不承诺硬截止。

查询默认3秒，从worker创建任务时开始，包括提交和发送等待；按worker处理911的时刻判定，处理排队过久仍可能timeout。重启保留已提交记录，旧pending不自动重发；提交后ACK丢失可由终端重发集合收敛。查询编号跨重启保证要求保留同一数据库；删除/回滚数据库备份不保留这项保证。

## 同步与后续步骤

后续代码修改后同步到Ubuntu，再运行一键测试。Windows同步脚本支持 `-IdentityFile` 和 `-KnownHostsFile`，凭据文件放项目目录之外；也可用MobaXterm的SFTP同步源码，排除build目录。详细操作见教程。

当前A/B/C/D/F阶段均已完成第一版验收，原始记录见docs/test-results。旧阶段文档是当时的历史状态，以本README和最终记录判断当前能力。

第二版接口、去重、迁移及工程验证见第二版验收记录。HTTP应用等待默认1秒，设备查询默认3秒；HTTP超时/断开不证明任务未提交，应使用同一clientRequestId重试，不能换号盲目重发。去重只保证数据库中仍保留的记录；CLI任务的client_request_id为NULL。

数据库schema自动从1升级到2，新增请求号列和索引。开发/演示使用新的access-v2.db，原Linux第一版目录及手动库保留。Windows源码更新前的V1源码快照放在.local/v1-before-v2-20261023.zip，仅备份源码，不包含运行数据库。若要迁移旧库，先停服务并用SQLite Backup API备份，在副本验证；V1程序不读取schema2。

尚无真实硬件或企业上线结果；不提供公网访问保证、多用户权限隔离、MQTT云转发、通用线程池、无损背压或长期容量结论。eventfd仍为可选后续优化。现有学习注释与历史文档保留，以本README和第二版记录判断当前版本能力。
