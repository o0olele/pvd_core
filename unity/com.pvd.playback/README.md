# PVD Playback for Unity

在 Windows x64 的 Unity 2022.3 及更新版本中接收 PhysX 3.4 PVD 实时流，也可打开原始 PVD 录制。插件把碰撞形状和姿态显示为 Unity 网格，支持实时跟随、播放、暂停和逐帧检查。Unity 接收端无需 PhysX SDK 或独立 PVD 查看器进程。

## 安装

在 Unity 的 **Window → Package Manager** 中选择 **+ → Add package from tarball…**，然后选中 `out/unity/com.pvd.playback-0.2.3.tgz`。压缩包内包含 Windows x64 Editor 和 Standalone Player 所需的 `pvd_unity_v3.dll`。

从旧版升级时，请在 Package Manager 移除旧版，再安装新的 `.tgz` 并重启编辑器。直接安装源码目录只会导入 C# 脚本和资源，原生 DLL 由 CMake 在打包时加入。

## 监听实时 PVD

通过 **GameObject → PVD Playback** 创建回放对象。在 Inspector 的 **Live PVD** 区域填入 **127.0.0.1** 和端口 **5425**，然后点击 **Start Listening**。本机的 PhysX 程序主动连接这个地址，用启用了 PVD 的 PhysX 3.4 SDK 发送数据。使用其他机器作为发送端时，填写 Unity 所在机器可达的绑定地址，并在发送端使用该地址和端口。

```cpp
auto* transport = PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10000);
pvd->connect(*transport, PxPvdInstrumentationFlag::eDEBUG);
```

**Follow Latest** 跟随最新完整帧。首帧到达前，会预览已收到的安全场景状态。帧滑块从首个完整帧开始可用。关闭跟随或拖动帧滑块可以检查已接收的帧，后台继续接收。点击 **Stop Listening** 或发送端断开后，已接收的数据仍保留在当前组件中。一次监听接收一个连接，下一次连接需重新开始监听。实时数据保存在内存中。

状态面板分别显示 TCP 连接状态、已收字节和完整帧数。连接后显示 `0 bytes` 时，当前连接还没有送来应用数据。有字节但完整帧为 0 时，预览会在收到足够的场景数据后出现。

标准 Unity 内置物理没有公开的 PVD 发送入口。插件可以接收外部程序或 Unity 原生插件自行运行的兼容 PhysX 场景。

## 在编辑器中回放

通过 **GameObject → PVD Playback** 创建回放对象。在 Inspector 中用 **Browse & Open…** 打开原始 `.pxd2` 录制。使用播放按钮和帧滑块控制回放。点击 **Frame in Scene View** 可将编辑器视图对准捕获中的几何。

播放速度使用录制帧/秒。面板从第 1 帧显示，C# API 的帧索引从 0 开始。捕获的碰撞几何原本使用右手坐标，默认转换按 Y 轴向上、镜像 Z 轴处理。模型的尺寸沿用录制的单位。

## 从 C# 控制

以下示例可在 Unity 主线程调用。Player 构建中使用可读取的本地录制路径。

```csharp
using Pvd.Playback;

// player 是场景中已有的 PvdPlayer 组件。
player.Open(capturePath);
// 等待 player.IsLoaded，再调用以下方法。
player.Play();
player.Pause();
player.Seek(0);
```

也可调用 `player.StartListening("127.0.0.1", 5425)` 接收实时流，调用 `player.StopListening()` 停止接收并保留当前会话。

组件在后台读取录制并准备帧数据，在主线程创建和复用网格及绘制对象。大型录制首次加载、第一次出现的新网格、场景中大量可见对象仍会增加开销。旧版 NVIDIA 查看器保存的容器和 OmniPVD 的 `.ovd` 使用其他格式。碰撞形状使用包内预览材质，适用于 Built-in 和 URP。使用其他渲染管线时，可给 **Material Override** 分配该管线的材质。

## 从源码构建

在已配置 MSVC x64 工具链的环境中，将 `$pvdBuild` 设为本工作区的 CMake 构建目录。沿用该目录的生成器、工具链和配置。插件目标需要启用渲染库和 TCP 传输库，并使用静态方式链接 PVD 内核。

```powershell
cmake -S . -B $pvdBuild -DPVD_BUILD_UNITY_PLUGIN=ON
cmake --build $pvdBuild --config Release --target pvd_unity_package
```

构建后，从 `out/unity` 中选择对应版本的 `.tgz` 安装。使用 MSVC 动态运行库构建时，目标机器还需要对应的 x64 Visual C++ 运行库。
