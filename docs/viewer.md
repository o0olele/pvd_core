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

支持打开菜单、Ctrl+O、拖入文件或命令行路径。录制在独立线程加载，加载失败会显示错误并保留原来的文档。Windows 文件路径通过 UTF-8 / UTF-16 转换处理。

## 操作

- 场景树、视口选中和属性检查器共享同一个对象选择状态；共享 shape 以 scene / actor / shape 及 generation 区分实例。
- 视口右键拖动旋转、中键拖动平移、滚轮缩放、单击选择、双击聚焦选中对象、F 聚焦场景。
- 时间轴支持场景切换、定位、前后逐帧、播放、速度和循环；Space 播放或暂停，方向键逐帧，Home / End 跳转首尾。
- 属性检查支持标量、向量、变换、字符串、对象引用跳转和分页数组；支持搜索对象与属性。
- Grid / Wire / Contacts 控制网格、线框和接触法线；Diagnostics 显示提取阶段遇到的不完整或未支持数据。
- 调试 points / lines / triangles 由 D3D11 绘制，调试文字由 UI 投影到相应世界位置。

默认时间点是 PVD 原生帧事件边界。PhysX 3.4 验证程序另外写入的 after-fetch 标记可显式切换；普通录制没有这些标记时不会推测同等语义。播放速度单位为捕获帧/秒，不作为原始仿真时间的推断。无限平面采用有限预览网格，录制中的几何与位姿保持原始世界坐标。

## 实时采集

点击 **Live capture** 设置监听地址（默认 `127.0.0.1`）、端口（默认 `5425`）和录制路径，再点击 **Start listening**。从 PhysX 3.4 应用连接该地址即可接收。端口 `0` 请求系统选择可用端口，绑定结果显示在状态栏。录制文件以独占方式创建，不覆盖现有文件。

**Follow live** 自动显示当前场景最新的完整帧；手动拖动时间轴、逐帧或播放历史会暂停跟随。接收及录制仍继续，可以随时重新打开跟随。**Stop** 取消网络接收，界面继续导入已接收的安全事件前缀，然后保留当前会话供回放。状态栏显示连接、流量、帧数、待导入事件以及错误；悬停查看接收、录制、安全前缀和丢弃尾部字节。

```powershell
& .\build-physx34\viewer\Debug\pvd_viewer.exe --listen 5425 --record .\captures\new-session.pxd2
```

网络线程负责接收、解码和录制。主线程默认每 100ms 通过 `TcpCapture::poll_from` 获取最多 16,384 个新事件、8 MiB 的载荷预算，追加到现有 `Document`，不重新加载整个录制。单个事件可超过载荷软预算以保证前进，其大小仍受解码上限限制。未结束的属性事务不会替换视口状态。暂停检查历史时保留原来帧的快照及几何缓存。

关闭 `PVD_BUILD_TRANSPORT` 时仍可构建完整离线查看器，界面不会显示实时采集入口。

## 验证

```powershell
ctest --test-dir build-physx34 -C Debug --output-on-failure
& .\build-physx34\viewer\Debug\pvd_viewer.exe --smoke `
  .\build-physx34\fixtures\render_capture.pxd2 --warp --hidden `
  --report .\build-physx34\fixtures\viewer-smoke.json `
  --screenshot .\build-physx34\fixtures\viewer-smoke.png
```

`viewer_model` 验证相机、矩阵、射线拾取和文档状态，`viewer_physx34` 读取真实 SDK 录制。`viewer_gpu` 使用 D3D11 WARP 实际光栅化，执行逐帧/播放、CPU 三角形拾取与属性联动，并从 GPU 读回图像，检查同一相机下不同捕获帧的图像变化，以及选择高亮改变像素。PNG 来自包含 ImGui 界面的真实交换链 backbuffer。`--warp` 可省略以使用本机硬件；硬件设备创建失败时自动回退 WARP。

`viewer_live` 验证安全增量、暂停和跟随、事务中间状态及取消；`viewer_live_physx34` 用真实 SDK 验证双场景、中文录制路径、载荷预算和重开后的状态一致；`viewer_live_gpu` 启动真实 SDK 客户端连接运行中的查看器，验证接收期间的 D3D11 画面更新、完整录制和离线重开。

这些是程序内模型及渲染验证，未替代系统鼠标、文件对话框及高 DPI 多显示器的人工交互验收。性能、内存、曲线、工程剪辑和旧版容器等完整应用功能按 [实施清单](application-roadmap.md) 继续建设。

本机验证结果：Debug 15 项及 Release 共享构建 7 项测试通过；最终相机/渲染调整后，重新通过全部 viewer 测试。安装后的 Release 程序已从其他工作目录启动并使用硬件渲染真实 PhysX 3.4 捕获。12 个 shape、3048 个渲染三角形（含高亮和调试图元），固定相机跨帧变化 1151 像素、选择变化 142 像素。WARP 对照同样通过；中文及带空格路径、无效文件错误出口已验证。

实际输出位于 `build-physx34/fixtures/pvd-studio.png`，对应 `viewer-hardware.json`；CTest 的 WARP 输出为 `viewer-smoke.png` / `viewer-smoke.json`。这些数量来自当前验证录制，并非所有录制的性能或完整性保证。

## 实现边界和依赖

- `viewer/model.*`：与 ImGui、Win32、GPU 无关的文档、时间轴、选择、相机和拾取。
- `viewer/live_session.*`：与 UI 无关的实时协调、增量导入和跟随状态。
- `viewer/renderer.*`：独立的 D3D11 场景渲染及 WIC PNG 导出，可由其他原生 UI 复用。
- `viewer/panels.*`：ImGui 面板。
- `viewer/main.cpp`：窗口、文件打开、异步加载、布局和应用生命周期。

Dear ImGui 固定为 [v1.91.9b / f5befd2](https://github.com/ocornut/imgui/tree/f5befd2d29e66809cd1110a152e375a7f1981f06)，遵循 [MIT 许可证](../third_party/imgui/LICENSE.txt)。`third_party/imgui/pvd-manifest.json` 记录来源与 SHA-256；`tools/fetch_imgui.py` 用于显式更新同一固定版本。本程序从本机 Windows Fonts 读取系统字体，不再分发字体。
