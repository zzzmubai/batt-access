# src 源码阅读指南

这个目录实现电池终端接入服务，以及用于验证它的模拟终端。它位于原 4G 终端的服务器一侧：接收 TCP/JSON 报文，维护设备状态、告警与参数查询，并保存关键记录。

每个源码文件开头的“文件作用”注释说明该文件负责什么、谁调用它、重点看什么；函数附近的注释解释重要边界和设计原因。

## 1. 文件索引

| 文件 | 职责 | 建议关注的入口 |
| --- | --- | --- |
| [access.cpp](access.cpp) | 服务端启动、epoll 网络循环、连接管理、CLI、正常退出 | `main`、`Server::run`、`process_object`、`drain_outputs` |
| [net.hpp](net.hpp) | 文件描述符 RAII、IPv4 地址和系统错误工具 | `Fd`、`address` |
| [json_slice.hpp](json_slice.hpp) | 从 TCP 字节流中切出完整 JSON 对象 | `JsonSlice::feed` |
| [send_buffer.hpp](send_buffer.hpp) | 有界发送缓存、短写续发和查询发送期限 | `SendBuffer::enqueue`、`flush` |
| [protocol.hpp](protocol.hpp) | 单设备登录、属性、新鲜度和当前告警集合 | `Protocol::handle`、`attributes`、`alarm_snapshot` |
| [session.hpp](session.hpp) | 多设备档案、当前连接绑定、重连替换与候选状态提交 | `Sessions::attach`、`disconnect`、`persisted_handle` |
| [query_task.hpp](query_task.hpp) | 查询创建、响应关联、超时和终态管理 | `Queries::begin`、`response`、`expire` |
| [bounded_queue.hpp](bounded_queue.hpp) | 线程间有界 FIFO、等待、关闭与排空 | `try_push`、`push_wait`、`pop_for`、`close` |
| [worker.hpp](worker.hpp) | 输入/输出消息、固定分片、线程与配置声明 | `Work`、`WorkerOutput`、`device_shard` |
| [worker.cpp](worker.cpp) | 执行分片业务，协调会话、查询和存储 | `Worker::run`、`process`、`command` |
| [store.hpp](store.hpp) | 持久化接口、共享写锁、恢复数据结构 | `Store`、`StoreShared`、`RecoveredDevice` |
| [store.cpp](store.cpp) | SQLite 表结构、参数绑定、事务、编号与启动恢复 | `schema`、`Store::write`、`initialize` |
| [console.hpp](console.hpp) | 将 JSON 日志事件转换为简洁中文控制台输出 | `quiet_console_line` |
| [sim.cpp](sim.cpp) | 模拟多台终端的上报、重连和延迟查询响应 | 报文生成函数、`Terminal`、`main` |

## 2. 先理解线程和状态归属

默认运行一个 I/O 主线程和两个固定设备分片 worker。

- I/O 主线程拥有 socket、epoll、收发缓冲和 CLI，负责解析完整 JSON 并路由任务。
- `device_shard(devId, workers)` 将同一设备固定分配给同一个 worker。
- worker 顺序消费输入，独占本分片的 `Sessions`、`Queries` 和 SQLite 连接。
- worker 不直接 `send`，而是产生 `WorkerOutput`，由 I/O 主线程执行连接操作与发送。
- 多个数据库连接通过公共写锁串行写事务；业务分片不代表 SQLite 并行写。

```text
终端字节 -> I/O 收包/切帧/解析 -> 分片输入队列 -> worker 业务处理
                                                    |
                                                    v
                                                 SQLite

终端     <- I/O 发送缓存/发送 <- 公共返回队列 <- WorkerOutput
```

输入队列满时，服务关闭本次无法入队的连接并统计过载。正常退出则停止新输入，排空已受理工作，再结束剩余查询与待发数据。

## 3. 协议与持久化边界

| 协议 | 用途 | 成功含义 |
| --- | --- | --- |
| `110 -> 111` | 登录 | 设备登记事务已提交 |
| `310 -> 311` | 属性上报 | 属性通过校验并更新到内存 |
| `410 -> 411` | 当前告警集合上报 | 本帧告警变化已提交 |
| `910 -> 911` | 服务器查询、终端响应 | pending 提交后才计划发送；success 提交后才展示成功 |

属性只保留当前内存快照；SQLite 保存设备档案、告警历史和查询记录。重启后设备先离线，旧属性不会恢复为 fresh，历史 pending 结束为 `link_lost/server_restart`。

`msgType` 表示类型，`devId` 表示设备，`txnNo` 关联请求与应答。终端上报编号与服务器查询编号是独立序列；服务器查询编号由 SQLite 分配。

## 4. 对照源码追踪三条流程

### 属性上报

```text
Server::receive -> JsonSlice::feed -> Server::process_object
-> Worker::process -> Sessions::handle -> Protocol::attributes
-> WorkerOutput -> Server::drain_outputs -> SendBuffer::flush
```

重点：一次 `recv` 不等于一个报文；四项属性各自更新时间；同一帧任一已用属性非法时整帧拒绝；311 不保证遥测历史落库。

### 告警提交

```text
Worker::process -> Sessions::persisted_handle -> 候选 Protocol 状态
-> Protocol::alarm_snapshot -> Store::alarms -> COMMIT
-> 发布候选状态 -> WorkerOutput -> 发送 411
```

重点：410 表示当前集合，持续项不重复建事件，消失项恢复，再触发产生新事件；数据库失败时不发布候选状态、不返回成功。提交后 ACK 仍可能丢失。

### 主动查询

```text
CLI query -> Server::dispatch_command -> Worker::command
-> Queries::begin -> Store::create_query -> 计划发送 910
-> 收到 911 -> Worker::process -> Queries::response
-> Store::finish_query -> 发布并展示 success
```

重点：同设备最多一个 pending；设备、当前 token、txnNo 都要匹配；达到 deadline 即超时，迟到响应不能修改终态。

首版查询默认 3 秒，从 worker 创建任务时起算，包括数据库提交和发送等待，不包括此前 CLI 排队时间。按 worker 处理响应时的时间判断，不能理解成“到达 socket 就一定算及时”。

## 5. 推荐精读顺序

1. [固定报文说明](../docs/fixtures/固定报文说明.md)和 `sim.cpp` 的报文生成函数：认清输入输出。
2. `protocol.hpp` 配合 [core_test.cpp](../tests/core_test.cpp)：理解单设备状态。
3. `session.hpp`：理解设备和连接的区别，跟踪新连接如何接管旧连接。
4. `bounded_queue.hpp`、`worker.hpp`、`worker.cpp`：理解状态归属和跨线程消息。
5. `net.hpp`、`json_slice.hpp`、`send_buffer.hpp`、`access.cpp`：理解网络收发全链路。
6. `query_task.hpp` 配合 [query_test.cpp](../tests/query_test.cpp)：理解查询终态与期限。
7. `store.hpp`、`store.cpp` 配合 [store_test.cpp](../tests/store_test.cpp)：理解提交与恢复。
8. `console.hpp` 以及 Python 网络/恢复测试：理解展示和端到端验证。

读完后尝试自行回答：为什么 fd 不能当会话身份？为什么连接在线但数据过期？为什么提交成功后仍可能没收到 ACK？为什么重启保留告警却不恢复在线？

## 6. 当前实现所用技术

C++17、STL/RAII、Linux/POSIX socket、非阻塞 I/O、epoll LT、标准库线程/互斥锁/条件变量、固定设备分片、nlohmann/json、SQLite C API、CMake/CTest。Python、Shell 和 PowerShell 用于测试、演示与开发同步。

更完整规格中有部分增强设计未进入首版。对当前能力的判断应结合源码、项目 README 和[第一版实施方案](../docs/第一版实施方案与验收清单.md)，例如首版使用断开式过载卸载、按处理时刻判断查询超时，以及 worker 的 10ms 有期限队列等待。
