# batt-access

Linux/C++17 电池终端 TCP/JSON 接入服务。V3 将设备档案、告警生命周期和只读查询任务迁移到 Oracle MySQL 8.4，保留固定 Worker 分片、提交后发布状态、HTTP 创建去重和游标分页。

V3.1 已包含独立 Windows Qt Widgets 运维客户端，支持单设备状态、主动只读查询和告警分页。客户端源码位于 `clients/qt`，通过 HTTP/SSH 使用服务，不连接数据库。

- [第三版操作与测试教程](docs/第三版操作与测试教程.md)：独立环境、初始化和配置。
- [第三版验收记录](docs/第三版验收记录.md)：实测结果与阶段状态。
- [第三版开发计划](docs/第三版开发计划_MySQL迁移与Qt运维客户端.md)：目标及完成门槛。
- [报文协议速查](docs/报文协议速查.md)：终端协议。

## 构建与测试

服务端使用独立 Ubuntu 24.04、GCC 13、CMake 3.20+、Oracle MySQL Server/C API 8.4；Windows Qt 工程与 Linux 构建入口独立。无需为服务端安装 Qt。

```sh
python3 -m venv .local/venv
. .local/venv/bin/activate
python -m pip install -r tools/m0/requirements.txt
export BATT_MYSQL_TEST_CONFIG=/absolute/private/mysql-test-admin.json
sh scripts/test-mysql.sh
```

测试管理账号仅获准创建/删除 `batt_test_` 前缀库，运行账号只有 DML 权限。夹具以 UUID 创建独立库，仅清理本进程登记的库。账号初始化和配置格式见教程；不要把普通业务库或旧 SQLite 文件交给测试。

测试覆盖核心状态机、MySQL 资源/配置/结构、真实 TCP、HTTP、事务故障、重启恢复和两个综合演示。Python/PyMySQL 从独立连接读库，不调用 Store 验证自身。

```sh
BATT_BUILD_DIR=build-v3-asan BATT_SANITIZE=ON sh scripts/test-mysql.sh
BATT_BUILD_DIR=build-v3-tsan BATT_THREAD_SANITIZE=ON sh scripts/test-mysql.sh
```

TSan 使用单独目录；本机 GCC13/Linux 地址布局限制与实际命令见验收记录。

Qt 构建使用 `scripts/build-qt-client.ps1`，运行依赖打包使用 `scripts/package-qt-client.ps1`。中文路径工具链限制、可运行包位置和真实 SSH 联调证据见教程及[MySQL验收记录](docs/第三版MySQL实现与验收.md)。

## 初始化与启动

管理员先创建新的空 MySQL 库，用私有初始化账号配置执行 DDL。初始化器核对完整结构后才写完成标记，不删除、修复未知库，不承诺 DDL 可整体回滚。

```sh
sh scripts/init-mysql.sh --db-config /absolute/private/initializer.ini
cmake -S . -B build-v3 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-v3 -j2
./build-v3/batt-access --db-config /absolute/private/runtime.ini --http-port 19002
./build-v3/batt-sim --demo serve --devices 1
```

运行配置从 `config/mysql.example.ini` 复制，在新主机私有文件填写并设置 `chmod 600`。固定绝对 `runtime_dir` 必须预先存在，所有实例使用相同目录和用户。旧 `--db` 参数明确报错。运行账号不需要建表权限。

CLI 保留 `status BT000`、`query BT000`、`task 1`、`alarms BT000`、`devices`、`stats`。查询成功、超时和断开结果落库后才发布；属性只保存最新内存快照。

## HTTP

默认关闭，启用后只监听 `127.0.0.1`。Windows 运维通过 SSH 本地转发访问。

| 方法与路径 | 行为 |
| --- | --- |
| `GET /v1/devices/{devId}` | 单设备状态 |
| `POST /v1/queries` | JSON devId、clientRequestId；201 已持久化创建，200 同号重放 |
| `GET /v1/queries/{txnNo}` | 持久化任务及完整响应 |
| `GET /v1/alarms?devId=BT000` | status、limit、beforeId 游标分页 |

POST 超时或丢响应时保留原 clientRequestId 重试。503/unknown 不代表未创建；同号不同设备返回冲突。HTTP 等待与设备查询分别默认 1 秒和 3 秒。编号和游标支持有符号 64 位整数。

## 边界与回退

单主机、同 Unix 用户、固定运行目录、单服务实例。公共写锁仍串行协调关键事务；数据库异常失败退出，不自动重连或重放写事务。COMMIT 连接丢失视为结果可能未知。

没有多实例、公网鉴权、遥测历史库、Redis、硬件联调或生产容量承诺。测试使用模拟终端和受控数据库故障。

V2/SQLite 保存在 Git 基线 `224a608` 及独立归档/旧机目录。该基线缺少 ApiCall 值类型定义，本分支已补齐并复现回归。V3 使用新库，不导入旧历史，不双写或自动写回 SQLite。旧教程只适用于对应基线。

第三方依赖及许可见 `third_party/`；MySQL 客户端和后续 Qt 分发遵循所选发行包许可。真实配置、密钥、库和构建生成物不进入 Git。
