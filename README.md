# pvd_core

独立的 C++17 PVD 数据与回放库。核心目标是让对象树、属性检查、时间轴和后续 3D 视图共享同一套数据接口，前端可以分别接 Qt、Web 或其他 UI。

数据内核只依赖 C++ 标准库，不链接 Qt、PhysX 或图形 API。PhysX 3.4 SDK 仅用于可选的真实录制生成与集成验证。

可选的 [Unity 回放插件](unity/com.pvd.playback/README.md) 支持在 Windows x64 上监听 PhysX 3.4 PVD 实时流，或打开原始录制、播放和逐帧查看碰撞形状。

0.2 版新增可追加会话，以及可独立启用的 `pvd::transport` TCP 模块，支持实时查询、原始录制、取消和断连安全前缀恢复。详见 [实时采集说明](docs/live-capture.md)。

0.3 版新增 `pvd::render`，包含七类几何提取、共享资源缓存、调试图元、三角网格生成和 JSON/OBJ 无界面导出。详见 [渲染数据提取](docs/render-extraction.md)。

0.4 版新增 Windows 原生 `pvd_viewer`：Direct3D 11 场景视口、帧回放、对象选择和属性检查。前端依赖保持在查看器内部。详见 [构建、操作和验证](docs/viewer.md)。

0.5 版接通 ImGui 实时监听、可选录制、最新帧跟随和历史帧检查。事件按安全前缀增量导入；停止与断连后可重新打开录制。完整应用的后续功能及逐章验收见 [实施清单](docs/application-roadmap.md)。

可选 `pvd::analysis` 提供与 UI、GPU、PhysX SDK 无关的 CPU profile 索引、时间/线程/名称查询、内存分配/释放历史、峰值/分组、错误筛选和 CSV 导出。支持 LE/BE 和增量 Session，详见 [性能分析](docs/profile.md) 和 [内存与错误分析](docs/memory.md)。

0.6 版在 ImGui 中接入 Profiler、Memory 和 Errors 工作区，包含按线程/context 的可缩放 CPU 时间线、时长聚合、历史内存分组、错误定位及筛选后的 CSV。

分析库还提供带生命周期/gap 语义的数值属性曲线、精确 CSV 与对象/属性/相机/原点偏移的帧差异查询。见 [曲线与状态差异 API](docs/curves.md)。

`pvd::core` 的 [raw 保存与裁剪 API](docs/saving.md) 支持 28 类事件重编码、可恢复前缀、UTF-8 路径和原子发布；裁剪保留起点元数据/对象状态，分析通道通过独立 provider 重建跨包状态。

`pvd::analysis` 的 [裁剪分析状态重建](docs/crop-analysis.md) 只携带起点所需的内存字典/活动分配及 CPU 未闭合区间/最新计数器。未知或不完整源默认拒绝；显式允许 partial 时把原始警告写入导出文件。

查看器现在支持[多会话工作区](docs/workspace.md)、可取消加载、书签和最近文件；实时接收与保存始终绑定原会话。事件耗时曲线按[原始 CPU scope 结束时间](docs/profile-curves.md)归入帧，保留延迟 flush 和缺失值语义。

独立 [RepX 导出库](docs/repx.md) 可导出当前状态的刚体、七类几何、共享 shape 和材质，并用真实 SDK 重开对照。原生[布料和粒子](docs/deformables.md)提供单独的渲染数据、选择和属性视图。它们均不引入 ImGui 或 SDK 运行依赖。长录制回放的测量方法与优化记录见[性能基线](docs/performance.md)。

0.7 版补齐六类扩展关节与原生 [Articulation/Aggregate](docs/articulations.md) 的回放及 RepX 回载验证。新增[场景查询索引](docs/scene-queries.md)和[查询视图](docs/scene-query-viewer.md)、[刚体包围盒及速度图层](docs/body-overlays.md)、[显示设置](docs/display-settings.md)和[属性检查偏好](docs/inspection-preferences.md)。这些数据接口仍可通过[已安装的独立 CMake 包](docs/library-consumption.md)使用。

ImGui 主机与面板支持[每显示器 DPI 和字体重建](docs/ui-scale.md)。本轮原生 3.4 工作流的功能覆盖、重现命令和已知验收限制统一记录在 [PhysX 3.4 验收说明](docs/physx34-acceptance.md)。

0.8 版新增独立的[命名剪辑目录](docs/clip-ranges.md)、[原点平移后的原生坐标](docs/origin-shift.md)、[逐帧内存活动](docs/memory-window.md)、[第一人称导航](docs/camera-navigation.md)和[录制相机持续跟随](docs/recorded-camera.md)。查看器提供[选中范围图层、局部坐标轴与透明显示](docs/visualization-layers.md)，Profiler 可直接测量任意时间范围。新增的公开数据接口继续支持关闭 SDK/UI 后单独安装和调用。

0.9 版补齐[配色编辑](docs/palette.md)与[显示比例、剔面、平面和关节尺寸](docs/display-settings.md)，GPU/CPU 拾取共享投影裁剪语义；工作区 v5 保存全局和各录制偏好并读取 v1–v4。显示溢出会保留已发布样本并允许恢复，实时原始接收继续。当前 3.4 验收和边界见[验收记录](docs/physx34-acceptance.md)。

0.10 版修复大量对象引用追加时的平方级内存增长，检查点增加默认 128 MiB 的结构预算，并缓存暂停状态的场景对象树。复杂初始化录制的复测与内存指标见[性能记录](docs/performance.md)。`SessionOptions` 新增字段，DLL 与调用方需要一同重新编译。

0.11 版针对实际场景的播放与相机卡顿增加完整只读 `SnapshotView`、选定对象查询和直接读取视图的渲染提取入口，复用静态对象及当前场景网格；实时增量读取支持不等待后台解析锁。新增 `pvd_viewer_benchmark` 可分别测量播放、相机、GPU 完成和面板耗时，见[性能记录](docs/performance.md)。升级时需同步更新 DLL 与调用方。

查看器默认开启背面剔除，让内向碰撞网格的屋顶和近侧墙壁露出内部；`Face culling → None` 可切回双面显示。工作区 v6 会将旧版默认 None 迁移为 Back，并保留新版显式选择，详见[显示设置](docs/display-settings.md)。

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
