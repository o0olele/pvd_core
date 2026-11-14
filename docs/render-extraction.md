# 渲染数据提取

0.3 版新增独立 C++17 目标 `pvd::render`。它将已解析的 PhysX 3.4 帧状态转换为形状实例、共享几何资源和调试图元，并提供统一三角网格生成。库只链接 `pvd::core`，没有窗口、GPU、Qt 或 PhysX 运行依赖。

## 使用和模块

```cmake
find_package(pvd_core 0.3 CONFIG REQUIRED)
target_link_libraries(my_viewer_backend PRIVATE pvd::render)
```

```cpp
#include <pvd/render.hpp>
#include <pvd/mesh_builder.hpp>

auto session = pvd::Session::load_file("capture.pxd2");
pvd::render::Extractor extractor;
auto state = session.snapshot_at_frame(0);
auto frame = extractor.extract(session.metadata(), state);

for (const auto& instance : frame.instances) {
    auto triangles = pvd::render::tessellate(instance.geometry);
    // triangles.positions 已应用几何缩放，但仍在 shape 局部坐标。
    // instance.world_pose 把这些点变换到当前 PhysX 世界坐标。
    // UI/GPU 层可自行缓存三角缓冲，并按实例上传 world_pose。
}
```

| 头文件 | 职责 |
| --- | --- |
| `pvd/render.hpp` | 几何模型、形状实例、资源缓存、变换和诊断 |
| `pvd/mesh_builder.hpp` | 将解析几何转换为含顶点、法线和索引的 TriangleBuffer |
| `pvd/debug_render.hpp` | 接触点与 SDK immediate-render 图元、文本和约束数据 |

配置 `PVD_BUILD_RENDER=OFF` 可不构建该模块；它与 TCP 模块可分别启用。静态库及 DLL 都通过同一套 CMake 安装包消费，需使用兼容的 C++ 工具链与运行库。

## 几何和坐标约定

输出保持当前 PhysX 原生右手坐标，不自动变换上轴、单位或左右手系。`world_pose` 精确组合 Actor 的 `GlobalPose` 与 Shape 的 `LocalPose`。源对象和属性载荷不会被修改。

| 几何 | 提取结果 |
| --- | --- |
| Box | 三个半轴长度 |
| Sphere | 半径 |
| Capsule | 半径、半长；线段沿局部 X 轴，半长按 SDK 3.4 要求大于0 |
| Plane | 无限平面，局部方程 X=0；无有限 world_bounds |
| Convex mesh | 原始顶点、按 hull polygon 扇形剖分的索引、资源 ID 与 scale |
| Triangle mesh | 原始顶点、索引、可用的逐三角材质号、mesh flags 和 scale |
| Height field | 有符号高度、行列数、比例、tessellation 和 hole 处理后的顶点与索引 |

共享网格保留资源局部坐标，不烘焙 Actor/Shape pose。MeshScale 使用 SDK 的 `Rᵀ diag(scale) R`，不是简单逐轴缩放后旋转。负 determinant 会反转曲面方向，消费方需反转绕序；`tessellate()` 已处理这一步。Height field 的资源坐标是 `(row, signed height, column)`，几何比例对应 `(rowScale, heightScale, columnScale)`。

`tessellate()` 为 box/plane 生成平面法线，为 sphere/capsule 生成光滑法线，为资源网格生成平面三角法线。它烘焙几何比例并保留三角材质号，不应用世界 pose。有限极大或极小三角形的法线使用 double 中间计算，避免 float 叉积溢出或下溢。无效索引、退化三角形、非法尺寸或输出超预算会抛出异常。

无限 Plane 的三角网格只是有限预览，范围由 `MeshBuildOptions::plane_half_extent` 明确设置；这不会改变提取模型的无限平面语义。当前不生成 UV、切线或纹理。

`world_bounds` 是解析基础几何或资源顶点的包围盒，不包含 SDK broadphase/contact offset 膨胀。包含未被三角引用的资源顶点时，包围盒也会包含这些顶点。不能将其直接视为 SDK 的 broadphase bounds。

`origin_shifts` 保留捕获中的显式偏移。提取器使用显式 native-coordinate 视图：SDK 不重发的 static/sleeping/cloth 世界属性由 Session shadow 逐次平移，后续属性消息自然切换到新值。原始 pose bytes 不被改写，提取器不会再次统一减去累计偏移。详见 [origin-shift.md](origin-shift.md)。

## 形状实例和资源缓存

ShapeInstance 保留 scene、actor、shape、geometry 的 ID 和 generation，以及 shape flags、静态/动态/kinematic/sleeping 状态、可选颜色和 pickable 标记。共享 shape 挂在两个 Actor 上时产生两个独立实例，世界 pose 也分别计算。

`PickingKey` 使用 scene/actor/shape 及各自生命周期代次区分实例；它是给后续 UI/GPU 使用的身份，不是已实现的屏幕拾取器。调用者可以在 `ExtractOptions::selected` 指定这些键，得到对应 selected 标记。`scene_id` 可以过滤场景。

Extractor 的缓存以资源 ID、代次、几何种类和相关不可变属性载荷签名识别资源。顶点或索引载荷更新会使缓存失效，倒放与 ID 复用也不会误用新资源。每个网格有顶点/三角上限，height field 有单独 cell 上限，缓存条目数受 `max_cache_entries` 约束；这不是整个进程的统一字节上限。

返回帧通过 shared_ptr 持有不可变 MeshData，因此缓存清除、淘汰或之后的提取不会破坏旧帧。一个 Extractor 的可变缓存需要调用方同步；可以为各 UI 工作任务建立独立 Extractor。

损坏、缺失、未知几何及资源限制通过逐对象的 structured diagnostics 返回，其他合法实例继续提取。调用方应检查诊断，再决定展示部分场景还是停止。Cloth、particles 和未建立关系的对象不会被伪造成刚体几何。

## 调试图元和时间范围

```cpp
#include <pvd/debug_render.hpp>
const auto& marker = session.frames().at(0);
auto debug = pvd::render::extract_debug(session, marker);
```

接触点从选定快照的 `PxScene.Contacts` 读取，区分“属性未采集”和“已采集但为空”。PvdContact 的32/64位布局与源字节序分别处理；NormalForce 只有记录的 valid 标记为真时才提供，不用0替代未知数据。

Immediate-render 支持 points、lines、triangles、text、合并 Debug 命令和全部五类 typed constraint，保留原始32位 ARGB、实例 ID、来源 channel 和事件位置。joint frames 可按显式 `joint_axis_length` 转成轴线；其他约束保留 typed 数据，并说明尚未变成显示网格，不自行猜测显示尺度。

`DebugRange` 是全局事件区间 `[begin_event_count, end_event_count)`。之前的数据包只用于恢复跨包 SetInstanceId 状态，不输出旧图元，也不占当前图元预算。未知 tag 没有长度字段可供跳过，会停止当前包并给出诊断。无可靠 Scene 归属的图元在场景过滤时不会被错误归到该场景；有证据时可传 `scene_renderer_ids`。

PhysX 3.4 原生 frame 结束在当前动态 pose 更新前。显式发在 frame 结束之后的用户绘制命令也可能不属于该 frame 区间。SDK 验证使用 `validation_after_fetch` 的事件游标和明确区间核对 post-fetch 数据，不能简单累积整段捕获的绘制包来显示一帧。

## 无界面导出

```powershell
.\build-physx34\Debug\pvd_scene.exe .\build-physx34\fixtures\render_capture.pxd2 --frame 0 --debug
.\build-physx34\Debug\pvd_scene.exe .\build-physx34\fixtures\render_capture.pxd2 --frame 0 --obj > scene.obj
```

默认 JSON 输出实例身份、世界姿态、几何参数/资源数量、包围盒、缓存和诊断。`--debug` 额外输出选定范围的图元计数与诊断；`--event N --debug` 需同时传 `--debug-begin M`，避免把历史图元无意累积到当前帧。`--scene ID` 可过滤场景。

`--obj` 向 stdout 输出世界坐标的形状三角网格和法线，适合外部工具检查。OBJ 不包含接触点、文本、关节、材质外观或动画；Plane 用有限预览。导出不是 GPU/UI 渲染结果。

本轮生成的可检查文件位于 `build-physx34/fixtures/render_scene.json` 和 `render_scene.obj`。导出校验脚本是 `analysis/render-sdk-test/validate_export.py`，结果为12个实例、1,665个顶点、3,023个三角形，全部索引、法线与绕序检查通过。

## 已完成验证与边界

- Windows x64 MSVC Debug 全套12/12通过；Release DLL 六项6/6通过，无编译警告。
- 真实 PhysX 3.4 捕获12步，独立 SDK oracle 包含144条 attachment 状态、284条顶点、300条三角形法线和40条 mesh flags 参考。
- 调试数据逐值核对24点、12线、12三角形、12文本，以及48个 callback 接触点；形状共享、缓存与倒序提取也通过。
- 合成测试覆盖32位引用、大端数据、坏索引/计数/NaN、资源预算、缓存失效、HF 两种剖分与 hole、镜像绕序和极值三角法线。
- 静态和共享安装包均通过外部项目链接测试；pvd_render DLL 只依赖数据内核与系统/C++运行库。

当前没有图形窗口、GPU buffer 上传、相机投影、屏幕 picking 或完整关节 gizmo 渲染。下一层 UI 只需消费这些数据模型并实现绘制与交互，无需再次解析 PVD 协议。
