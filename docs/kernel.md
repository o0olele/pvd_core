# C++ PVD 内核

`pvd_core` 是 UI 无关的 C++17 库，读取 PhysX 3.4 SDK 原始通信录制并提供反射元数据、对象状态与按帧回放查询。没有 Qt、PhysX、窗口或 GPU 运行依赖。

## 项目边界

| 模块 | 接口 | 职责 |
| --- | --- | --- |
| 通信解码 | `pvd/protocol.hpp` | 增量 feed、初始化头、事件分组、28 类带类型的事件、大小端与输入限制 |
| 会话与对象模型 | `pvd/session.hpp` | 可追加事件、类/继承/属性/消息元数据、64 位对象 ID、生命周期代次、属性值、引用和帧索引 |
| 历史查询 | `Session::snapshot_at_frame` / `snapshot_at_event` | 共享不可变载荷、检查点与事件重放；查询顺序可以向前或向后 |
| 原生坐标 | `pvd/coordinates.hpp` | 保留 last-wire 原文，并提供 OriginShift 后的实际世界值 |
| 非破坏剪辑 | `pvd/clips.hpp` | 独立命名范围、稳定 ID、场景代次及源身份校验；不依赖查看器 |
| 保存/裁剪 | `pvd/writer.hpp` | 原始事件重编码、起点对象图和原子文件发布 |
| 无界面工具 | `pvd_inspect` | 捕获摘要、帧索引、对象与属性原始值的 JSON 输出 |
| SDK 验证 | `pvd_physx34_capture` | 使用外部 PhysX 3.4 模拟生成数据和独立期望 CSV，不参与内核链接 |
| TCP 采集 | `pvd/capture.hpp` | 独立 `pvd::transport`，线程安全查询、原始录制与断连恢复，见 [实时采集](live-capture.md) |
| 渲染提取 | `pvd/render.hpp` | 独立 `pvd::render`，七类几何、实例、调试图元与网格生成，见 [渲染数据提取](render-extraction.md) |

输入是 SDK `PxDefaultPvdFileTransportCreate` 生成的原始流，magic 为 `0x31E5E882`。遇到原查看器保存容器的 `0x36101BB0` 会明确报 unsupported；当前不声称兼容旧保存容器、PhysX 3.0–3.3 或 OVD。

## 构建和安装

```powershell
cmake -S . -B build-core -G "Visual Studio 17 2022" -A x64
cmake --build build-core --config Debug
ctest --test-dir build-core -C Debug --output-on-failure
cmake --install build-core --config Debug --prefix "E:/Code/pvd/install-core"
```

`BUILD_SHARED_LIBS=ON` 可切换为 DLL/共享库。静态目标会向使用者传递 `PVD_CORE_STATIC`；共享构建使用导出宏。当前是 C++ API，STL 类型跨 DLL 边界要求调用者与库使用兼容的工具链和运行库；这不是稳定 C ABI。

其他 CMake 项目使用：

```cmake
find_package(pvd_core 0.8 CONFIG REQUIRED)
target_link_libraries(my_backend PRIVATE pvd::core)
```

通过 `CMAKE_PREFIX_PATH` 指向安装前缀即可。`examples/consumer/` 是独立消费项目，不要求源码树或 SDK include 路径。

## 读取和查询

```cpp
#include <pvd/session.hpp>
#include <pvd/coordinates.hpp>

auto session = pvd::Session::load_file("capture.pxd2");
const auto& schema = session.metadata();
const auto& frames = session.frames();
if (!frames.empty()) {
    auto state = session.snapshot_at_frame(0); // 全局第一个已完成 frame 标记
    for (const auto& [id, object] : state.instances) {
        if (auto value = object.property("GlobalPose")) {
            if (auto pose = pvd::coordinates::native_transform(*value)) {
                // pose->position 与 pose->rotation 可交给任意渲染前端。
            }
        }
    }
}
```

`frames()` 保留 `(stream_id, scene_id)` 和该组合内从零起的局部序号，同时按录制顺序形成全局标记列表。三参数 `snapshot_at_frame(stream, scene, index)` 按组合定位；一参数重载使用全局列表下标。快照是该 frame 结束事件之后的全局对象状态，不是只包含该 scene 的过滤结果。

`snapshot_at_event(n)` 表示应用前 n 个事件后的状态；0 是空前缀。`latest()` 则表示整个捕获末尾，SDK 清理代码可能已在这之前销毁场景，所以查看最后一帧应使用帧接口。

`sections()` 还提供所有已闭合的命名 section，包含流、section ID、时间戳和事件前缀位置，便于上层实现自定义时间区段分析。

PhysX 3.4 的 `ScbScenePvdClient::frameEnd()` 在发送当前动态姿态之前结束 `frame` section，源文件注释说明这是为了同步 contacts。因此这里的 frame 快照严格对应 EndSection 事件，不能直接解释为本次 `fetchResults()` 返回后的状态。集成测试分别记录模拟前帧真值，以及 fetch 后发出的独立 `validation_after_fetch` section 标记，验证两个时间点，而不通过猜测固定偏移匹配姿态。

对象 ID 保持原始 64 位值；`generation` 区分 ID 回收后的新对象。引用可能指向尚未创建或已销毁的对象，调用者应查询快照确认存在，不能把 ID 作为本进程内存指针。

## 增量输入和错误

```cpp
pvd::StreamDecoder decoder;
decoder.feed(data, size); // 可逐块来自文件或上层传输。
// 重复 feed 后：
decoder.finish();        // EOF 检查截断。
pvd::Session session(*decoder.header(), decoder.events());
```

`StreamDecoder` 接受任意分段边界；只有完整且一致的 event group 才提交事件。`DecodeError` 带字节偏移，`SessionError` 带事件序号和字节偏移。解析器失败后不能继续 feed，需要创建新实例。

`DecodeLimits` 限制 group、blob、字符串、数组和事件数量。未知协议版本、未知事件标签、未知头 flags 和旧保存容器都会显式报错，避免静默错位。属性数据保持发送端字节序和指针宽度；常见标量、Vec3、Quat、Transform、字符串、对象引用有类型化读取函数。未知自定义属性载荷保留为 opaque bytes。

属性值保持 incoming 类型，不隐式转换成 schema 中声明的数值类型；getter 读取 incoming 类型，不匹配时返回 `nullopt`，项目越界时抛出 `out_of_range`。内部 4 字节 ObjectRef 编码没有原查看器 ID 映射时保持 opaque；从发送端 VoidPtr 输入的引用才可还原为原始对象 ID。

分块属性和批量消息按各自 stream 的事务状态解释。普通 String 和消息中附加的 C 字符串由 SDK 编码规则解析，不会解引用录制中的指针。

## PhysX 3.4 验证

配置 `PVD_BUILD_PHYSX34_VALIDATION=ON` 并设置 `PHYSX34_ROOT` 后，CTest 会先生成真实 SDK 捕获，再进行回放对照。默认48步、60Hz，包含 plane、下落 box、模拟过程中创建和删除的 box，启用接触点采集。期望姿态直接来自 `getGlobalPose()`，而不是由被测解码器计算。

另有真实 `PvdDataStream` 生成的自定义捕获，用来覆盖数组、分块属性、普通与分组消息、引用、字符串和对象删除。生成器只读 SDK 并把输出、DLL 和构建产物写到本项目构建目录。

本地 SDK 的 Extensions 静态库使用 `/MTd`。基础生成器使用 `/MDd` 并只链接核心 DLL 的 import libraries；需要 Extensions 的独立生成器与验证器显式选用 `/MTd`，避免直接混用两套 runtime。无需修改或重建 SDK。DLL 的构建 CRT 不能仅从新生成器选项推定。

2026-11-14 在本机完成的0.1版初始验证（0.2版新增的实时验证见 [实时采集](live-capture.md)）：

| 验证项 | 结果 |
| --- | --- |
| MSVC Debug 静态库及 PhysX 3.4 CTest | 5/5 通过 |
| 真实物理捕获 | 48 帧、2,276 个事件；283 次正序、283 次逆序和283次无检查点对象状态对照，覆盖帧边界及 fetch 后状态 |
| SDK 自定义捕获 | 12 帧、127 个事件；48 次顺/逆序对象状态对照，16 个检查点与无检查点原始属性一致 |
| MSVC Release DLL | 构建无导出警告；2/2 核心测试通过 |
| 安装后独立消费 | 静态 Debug、共享 Release 两种外部 CMake 项目均链接、运行成功 |
| DLL 依赖检查 | 仅系统及 MSVC 运行库，无 Qt 或 PhysX DLL |

构建产物在 `install-static/` 和 `install-shared/`，测试录制及 CSV 在 `build-physx34/fixtures/`。完整 CTest 记录在 `build-physx34/Testing/Temporary/LastTest.log`。这些目录可重新构建生成，不作为源代码依赖。

## 当前限制

- 可追加会话及独立 TCP 接收器已提供；一个采集对象对应一个连接，重连需创建新会话。直接使用可变 Session 时由调用方同步，TcpCapture 则提供同步后的副本查询。
- 事件和属性载荷保存在内存，检查点数量可配置；当前没有磁盘分页、压缩或大型录制性能承诺。
- Profile / memory / errors 的历史索引、聚合、原生帧活动和 CSV 已由独立 `pvd::analysis` 提供；debug-render 解码与坐标轴等派生数据由 `pvd::render` 提供。
- OriginShift 保留原始属性字节，并为已确认的 PhysX 3.4 世界字段维护逐次 float 平移的不可变 shadow。渲染或物理读取使用 `coordinates::native_transform/native_vec3/native_value`；`as_*` 仍表示 last-wire 值，不能在 shift 后直接当作当前世界坐标。详见 [origin-shift.md](origin-shift.md)。
- Raw 保存、命名剪辑及裁剪已实现；`pvd::repx` 提供独立导出，ImGui/D3D11 只在可选查看器中。旧保存容器和旧 SDK 协议仍按用户范围后续兼容。
- 已实现源协议大小端和32/64位字段处理；真实 SDK 集成首先验证本机 x64 little-endian 3.4，其他组合以合成协议测试覆盖。

这些边界不进入 UI 层接口；后续可在独立格式适配器、分析模块和渲染提取层扩展。
