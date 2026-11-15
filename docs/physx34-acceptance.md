# PhysX 3.4 工作流验收

本轮按用户确定的范围，以本机 `D:/Code/PhysX-3.4-master` 的真实 Debug x64 SDK 验证原生 3.4 工作流。旧查看器保存容器、旧 SDK 协议、旧 CUDA payload 及独立 APEX SDK 留待后续兼容。没有修改 SDK 源码或二进制。

2026-11-15 的 0.9 回归为 Debug **156/156**、Release shared **52/52**。SDK/UI 关闭的静态五模块构建通过 **24/24**，core-only **5/5**、core+analysis **14/14**；最小离线查看器 **31/31**，并实际 WARP 回放真实录制。静态和共享安装包均由独立项目仅通过已安装的公开头文件与 CMake targets 消费。五个 DLL 的导入表没有 PhysX、ImGui 或图形 API；Transport 额外依赖系统 Winsock。0.7 的 126/35 项属于此前基线，已由本轮覆盖扩展。

## 已覆盖的工作流

| 工作流 | 实现与证据 |
| --- | --- |
| SDK → TCP → 在线查看 → 原始录制 → 离线重开 | 真实双场景 SDK、增量安全前缀、暂停/跟随、取消/断连、中文路径；接收期间保存独立快照并切换其他会话，原始录制继续完成 |
| 文件 → 帧回放 → 对象检查 → 场景选择 | 原生帧及明确的 after-fetch 采样；反向/随机 seek、生命周期 generation、共享 shape、七类几何、所有反射属性及引用跳转；真实 ImGui 输入和 GPU 选择高亮 |
| Contacts / constraints / queries | 原生调试通道；11 类关节绘制事件由 SDK 实际线输出对照；普通和批量 raycast/sweep/overlap 的参数、命中可用性、来源定位与真实几何预览；相同数组重新发布仍被识别为新查询 |
| 原生扩展对象 | Cloth 点/固定点/记录的 fabric 约束、粒子有效索引及速度；Articulation 父子图、inbound joints、Aggregate 成员和跨场景迁移、销毁重建与地址复用 |
| 工程分析 | CPU scope/thread/context、任意时间区间测量、native frame begin/end 耗时曲线及精确有理数 CSV；延迟 flush、内存逐帧活动与累计存量、地址复用、错误流、属性曲线和帧差异；未采集和缺失样本明确区分 |
| 保存与裁剪 | 28 类事件重编码、起点对象图、跨包分析状态、原子发布、覆盖保护和取消；命名多片段 split/trim/delete/Save As、跨标签导出及工作区恢复；刚体、Articulation、查询、cloth/particle 真实捕获均有保存及裁剪重开状态对照 |
| RepX → SDK → 官方 XML → SDK | 七类几何、共享/独占 shape、材质、六类扩展关节、原生 Articulation/links/inbound joints 和 Aggregate；场景、可见 attachment、显式选中 owner 与整组依赖；严格拒绝或明确 partial，绝不静默补选外部关节端点 |
| 工作区与显示 | 多会话、身份校验的书签/最近文件、取消加载、后台导出；工作区 v5 保留剪辑、配色和完整显示偏好并读取 v1–v4；录制相机持续跟随/自由相机/第一人称、对象跟踪、上下轴/镜像手系、真实 4x MSAA、bounds/velocity/actor/shape/COM 图层、透明显示与动态分类范围；enum/flags、精确十六进制/复制和大数组分页 |
| 显示偏好与恢复 | 状态/布料/粒子/Profile RGBA、几何显示比例、planes、face culling、orbit target、joint 尺寸；GPU 与刚体/原生点拾取遵守相同 near/far 视深度及手系；显示溢出时导航回滚，实时导入继续接受原始数据并暂停显示；全局默认与匹配源偏好、恢复内置值均有真实主窗口保存恢复验证 |
| 原点平移 | 两场景、连续小平移、静态/睡眠/活动物体、cloth/particles 与显式 camera；native/wire 双视图、正反向/checkpoint、裁剪继续回放和三种 RepX SDK 回载；9,444 项精确 float/getter 对照 |
| DPI | 96/144/192/96 的原生消息路径、字体 atlas 重建与实际 D3D11 上传、逻辑布局尺寸、返回 1x 不累积舍入；另有 1.5x 查询和 2x 场景 GPU 拾取回归 |
| 独立库消费 | SDK/UI 均关闭的静态五模块安装包、core-only、core+analysis、关闭 transport/analysis/RepX 的最小离线查看器；外部项目仅通过已安装 `find_package(pvd_core)` 与公开头文件调用 |

Articulation 录制有 41 个独立采样前缀；正反向和 checkpoint on/off 每遍对照 53,300 个 getter 字段及 738 个 shape attachment。完整 RepX 原生集合实际加载对照 28 个对象、831 个字段。显示比例的独立 SDK oracle 对照 852 个世界顶点、372 个 Fit 八角和 900 条 front ray，核验 52 个查询体及 108 条关节轴，原始属性和 RepX XML 逐字一致。对应永久测试为 `geometry_display_physx34`。见 [显示设置](display-settings.md)、[原生对象](articulations.md) 和 [RepX](repx.md)。

## 重现

```powershell
cmake -S . -B build-physx34 -G "Visual Studio 17 2022" -A x64 `
  -DPVD_BUILD_PHYSX34_VALIDATION=ON -DPVD_BUILD_VIEWER=ON `
  -DPHYSX34_ROOT="D:/Code/PhysX-3.4-master"
cmake --build build-physx34 --config Debug --parallel 4
ctest --test-dir build-physx34 -C Debug --output-on-failure --parallel 2

cmake -S . -B build-shared -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_SHARED_LIBS=ON -DPVD_BUILD_PHYSX34_VALIDATION=OFF -DPVD_BUILD_VIEWER=ON
cmake --build build-shared --config Release --parallel 4
ctest --test-dir build-shared -C Release --output-on-failure --parallel 2
cmake --install build-shared --config Release --prefix install-shared
```

安装后的 `install-shared/bin/pvd_viewer.exe` 可直接打开 SDK 原始录制；用 `--no-settings` 可禁用用户设置读写，`--ui-scale 1.5` 可显式覆盖自动 DPI。独立项目的配置与四个模块开关见 [安装包消费](library-consumption.md)。SDK 只链接到可选的测试生产器及 XML 验证器，独立库和查看器不需要 SDK 运行时。

GPU 图像来自实际 D3D11 WARP 光栅化和 backbuffer 读回，位于 `build-physx34/fixtures/viewer-*.png`，相应 JSON 保留实际像素变化、字体代次及窗口尺寸；这些构建/录制产物不进入 Git。

最终已安装的 0.9 Release 程序也从另一个工作目录实际启动。硬件和 WARP 均验证 12 个 shape、3,048 个三角形及 16 次 draw；硬件的选择/跨帧变化为 123/1,052 像素，WARP 为 121/1,054。报告与交换链截图位于 `build-installed-final/installed-{hardware,warp}.{json,png}`。共享安装包的独立 consumer 再次运行 physical/origin 两份录制，实际调用新 clips/coordinates/frame-timing API；不依赖源码树或 SDK include。

## 明确限制

- Native EndFrame 在部分 pose/velocity 更新之前；after-fetch 是显式验证采样，不能把时间轴整体偷偷平移一帧。
- SDK PVD 没有记录 cloth 表面三角拓扑和完整 fabric tether 数据，查看器展示实际记录的点/约束，不虚构表面或声称可完整重建布料仿真。Scene query 未发送的 hit status/flags 等字段同样明确为 unavailable；空命中不能自行解释为 miss。
- RepX 是记录中可序列化对象的重建，不携带 solver cache、外力、完整运行历史或完整仿真续算状态。不可恢复字段、未发布成员和选区边界由严格模式拒绝；partial 必须显式选择且诊断写入 XML。
- 已测长帧数及多 actor 的规模和数字见 [性能基线](performance.md)。这些是中等验证负载，不能代替用户的生产级 mesh-heavy 录制基准。
- 本轮原生电脑操作工具初始化和重置后仍以 `helper_unknown_error: setup refresh had errors` 退出；系统鼠标、原生文件对话框及物理混合 DPI 多显示器人工交互尚未验收。程序内真实 ImGui 输入、字体重建、Win32 DPI 消息路径和实际 GPU 输出已分别验证，不能将它们描述为已经实测物理多屏。

上述限制是实际记录与测试环境边界，不改变本轮先完成 PhysX 3.4 工作流、旧格式后续兼容的范围。
