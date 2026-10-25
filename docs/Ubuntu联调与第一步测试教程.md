# Ubuntu联调与第一步测试教程

> 当前M3已完成，最新测试和交互查询步骤见[第三步测试教程](第三步测试教程.md)。本篇保留初次配置与历史M1演示。

> 这是第一步的历史教程。当前已完成M2，自动测试扩展为402个核心断言、19个网络场景；新的复现入口见[第二步测试教程](第二步测试教程.md)。下面的单设备命令已补--devices 1，可继续复现第一步。

## 1. 先认识三个东西

- VirtualBox负责运行Ubuntu，它提供真正的Linux内核。
- MobaXterm是Windows上的终端和文件传输工具，通过SSH操作Ubuntu。MobaXterm自己的Local terminal不等于虚拟机里的Ubuntu。
- 我在Windows编辑源码，通过SSH/SFTP同步到同一台Ubuntu，执行编译、测试并查看输出。你在MobaXterm执行的也是同一套程序，不需要再配另一台机器。

本项目服务端使用epoll，必须在Linux编译。不要直接在Windows的MinGW里编译，也不要把Linux生成的可执行文件拿回Windows双击。

```text
Windows：E:\4G模块\batt-access（源码主副本）
        │ SSH / 文件同步（127.0.0.1:2222）
        ▼
VirtualBox中的Ubuntu：/home/book/batt-access
        ├─ 终端A：batt-access 服务端，监听127.0.0.1:19001
        └─ 终端B：batt-sim 模拟终端，连接127.0.0.1:19001
```

业务程序都在虚拟机里，19001不需要向Windows做端口转发。SSH用2222，项目业务用19001，不要混用。

## 2. 启动和登录

1. 打开VirtualBox，启动名称为ubuntu的虚拟机；若已经运行，不要再启动第二台。
2. MobaXterm选择Session → SSH。
3. Remote host填127.0.0.1，勾选Specify username并填book，Port填2222。
4. 使用你原来的Ubuntu密码登录。成功后提示符类似 `book@100ask:~$`。
5. 可以打开两个相同SSH会话，分别当终端A和B。

先在任意SSH终端确认环境：

```sh
whoami
uname -s
pwd
```

应分别看到book、Linux，以及Linux目录。Windows PowerShell的 `PS E:\...>` 和Moba本地终端不是这里要执行构建命令的位置。

已确认你的VirtualBox使用NAT转发：宿主机2222 → 来宾22；不需要修改网卡、配置桥接或重装系统。虚拟机运行时保持Moba可连接即可。

## 3. 第一次先只运行自动测试

在Ubuntu SSH终端粘贴：

```sh
cd /home/book/batt-access
sh scripts/test-linux.sh
```

每行含义：cd进入Linux项目副本；sh执行项目脚本；脚本配置CMake、编译三个目标并运行CTest。

成功输出类似（测试组仍为2，组内场景随阶段增加）：

```text
Test project /home/book/batt-access/build
    Start 1: core
1/2 Test #1: core .............. Passed
    Start 2: network
2/2 Test #2: network ........... Passed
100% tests passed, 0 tests failed out of 2
```

这不是只检查“能编译”：core内部包含多个边界断言，network初版有8个独立场景、当前已扩展到19个。测试自动启动临时服务并自动清理，不需要先手动启动服务器。

当前CMake为3.10.2，不支持原示例中的新式-S/-B和ctest --test-dir用法。需要手动拆开时执行兼容写法：

```sh
cd /home/book/batt-access
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -- -j2
ctest --output-on-failure
```

只查看核心测试详情：

```sh
cd /home/book/batt-access
./build/core_test
```

只运行网络测试：

```sh
cd /home/book/batt-access
python3 tests/network_test.py ./build/batt-access ./build/batt-sim
```

网络测试需要约5秒，因为其中一项真实等待5秒的未闭合帧超时。看到临时静默不代表卡死。

## 4. 亲眼看一次双终端通信

终端A启动服务，保持这个窗口运行：

```sh
cd /home/book/batt-access
./build/batt-access
```

第一行JSON应包含 `"event":"ready"`、`"port":19001`、`"persistence":"memory_only"`。当前M2默认max_clients为128，原M1才是1；这是服务已监听的标记。

终端B执行：

```sh
cd /home/book/batt-access
./build/batt-sim --devices 1 --slice 1
```

模拟器把每次应用send限制为一个字节，服务端自己组帧。TCP仍可能合并这些字节；不能根据send次数断言recv次数。

B应该收到8个应答：一次111登录、一次311属性、六次411告警。最后一行包含 `"event":"demo_complete"`。

A中可观察到：

| 事件 | 应看到的行为 |
| --- | --- |
| login | BT000登录一次 |
| attributes | 电压52.8V、电流-1.25A、SOC80、温度28℃；状态online_fresh |
| alarms | 同一03告警持续上报，history不增加；加入01后open变2 |
| alarms | 移除03仅清03，000清空全部；再次03新建事件 |
| closed | 全新服务首次执行此单设备演示，alarm_history为3、open_alarms为1；M2重复演示会累积该设备的进程内历史 |

终端B退出后服务仍在等连接，可重新运行模拟器。第一步新连接会新建自己的内存会话，暂不保留上一连接历史。A按Ctrl+C结束，应显示stopped。

M1当时只允许一个活跃终端；当前M2已支持多设备。这里显式使用--devices 1复现原演示，多设备及替换操作见第二步教程。

## 5. 看懂第一步测试覆盖了什么

| 验收项 | 如何验证 |
| --- | --- |
| A01固定协议与单位 | core和network独立样例核对111/311/411、52.8V、-1.25A等 |
| A02分片与连续对象 | core遍历单切分位置、单字节及随机分片；network使用真实TCP |
| A03坏JSON与残帧 | 已配平坏对象不吃掉后续；未闭合对象真实等待5秒后断开 |
| A04字段与原子更新 | SOC101整帧拒绝，电压不部分更新；未知浮点字段被忽略 |
| A05告警集合 | 持续去重、减少一项、组缺席、000清空、恢复再触发 |
| A06短写与缓存 | core注入EINTR/EAGAIN/EPIPE和短写，network限制应答发送块大小 |

core使用可控单调时间测试90秒新鲜度，不必人工等待90秒。A06临时不可写由可重复的发送器注入验证，不能宣称已经做过所有真实网络拥塞场景。

## 6. 我修改代码以后怎么同步

约定：Windows是源码主副本，Ubuntu只编译和运行。这样你看到的Windows项目与我交付的内容一致。不要同时在Linux修改同一个源码文件，下一次同步会覆盖它。

我可以使用项目专用SSH公钥同步并测试；私钥留在项目目录外，源码里不包含密码或私钥。你不需要把Ubuntu密码发送到聊天里。

如果你自己同步，可以在Windows PowerShell执行：

```powershell
cd 'E:\4G模块\batt-access'
.\scripts\sync-to-vm.ps1
```

未配置SSH免密时会在你自己的终端询问Ubuntu密码。也可以显式传 `-IdentityFile` 指定你自己持有的私钥；脚本不会自动读取Moba保存的密码。首次主机指纹确认需要核实，不能把密码填到脚本里。

脚本只打包源码、文档和固定依赖，排除build、.git、.local和数据库，不删除Linux里的旧文件；将来删除/重命名源码时需额外核对副本。若PowerShell执行策略阻止脚本，可以使用Moba左侧SFTP把修改文件同步到 `/home/book/batt-access`，保留目录结构；不要复制Windows编译产物。

同步完成后回Ubuntu运行：

```sh
cd /home/book/batt-access
sh scripts/test-linux.sh
```

不需要共享文件夹、不需要把Windows的E盘路径直接当Linux路径使用。

## 7. 常见输出与排查

| 现象 | 检查 |
| --- | --- |
| SSH connection refused | VirtualBox的ubuntu是否启动，地址是否127.0.0.1、端口是否2222 |
| SSH permission denied | 用户是否book；密码/公钥是否匹配，这是登录问题，不是C++编译错误 |
| cd找不到目录 | 是否真正登录Ubuntu，以及源码是否已同步到/home/book/batt-access |
| CMake说build源目录不存在 | 不使用-S/-B，改运行sh scripts/test-linux.sh |
| bind/listen: Address already in use | 是否有另一个batt-access占19001；回原终端Ctrl+C，或两边都改用19002测试 |
| 模拟器connect失败 | 服务是否先运行、两程序是否在同一Ubuntu、业务端口是否一致 |
| connection_rejected | M1已有一个客户端，先退出旧模拟器 |
| make Permission denied | 检查Linux源码和build是否属于book，不要随意sudo构建 |

替代业务端口时两边都要改：`./build/batt-access --port 19002`，以及 `./build/batt-sim --port 19002 --slice 1`。19002这里只是演示替代端口，第一步没有模拟器控制服务器。

报错时提供运行的完整命令和终端文字即可。自动测试失败可以查看 `/home/book/batt-access/build/Testing/Temporary/LastTest.log`。

## 8. 本轮边界和下一步

本篇记录第一步单设备内存闭环；当前多连接与connection_id隔离也已完成。第三步将引入分片工作线程/查询，第四步才有SQLite与恢复。现在重启服务仍丢失状态。

工具依据：[MobaXterm文档](https://mobaxterm.mobatek.net/documentation.html)、[VirtualBox网络说明](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/networkingdetails.html)。地址与版本来自本机实际检查，不是通用默认值。
