# pvd_core

独立的 C++17 PVD 数据与回放库。核心目标是让对象树、属性检查、时间轴和后续 3D 视图共享同一套数据接口，前端可以分别接 Qt、Web 或其他 UI。

数据内核只依赖 C++ 标准库，不链接 Qt、PhysX 或图形 API。PhysX 3.4 SDK 仅用于可选的真实录制生成与集成验证。

0.2 版新增可追加会话，以及可独立启用的 `pvd::transport` TCP 模块，支持实时查询、原始录制、取消和断连安全前缀恢复。详见 [实时采集说明](docs/live-capture.md)。

0.3 版新增 `pvd::render`，包含七类几何提取、共享资源缓存、调试图元、三角网格生成和 JSON/OBJ 无界面导出。详见 [渲染数据提取](docs/render-extraction.md)。

0.4 版新增 Windows 原生 `pvd_viewer`：Direct3D 11 场景视口、帧回放、对象选择和属性检查。前端依赖保持在查看器内部。详见 [构建、操作和验证](docs/viewer.md)。

0.5 版接通 ImGui 实时监听、可选录制、最新帧跟随和历史帧检查。事件按安全前缀增量导入；停止与断连后可重新打开录制。完整应用的后续功能及逐章验收见 [实施清单](docs/application-roadmap.md)。

```powershell
cmake -S . -B build-core -G "Visual Studio 17 2022" -A x64
cmake --build build-core --config Debug
ctest --test-dir build-core -C Debug --output-on-failure
```

真实 SDK 验证：

```powershell
cmake -S . -B build-physx34 -G "Visual Studio 17 2022" -A x64 `
  -DPVD_BUILD_PHYSX34_VALIDATION=ON -DPHYSX34_ROOT="D:/Code/PhysX-3.4-master"
cmake --build build-physx34 --config Debug
ctest --test-dir build-physx34 -C Debug --output-on-failure
```

当前本地 SDK 是 Debug x64 版本，因此生成器须使用 Debug 配置。普通内核构建没有这项限制。

- [内核 API、构建和兼容范围](docs/kernel.md)
- `include/pvd/protocol.hpp`：独立的增量通信解码器及 28 类事件。
- `include/pvd/session.hpp`：反射元数据、对象生命周期、属性和帧状态查询。
- `tools/pvd_inspect.cpp`：无界面 JSON 检查工具。
- `tools/pvd_capture.cpp`：无界面 TCP 接收和录制工具。
- `include/pvd/render.hpp`、`debug_render.hpp`、`mesh_builder.hpp`：UI/GPU 无关的渲染数据模型。
- `tools/pvd_scene.cpp`：场景 JSON 检查与 OBJ 导出。
- `examples/consumer/`：通过已安装 CMake 包使用本库的独立项目。

以下保留对 `D:\Program Files\PhysX Visual Debugger 3` 的首轮静态逆向研究，作为旧版兼容的证据基线。

- [逆向结果与证据索引](docs/reverse-engineering.md)
- [协议和录制兼容笔记](docs/protocol-and-compatibility.md)
- [现代化重写架构与验收路线](docs/rewrite-plan.md)

最重要的结论：PVD 的核心是可按时间查询、前进和回退的对象图；3D、属性树、性能分析、内存分析和曲线都是这个对象图及事件通道的视图。重写应先确定数据与回放内核，再实现界面。

本地 `analysis/` 保存原始证据、提取脚本、反汇编片段、手册提取、恢复的着色器和固定提交的官方 SDK 参考；各子目录的报告注明了观察、推断与未知项。该目录及构建产物、录制数据不纳入源码仓库，研究文档中的 `analysis/` 路径指向本地证据。
