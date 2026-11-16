# Native viewer

`pvd_viewer` 是 Windows 原生 C++ 查看器，使用 Win32、Direct3D 11 和 Dear ImGui。数据通过独立的 `pvd::core` / `pvd::render` / `pvd::transport` 读取；这些库不依赖窗口、ImGui 或 GPU。界面支持离线检查和实时 TCP 采集。

## 构建和打开

需要 Visual Studio 2022 的 C++ 工具链和 Windows SDK；依赖源码已经放入 `third_party/imgui`，CMake 构建不访问网络。

```powershell
cmake -S . -B build-physx34 -G "Visual Studio 17 2022" -A x64 `
  -DPVD_BUILD_VIEWER=ON -DPVD_BUILD_PHYSX34_VALIDATION=ON `
  -DPHYSX34_ROOT="D:/Code/PhysX-3.4-master"
cmake --build build-physx34 --config Debug --parallel 4
& .\build-physx34\viewer\Debug\pvd_viewer.exe .\build-physx34\fixtures\render_capture.pxd2
```

普通查看器构建不需要 PhysX SDK。`PVD_BUILD_VIEWER=OFF` 可完全关闭前端。Windows 上启用 render 模块时默认构建查看器；其余平台默认关闭。

可通过 `cmake --install build-shared --config Release --prefix install-shared` 将 Release 查看器与库安装到同一 `bin` 目录。共享构建的开发输出也会自动复制必要的 core / render DLL 到查看器目录，脱离构建工作目录启动即可。

支持 **File → Open capture...**、Ctrl+O、拖入文件或命令行路径。录制在独立线程加载，加载失败会显示错误并保留原来的文档。Windows 文件路径通过 UTF-8 / UTF-16 转换处理。

## 操作

- 默认进入全宽的 **Scene** 视口。顶部第一行为 46dp 的应用栏，包含 **File / Workspace / Help**；第二行为 42dp 的录制栏，左侧是带文件图标和选中底边的录制标签，右侧为 **Scene / Analysis / More**。**Analysis** 提供 Profiler、Memory、Errors、Property curves、Event curves 和 Frame diff；**More** 提供 Deformables、Scene queries、Named clips、Recorded scene 和 Workspace。**Workspace** 菜单提供标签、书签、工作区保存/载入和布局重置。
- **File** 菜单集中提供 Open capture、Live capture、Save / crop 和 Export RepX；可选模块关闭时，对应入口不显示。底部状态入口显示 **Ready** 或当前任务状态，点击后打开 **Activity**，汇总加载、导出、实时采集、通知和错误；后台任务不会在视口上方额外增加状态行。
- 45dp 高的视口工具栏使用线性图标。左侧图标的提示为 **Scene objects**，打开对象树浮层；右侧图标的提示为 **Object properties**，打开属性浮层，两者互斥。再次点击当前图标、点击浮层右上角关闭图标或按 Escape 可关闭。浮层覆盖场景，不改变渲染纹理尺寸或相机视角；在浮层内点击、滚动不会操作背后的场景。
- 场景树、视口选中和属性检查器共享同一个对象选择状态；共享 shape 以 scene / actor / shape 及 generation 区分实例。选中对象后，视口下方显示真实对象名和 **/ Selected**，点击该摘要打开属性浮层；名称缺失时使用捕获类型作为回退。
- 视口右键拖动原地转向、Alt+左键拖动环绕目标、中键拖动平移、滚轮缩放；悬停视口用 WASD/QE 移动，长按渐进加速，Shift 额外加倍。单击选择、双击聚焦选中对象、F 聚焦场景。
- 场景选择位于视口工具栏的下拉框；其他工作区也可通过 **More → Recorded scene** 切换。底部时间轴为 96dp 的两行布局：第一行是首尾、逐帧、播放图标、帧号、采样点与速度；第二行是细轨、圆形游标和整数帧刻度。采样位置明确显示为 **Frame end** 或 **After simulation**。滑轨保留无符号 64 位帧索引，Ctrl 点击可输入精确帧号，也支持键盘导航。**…** 图标打开 Playback options，包含 Loop、Reverse 和可用的录制标记间隔；窄布局的采样点选择也在其中，当前采样点显示在帧号旁。Space 播放或暂停，方向键逐帧，Home / End 跳转首尾。
- 属性检查器先展示名称、类型、可复制的 ID 和 Actor / Shape / Scene 切换，再按实际捕获字段显示 Transform、Dynamics、Collision 或 Scene 摘要。**All properties** 展开完整原始属性表，保留属性搜索、分页数组、对象引用跳转和精确复制；没有可用摘要字段的对象默认展开原始属性。右上角齿轮图标（提示 **Number format and precision**）打开 Inspector preferences，集中设置数值格式、精度与数组分组，摘要与原始表使用同一套格式和复制规则。
- **Fit** 按钮适配当前可见场景。**Display** 菜单中的 Grid / Wireframe / Contacts 控制常用图层；**Display → Display and camera settings...**（Ctrl+Shift+S）打开分为 **Scene / Camera / Display / Layers** 四组的设置弹窗，各组内容独立滚动。**Display → Diagnostics** 打开诊断浮层，显示提取阶段遇到的不完整或未支持数据。
- 调试 points / lines / triangles 由 D3D11 绘制，调试文字由 UI 投影到相应世界位置。

默认时间点是 PVD 原生帧事件边界。PhysX 3.4 验证程序另外写入的 after-fetch 标记可显式切换；普通录制没有这些标记时不会推测同等语义。播放速度单位为捕获帧/秒，不作为原始仿真时间的推断。无限平面采用有限预览网格，录制中的几何与位姿保持原始世界坐标。

查看器主机采用经实际截图校准的 17dp 原生基准字体，顶部、工具栏和图标尺寸随 DPI 同步缩放。原生字体的字形高度与浏览器 CSS 字号不直接等同。视口的内置背景为深灰蓝，选中轮廓使用薄荷色；录制自带的 shape 配色仍保留，已有的背景与配色偏好继续生效。

内置菜单、按钮、提示和状态文案统一使用英文；捕获文件中的对象名称、属性值和录制标签保留原始文本。下拉菜单、上下文菜单与弹窗使用一致的背景、边框、圆角和条目间距，不继承工具栏的紧凑行间距。

## 实时采集

点击 **File → Live capture...** 设置监听地址（默认 `127.0.0.1`）、端口（默认 `5425`）和录制路径，再点击 **Start listening**。从 PhysX 3.4 应用连接该地址即可接收。端口 `0` 请求系统选择可用端口，绑定结果显示在底部 **Activity** 详情中。录制文件以独占方式创建，不覆盖现有文件。

点击底部任务状态以打开 **Activity**，可使用 **Follow live** 和 **Stop**。**Follow live** 自动显示当前场景最新的完整帧；手动拖动时间轴、逐帧或播放历史会暂停跟随。接收及录制仍继续，可以随时重新打开跟随。**Stop** 取消网络接收，界面继续导入已接收的安全事件前缀，然后保留当前会话供回放。Activity 详情显示连接、流量、帧数、待导入事件以及错误；悬停实时状态可查看接收、录制、安全前缀和丢弃尾部字节。

```powershell
& .\build-physx34\viewer\Debug\pvd_viewer.exe --listen 5425 --record .\captures\new-session.pxd2
```

网络线程负责接收、解码和录制。主线程默认每 100ms 通过 `TcpCapture::poll_from` 获取最多 16,384 个新事件、8 MiB 的载荷预算，追加到现有 `Document`，不重新加载整个录制。单个事件可超过载荷软预算以保证前进，其大小仍受解码上限限制。未结束的属性事务不会替换视口状态。暂停检查历史时保留原来帧的快照及几何缓存。

关闭 `PVD_BUILD_TRANSPORT` 时仍可构建完整离线查看器，界面不会显示实时采集入口。

## 保存与裁剪

**File → Save / crop...** 或 Ctrl+S 打开保存面板，可保存当前完整安全事件前缀，或选择当前 scene 的原生帧范围（含首尾，界面从 1 开始）。同一全局事件区间内其他 scene/stream 的数据仍保留。裁剪会重建起点元数据、活动对象和属性，不复制全部旧运动历史来冒充裁剪。

任务在后台准备和写出，底部 **Activity** 详情显示实际阶段、耗时、取消请求及最终事件/字节数量，并可打开 Export details。任务期间源 Session 不追加或替换；在线接收和直接 raw 录制独立继续，视图在保存后追上新数据。不能把快照写到正在使用的 raw 录制文件路径。取消、写出失败和被拒绝的覆盖均保持已有目标文件不变。

内存裁剪只合成起点字典和活动分配，累计/峰值从该基线重新计数，allocation generation 在新录制内重基。默认拒绝不完整内存源；显式允许 partial 时仅接受已有分配未被观察到的 free，并把警告写进标准 ErrorMessage，重开后仍可在 Errors 查看。CPU 裁剪保留未闭合 scope 和最新计数器，跨包 stop 不丢失。详情见 [保存 API](saving.md) 和 [分析状态重建](crop-analysis.md)。

## 工程分析工作区

录制栏右侧的 **Analysis** 菜单切换 Profiler、Memory、Errors、Property curves、Event curves 和 Frame diff，**Scene** 返回视口。分析通过可选的 `pvd::analysis` 库提供；关闭 `PVD_BUILD_ANALYSIS` 时仍可使用场景查看及实时采集。

- **Profiler** 按 zone、thread、context 分开绘制 CPU 区间，可搜索事件名、过滤线程/zone、限制在当前帧接收到的 profile 包；滚轮缩放时间范围、中键平移。选择区间可查看原生 tick 和校准时长，双击定位接收该包的场景帧。Summary 为包含嵌套时间的 inclusive 聚合，Intervals 为可虚拟滚动的明细。CSV 使用同样的筛选，保留原始计数。GPU、丢失配对、损坏包及 SDK value 事件的时间/截断限制均会报告。
- **Memory** 在当前帧或整份录制的事件前缀查询分配历史与累计/峰值，保留原始地址、generation、类型、文件和行号。History 可按文本、active-only 筛选并分页；Groups 按类型、文件及行号分组。页头总量和 Groups 覆盖选中前缀的全部分配，历史筛选仅作用于 History 及其 CSV；两种 CSV 入口分别导出相应范围。不完整采集（例如启用监听前已发生的分配在录制中释放）明确显示诊断。
- **Errors** 保留代码数值、消息、文件、行号及源事件，支持筛选、分页和 CSV。双击消息可定位接收它的场景帧。未知代码保留原始数值。

Profile / memory 流未采集时会说明应启用的 SDK instrumentation 标志，不会显示伪造的零耗时或零内存。实时分析只解析新 Session 前缀；历史查询和过滤不重置解析器的跨包状态。若录制仅含分析通道而没有场景帧，可直接分析全部捕获数据。

**Analysis → Property curves** 可从已检查对象添加数值属性，也可输入对象 ID、generation、属性名、component 及数组 item。支持最多 16 条曲线、各自显隐/颜色/删除，滚轮缩放、拖动平移、点选定位原始帧，以及选中曲线的精确 CSV。默认每条最多 2000 个原始采样点；缺失值、不同 generation、未采样区间均断线。实时自动刷新最多每秒一次，普通绘制帧不重做采样。帧横轴与界面编号从 1 开始；CSV 保留原始 0-based 源索引与事件/tick。

**Analysis → Frame diff** 选择当前 scene/sample mode 下的 A/B 帧，比较创建、销毁、属性、引用、flags、颜色、相机及原点偏移。对象 ID 复用按旧 generation 销毁、新 generation 创建分别显示；可查看 before/after 或跳转检查对应对象。界面帧号同样从 1 开始。

```powershell
& .\build-physx34\viewer\Debug\pvd_viewer.exe .\build-physx34\fixtures\profile_capture.pxd2 --page profile
& .\build-physx34\viewer\Debug\pvd_viewer.exe .\build-physx34\fixtures\allocations_memory.pxd2 --page memory
```

## 验证

```powershell
ctest --test-dir build-physx34 -C Debug --output-on-failure
& .\build-physx34\viewer\Debug\pvd_viewer.exe --smoke `
  .\build-physx34\fixtures\render_capture.pxd2 --warp --hidden `
  --report .\build-physx34\fixtures\viewer-smoke.json `
  --screenshot .\build-physx34\fixtures\viewer-smoke.png
```

`viewer_model` 验证相机、矩阵、射线拾取和文档状态，`viewer_physx34` 读取真实 SDK 录制。`viewer_gpu` 使用 D3D11 WARP 实际光栅化，执行逐帧/播放、CPU 三角形拾取与属性联动，并从 GPU 读回图像，检查同一相机下不同捕获帧的图像变化，以及选择高亮改变像素。PNG 来自包含 ImGui 界面的真实交换链 backbuffer。`--warp` 可省略以使用本机硬件；硬件设备创建失败时自动回退 WARP。

`viewer_scene_first_normal_gpu` 和 `viewer_scene_first_compact_gpu` 使用 `--layout-smoke` 验证默认 1440×900 与 1060×700 的场景优先布局。它们在原有场景/拾取验证后，通过真实 ImGui 鼠标与键盘事件检查 Scene objects / Object properties 浮层互斥、关闭图标 / Escape 关闭、选中摘要打开属性、浮层滚轮不影响相机、空白场景点击清除选择，以及浮层开关期间 GPU 纹理尺寸不变。

```powershell
ctest --test-dir build-physx34 -C Debug --output-on-failure `
  -R 'viewer_scene_first_(normal|compact)_gpu'
& .\build-physx34\viewer\Debug\pvd_viewer.exe --layout-smoke `
  .\build-physx34\fixtures\render_capture.pxd2 --warp --hidden --size 1060 700 `
  --report .\build-physx34\fixtures\viewer-scene-first-compact.json `
  --screenshot .\build-physx34\fixtures\viewer-scene-first-compact.png
```

布局 smoke 在基础截图文件名上追加 `-objects` 和 `-properties`，分别保存两个浮层的真实 backbuffer 图像。例如上面的命令生成 `viewer-scene-first-compact.png`、`viewer-scene-first-compact-objects.png` 和 `viewer-scene-first-compact-properties.png`；normal 用例以 `viewer-scene-first-normal` 为前缀。报告中的 `scene_first_interactions` 表示布局交互验证已完成。

同一验证还保存 `-file-menu`、`-more-menu`、`-display-menu`、`-view-settings` 和 `-inspector-settings` 截图，并检查设置弹窗的打开路径，以及 Escape 先关闭弹窗、保留后方属性侧栏的行为。

`viewer_live` 验证安全增量、暂停和跟随、事务中间状态及取消；`viewer_live_physx34` 用真实 SDK 验证双场景、中文录制路径、载荷预算和重开后的状态一致；`viewer_live_gpu` 启动真实 SDK 客户端连接运行中的查看器，验证接收期间的 D3D11 画面更新、完整录制和离线重开。

`analysis_panels` 通过真实 ImGui widget 验证历史前缀切换、分页、错误过滤及 1440/1060 宽度布局。`analysis_panels_physx34` 读取真实 memory/error 捕获。`viewer_profile_gpu`、`viewer_memory_gpu`、`viewer_errors_gpu` 渲染实际分析工作区并保存 GPU backbuffer 截图，路径为 `build-physx34/fixtures/viewer-{profile,memory,errors}.png`；JSON 同名。

`curve_panels` / `curve_panels_physx34` 使用真实 ImGui 鼠标输入验证点选定位、缩放、scene/sample mode、生命周期、CSV 按钮及窄布局。`viewer_curves_gpu` / `viewer_diff_gpu` 渲染真实物理捕获的曲线和状态差异，输出 `viewer-curves.png` / `viewer-diff.png`。

`export_job` 验证真实异步写出、覆盖保护、取消、安全尾及两种分析 provider。`viewer_export_gpu` 通过真实窗口后台任务保存整份录制和第 3–15 帧，重开并与源结束前缀逐状态核对；最新输出索引为 `viewer-export-latest.json`。`viewer_live_gpu` 还在接收期间独立保存快照并核对其原始前缀，同时验证完整 TCP 录制继续写完。

这些是程序内模型及渲染验证，未替代系统鼠标、文件对话框及物理多显示器的人工交互验收。性能、内存、曲线、工程剪辑、RepX 和原生扩展对象的当前覆盖见 [PhysX 3.4 验收说明](physx34-acceptance.md)；旧 SDK/旧容器按用户确定的范围后续兼容。

早期 0.4 基线验证结果：Debug 15 项及 Release 共享构建 7 项测试通过；最终相机/渲染调整后，重新通过全部 viewer 测试。安装后的 Release 程序已从其他工作目录启动并使用硬件渲染真实 PhysX 3.4 捕获。12 个 shape、3048 个渲染三角形（含高亮和调试图元），固定相机跨帧变化 1151 像素、选择变化 142 像素。WARP 对照同样通过；中文及带空格路径、无效文件错误出口已验证。当前 0.9 验证不以这些历史数量作为完整性或性能保证。

实际输出位于 `build-physx34/fixtures/pvd-studio.png`，对应 `viewer-hardware.json`；CTest 的 WARP 输出为 `viewer-smoke.png` / `viewer-smoke.json`。这些数量来自当前验证录制，并非所有录制的性能或完整性保证。

## 实现边界和依赖

当前多会话主机、加载取消、书签、用户设置和快捷键行为见
[工作区说明](workspace.md)。**Analysis → Event curves** 对原始 CPU scope 做原生帧窗口
分析，支持独立单位组、精确 CSV 及延迟回填；详见
[事件耗时曲线](profile-curves.md)。**More → Deformables** 将原生对象面板与视口并排
显示，提供布料固定点/约束边及粒子属性、速度、独立拾取/隔离/聚焦。
原生 cloth 的面拓扑未被 SDK PVD 记录，视图明确显示这一限制。

**File → Export RepX...** 异步导出当前显示快照，支持场景、全部、可见 attachment 和
选中对象范围。严格模式拒绝未知内容或原生格式损失；显式 partial 模式的
诊断保留在 XML、导出弹窗和底部 Activity 详情中，长诊断可悬停查看。任务只冻结源会话的
导入，其他标签页、网络接收和 raw 录制继续工作。

新增验证入口为 `--deformable-smoke CAPTURE`、
`--workspace-smoke CAPTURE --workspace-state NEW.pvdws` 和
`--repx-smoke CAPTURE --output NEW.repx [--allow-partial]`。
`viewer_deformables_gpu` 验证真实 SDK 的 16 个布料点、58 条约束、12 个粒子及
选择改变 81 个 GPU 像素；`viewer_repx_gpu` 验证严格拒绝与显式 partial 发布。
工作区、raw 保存、在线快照和曲线 GPU 验证继续保留。

**More → Scene queries** 展示原生及批量 raycast/sweep/overlap 的真实参数、命中、来源和查询几何；空命中与未发送的字段分别说明。**Display → Display and camera settings...** 提供上下轴/镜像手系、背景、真实 MSAA、原生关节限制线、tight geometry bounds 与记录的线/角速度，详见[查询视图](scene-query-viewer.md)和[显示设置](display-settings.md)。

界面按显示器 DPI 重新栅格化字体及缩放逻辑尺寸；`WM_DPICHANGED` 在下一帧前安全更新 CPU atlas 和 D3D11 纹理。可用 `--ui-scale 1.5` 显式覆盖本次进程的自动 DPI，或用 `--dpi-smoke CAPTURE --warp --hidden` 验证 1x→1.5x→2x→1x 的实际窗口/字体/GPU 路径。OS 可以约束过大的窗口；JSON 保留实际 client 尺寸。物理显示器人工验收的限制见[缩放说明](ui-scale.md)。

- `viewer/model.*`：与 ImGui、Win32、GPU 无关的文档、时间轴、选择、相机和拾取。
- `viewer/live_session.*`：与 UI 无关的实时协调、增量导入和跟随状态。
- `viewer/renderer.*`：独立的 D3D11 场景渲染及 WIC PNG 导出，可由其他原生 UI 复用。
- `viewer/panels.*`：ImGui 面板。
- `viewer/main.cpp`：窗口、文件打开、异步加载、布局和应用生命周期。

Dear ImGui 固定为 [v1.91.9b / f5befd2](https://github.com/ocornut/imgui/tree/f5befd2d29e66809cd1110a152e375a7f1981f06)，遵循 [MIT 许可证](../third_party/imgui/LICENSE.txt)。`third_party/imgui/pvd-manifest.json` 记录来源与 SHA-256；`tools/fetch_imgui.py` 用于显式更新同一固定版本。本程序从本机 Windows Fonts 读取系统字体，不再分发字体。
