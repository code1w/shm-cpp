# 轮询错误码修订验证

本次新增代码不含try/catch/throw。配置和注册返回ErrorCode，注册使用nothrow分配直接存储可调用对象；轮询回调返回错误码，Run清理墓碑后原样返回。客户端安装错误沿连接失败路径处理，服务端安装错误回滚连接。

Release及ASan/UBSan EventLoop、ShmApp默认/两种单端轮询/双方轮询四配置通过。新增测试覆盖零检查间隔、空函数指针、非法fd标识、运行中配置拒绝、同轮移除/替换、timerfd处理、kProtocol/kNoMemory回调返回传播、运行恢复和跨线程Stop。kNoMemory传播测试为回调返回错误码，不是实际分配失败注入。

ASan/UBSan 18个尺寸/批量轮询冒烟通过，全部响应和已覆盖的资源清理检查通过。日志和命令见smoke目录，修订后源码/二进制指纹见sources.sha256。未重新做正式性能测量，之前性能报告对应原指纹版本。
