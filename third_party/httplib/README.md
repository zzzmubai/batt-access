# cpp-httplib v0.63.0

固定上游发布tag：v0.63.0。来源：[上游发布](https://github.com/yhirose/cpp-httplib/releases/tag/v0.63.0)、[原始头文件](https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.63.0/httplib.h)。MIT许可保留在LICENSE；编译不联网，不启用TLS/压缩等可选依赖。

原始httplib.h SHA-256：`9021001a719768893a8a6e4051c56107690e8b8e49b41e5f903534fa19ae9afb`。

本项目有一处访问权限补丁：将Server::process_and_close_socket虚函数从private暴露为protected，然后立即恢复private。HTTP解析和服务逻辑均未修改。具体差异见lifecycle-access.patch。

补丁后httplib.h SHA-256：`9f2fbd22c177c7e539c57baf2fa8227d2ec3d13a2335f6116bd7d56912457a31`。

LICENSE SHA-256：`4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b`。

用途：适配器在每条正在处理的HTTP连接上持有一个dup描述符，停止时shutdown该副本以解除阻塞收发；副本保证原fd关闭/复用后不会误操作设备连接。连接仍由上游库关闭，副本由RAII释放。

固定2条HTTP处理线程、最多8条排队连接；设备业务仍用原分片worker。上游在Linux默认使用SO_REUSEPORT，本项目覆盖为SO_REUSEADDR，使端口占用明确失败。宏限制单头部行1024字节、最多16个头部字段、URI2048字节；总字段行容量不超过16KiB。应用body限4KiB、单响应限256KiB、每连接只处理一次请求。

这是阻塞HTTP/1.1库；不得将其描述为本项目的epoll HTTP实现或自研通用线程池。使用范围为本机/SSH隧道；准入及故障验证记录见第二版验收记录。
