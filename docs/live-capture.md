# 实时采集与录制

0.2 版将实时功能拆成两层：`pvd::core` 提供可追加的 Session 和通信解码，`pvd::transport` 提供独立 TCP 接收器、后台任务与原始录制。两层都没有 UI 或 PhysX 运行依赖；系统 socket 和线程依赖只进入 transport 目标。

## 直接采集 PhysX 3.4

```powershell
cmake --build build-physx34 --config Debug
.\build-physx34\Debug\pvd_capture.exe live.pxd2 --port 5425
```

监听默认地址是 `127.0.0.1`。在被调试程序中使用 PhysX 3.4 的 `PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10000)` 接入。程序断开后结束采集；Ctrl+C 或 `--seconds N` 可以主动停止。输出路径必须尚不存在，采集器不会覆盖旧录制。

```powershell
.\build-physx34\Debug\pvd_inspect.exe live.pxd2 --frame 0 --objects
```

每个采集对象只接受一个连接。下一次连接应创建新的 TcpCapture 和新录制文件，避免将不同会话的类型表与对象 ID 拼在一起。`--bind` 可以指定其他数字 IPv4/IPv6 地址；端口0可请求系统分配端口，实际端口会在监听成功后输出。

## 在自己的程序中链接

```cmake
find_package(pvd_core 0.2 CONFIG REQUIRED)
target_link_libraries(my_backend PRIVATE pvd::transport)
```

```cpp
#include <pvd/capture.hpp>

pvd::CaptureOptions options;
options.port = 5425;
options.recording_path = "new-session.pxd2";
pvd::TcpCapture capture(options);
capture.start(); // 同步完成 bind/listen 和录制文件创建，失败则抛出。

auto status = capture.status();
if (status.frame_count) {
    auto state = capture.snapshot_at_frame(0);
    // state 可交给任意 UI；不是接收线程内部数据的引用。
}

// 稍后由调用方控制停止；正常对端断开也会自动结束。
capture.cancel();
capture.wait();
```

接收工作在库拥有的后台线程执行，析构时会取消并 join，不使用 detached thread。`status()`、元数据、索引和快照查询都加锁并返回副本；属性二进制载荷仍共享不可变存储。无需 UI 线程直接操作 Session。`wait()` 只等待线程收尾，后台解析、网络或录制错误通过终态和 `status.errors` 返回。

取消通过原子标记配合 socket 轮询完成，响应时间为 `poll_interval` 加当前解析、录制或查询操作耗时。没有接收者与解析者之间的无限队列；处理变慢会通过 TCP 接收窗口给发送方背压。较重的快照查询也可能暂时阻塞接收，因此 UI 应按需查询可见数据。

只使用核心时可配置 `-DPVD_BUILD_TRANSPORT=OFF`，得到没有 socket 或线程模块的库。

## 可追加 Session

```cpp
pvd::StreamDecoder decoder;
std::unique_ptr<pvd::Session> session;

// 在每次收到字节后：
decoder.feed(data, size);
if (!session && decoder.header())
    session = std::make_unique<pvd::Session>(*decoder.header(), pvd::SessionOptions{});
if (session)
    session->append(decoder.take_events());
(void)decoder.take_group_boundaries(); // 此例不自行录制，释放不再使用的组边界。

// EOF：
decoder.finish();
if (session) session->finish();
```

跨次 append 的分块属性和消息组会保留事务状态。只处理新事件，不重建此前历史；已有帧和事件位置仍可查询。`take_events()` 移出完整事件组中的事件，`event_count()` 始终是累计计数，不因提取而归零。

直接使用 Session 时，需要调用方同步 append 与查询。返回的容器元素引用不能跨 append 保留；快照拥有独立对象状态，可以继续使用。离线构造、`parse()` 和 `load_file()` 仍表示已经 finish 的会话，不能再追加。

追加失败会标记 `failed()`，禁止继续追加。库会在异常路径重建已成功接受的事件前缀，让旧状态与索引仍可查询；正常路径不重放历史。持续内存分配失败使重建也无法完成时，`state_available()` 为 false，派生状态查询抛出 `SessionRecoveryError`，原始事件、header 和有效事件计数仍可访问，调用方可以在资源恢复后重建新会话。

## 断连恢复和状态

录制的是原始 SDK 通信字节，保留完整事件组的原顺序，不转换为旧查看器容器。接收器只在“wire group 完整，并且 Session 没有待完成的分块属性或消息组”时提交录制前缀。这样不会把半个属性事务留在可重新加载的文件末尾。

`recoverable_bytes` 和 `recoverable_event_count` 描述最近的安全前缀；`recorded_bytes` 是实际成功写盘的长度。未启用录制时 `recorded_bytes` 为0，仍可查询可恢复位置。`discarded_tail_bytes` 是已接收但未进入该安全前缀的字节数量，不包括 socket 中尚未读取的数据。

| 终态 | 含义 |
| --- | --- |
| completed | 对端正常 EOF，解码和属性事务均完整 |
| cancelled | 调用方主动停止；已保存的安全前缀保留 |
| truncated | 对端复位、EOF 残缺，或未闭合的属性事务；记录错误并保留安全前缀 |
| failed | 无效协议/语义、录制 IO 失败或配置/资源限制等错误 |

未收到完整28字节头时没有可加载的录制前缀。坏数据或半开事务之后的实时状态可能比录制安全位置更靠后；要与恢复文件比较，应查询 `snapshot_at_event(status.recoverable_event_count)`。

尾部缓冲由 `max_pending_record_bytes` 限制。超过限制会明确失败，不会悄悄丢弃事件。写入失败会尝试将文件截回上一次成功前缀；若底层文件系统连回退也失败，错误会同时报告，不能承诺该文件仍可加载。这里不提供断电持久化或磁盘分页保证。

## 验证入口

```powershell
ctest --test-dir build-physx34 -C Debug --output-on-failure
```

TCP 单元测试使用回环临时端口，覆盖任意分段、残缺尾部、属性事务中断、取消监听/接收、重复端口、录制路径冲突及缓冲上限。真实 SDK TCP 测试使用 Debug x64 PhysX 3.4、eDEBUG 通道，发送600步双场景数据，验证1,200个独立帧标记、接收期间快照、历史查询与落盘后回放，并通过 SDK 的期望 CSV 核对姿态和生命周期。当前实际验证平台为 Windows x64；POSIX 分支尚未在本机运行验证。

Release 共享构建也运行协议、会话与 TCP 三项测试，且安装后的外部 CMake 项目可独立链接 `pvd::core` 和 `pvd::transport`。内核 DLL 只依赖系统与 C++ 运行库，transport DLL 另依赖内核和 Winsock；两者均不加载 Qt 或 PhysX DLL。

分配故障注入另外在已验证的 Clang/GCC 静态测试配置执行。MSVC Debug 和 DLL 配置继续执行正常及语义失败用例，不对其 CRT 异常机制注入全局分配故障，避免测试机制触发运行库对话框。

本轮最终结果：MSVC Debug 全套7/7通过；Release DLL 三项3/3通过；Clang 静态测试验证157个分配失败位置；静态与共享安装包的外部消费均通过。真实 TCP 测试得到600步双场景、1,200帧，核对128次接收期间快照和1,795个独立 SDK 对象状态，录制与离线回放一致。最终日志位于 `build-physx34/Testing/Temporary/LastTest.log`，共享库日志位于 `build-shared/Testing/Temporary/LastTest.log`。
