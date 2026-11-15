# ImGui PVD 应用实施清单

目标是继续实现完整 PVD 应用，以 Dear ImGui 提供轻量界面，保持 C++ core 可以独立构建、安装和被其他 UI 使用。每章完成后验证并提交本地 Git；后续提交不自动推送远程。

本清单以 [旧版功能基线](reverse-engineering.md) 和 [重写方案](rewrite-plan.md) 为依据。未完成项继续保留，不以已有离线 3D 查看器代替完整应用验收。PhysX 3.4 是首要真实 SDK 验证版本；其他版本及旧容器需要各自的实物样本和验证。

2026-11-15 用户明确本轮先验收 **PhysX 3.4 的完整工作流**；旧 PVD 容器与旧 SDK 协议列为后续兼容工作。本轮继续覆盖采集、检查、分析、曲线、视图工具、多会话、保存/剪辑、RepX 及独立库验证，不因延后旧格式而省略这些功能。

| 章节 | 功能及验收 | 当前证据 / 状态 |
| --- | --- | --- |
| 0 数据和离线查看基线 | 增量协议、元数据、历史状态、七类几何、共享 shape、树/属性/选择/逐帧回放、GPU 实际截图 | 已有 `96daf4d`；15 项 Debug、7 项 Release 测试和真实 SDK 捕获 |
| 1 实时采集与录制 | UI 设置监听地址/端口和录制路径；连接/接收/停止/错误状态；跟随最新或暂停检查历史；增量传输避免反复复制全历史；SDK 在线显示、取消、断连和录制重开一致 | 已实现；Debug 18 项/Release 8 项测试覆盖，真实双场景 SDK + D3D11 在线烟测；极小批次和中文路径验证 |
| 2 性能分析 | 真实 profile 通道解码、zone/thread/context、嵌套时长、时间线和聚合；选事件跳转帧；CSV；明确未知/截断数据 | 原生 3.4 CPU 核与 ImGui 已实现并通过 SDK oracle/UI/GPU 验证。SDK 的 `CUDAProfileBuffer` 明确是 `<3.4` 旧数据占位；旧 CUDA payload 仅诊断，按用户范围后续兼容 |
| 3 内存及错误分析 | allocate/free、地址复用、类型/文件/行号、历史时间点活动分配、峰值/分组、错误列表/定位、CSV；SDK 真实数据对照 | 核与 ImGui 已实现：SDK 5 阶段 / 2 次地址复用 / 5 条错误逐字段核对，真实 UI 前缀/分页/布局及 D3D 截图通过 |
| 4 曲线与帧差异 | 对象数值属性及事件耗时曲线、多条曲线、范围缩放、点选跳帧、精确 CSV；生命周期及缺失值语义；帧间对象/属性差异 | 核与 UI 已实现；事件耗时核 `9afce43` 通过 SDK 6 窗口 / 17 区间 / 60 组 oracle。UI 真鼠标、宽整数 CSV、420/1060 布局及 GPU 截图均通过，延迟 flush 可修订历史且空窗口明确为 gap |
| 5 场景检查与视图工具 | 完整对象类别树/搜索、大数组虚拟化；对象显示隐藏/隔离/跟踪；状态着色与图例；调试通道开关；录制相机/自由相机、上下轴/手系、抗锯齿与背景设置 | class tree、分页搜索、数值格式、attachment 显隐/隔离/跟踪、着色图例、录制相机和倒放已验证。上下轴/显示手系、实际 4x MSAA/背景、原生关节帧/限制线、三类普通及批量 scene query 参数/命中/几何预览均有 SDK、ImGui 和 WARP 证据。tight geometry bounds 与记录的线/角速度图层按可见 attachment 生成，缺失数据明确诊断；真实 GPU 显隐回归通过 |
| 6 多会话与工程体验 | 多录制/实时会话切换、书签、最近文件、布局及用户设置持久化、快捷键帮助、加载进度/取消、错误恢复 | 稳定 slot 加载/live/export、独立 UI、验证书签及损坏设置保护；真实双会话恢复和切换中 TCP/快照通过。工作区 v5 保存剪辑归档、完整显示/配色/检查/导航及全局默认，读取 v1–v4；无效源不消耗配额，不同相机的书签均保留，待验证剪辑不会被普通编辑误删 |
| 7 保存、裁剪与旧容器 | 原始录制保存、游标范围裁剪且保留起始对象状态；明确识别格式；损坏数据有定位 | Raw writer、起点 snapshot、memory/profile bootstrap 与 ImGui 异步保存已实现；原子/取消/中文路径及 SDK 重开状态核对通过；在线快照不停止 TCP 录制也已验证。旧容器/旧协议按用户选择后续补齐 |
| 8 RepX 和扩展对象 | 选中/可见/全部场景导出，刚体/网格/材质/关节在 SDK 中重新加载对照；cloth/APEX/particles 按真实样本恢复相应显示和属性行为 | RepX 刚体核 `cee6d97` 与后台导出 UI 已通过；六类扩展 joint 已由 `e8552a4` 通过真实 SDK 逐字段回载和官方 XML 二次回载。Articulation/Aggregate 真实两场景 41 个采样点正反向与 checkpoint on/off 对照每遍 53,300 字段；RepX 六变体实际加载和官方 XML 二次加载通过，完整集合 28 对象/831 字段。严格失败不发布，显式 partial 诊断保存在 XML 和 UI。原生 cloth/particles 核、面板和独立选择/拾取/隔离/fit/速度视图已通过 SDK 正反向与实际 GPU 高亮验证；独立 APEX SDK 样本未提供 |
| 9 完整应用验收 | 按旧版功能矩阵验证；静态/共享库独立消费；真实 SDK 文件/实时一致；输入、缩放、故障恢复和有明确规模的性能测量 | 0.9：Debug 156/156、Release shared 52/52；SDK/UI 关闭静态模块 24/24、core-only 5/5、core+analysis 14/14、最小离线查看器 31/31 及已安装独立消费通过。1x/1.5x/2x 真 ImGui 输入、Win32 DPI/字体/D3D、导航及 live 显示溢出恢复通过；系统鼠标/原生对话框/物理多屏因工具初始化故障仍待人工验收。完整覆盖与边界见 [验收记录](physx34-acceptance.md) |

## 0.8 工作流复核

在既有回归之上逐项重查旧版调试工作流，补齐了下列可操作功能：

- [OriginShift 原生坐标](origin-shift.md)：保留原始字节，渲染、检查、曲线、diff、裁剪及 RepX 使用逐次 float 平移后的实际值；不可判定的跨平移查询明确诊断。
- [命名多片段剪辑](clip-ranges.md)：分割、裁左/右、命名、删除和逐片段 Save As；源身份、场景代次、实时增长及工作区 v4 的待验证归档不会被普通编辑丢弃。
- [逐帧内存活动](memory-window.md)、[Profiler 区间测量](profile.md)及原生 frame begin/end 耗时曲线；保持原生时钟、精确整数/CSV 和缺失值语义。
- [相机导航](camera-navigation.md)及[持续录制相机](recorded-camera.md)：第一人称、WASD/Shift、速度/投影/look-at，以及 seek/播放时跟随和手动释放。
- [显示范围与坐标轴](visualization-layers.md)：睡眠/触发分类与叠加层按选中状态动态过滤，actor/shape/COM 坐标轴、真实透明深度混合和一致拾取。
- [属性检查](inspection-preferences.md)：SDK enum/flags、未知位、整数十六进制与完整 native/wire 值复制；工作区 v4 保存偏好并读取 v1/v2/v3。

0.9 已进一步补齐[状态与 Profile 配色](palette.md)、[geometry scale、planes、face culling、orbit target 和 joint 尺寸](display-settings.md)，并由工作区 v5 保存。显示变换与 source/RepX 隔离；拾取遵守投影裁剪；显示溢出的导航回滚和实时继续接收可恢复。当前完整汇总、构建命令与环境边界以 [3.4 验收记录](physx34-acceptance.md) 为准。

## 持续约束

- `pvd::core`、`pvd::transport`、`pvd::render`、后续分析/导出库均不包含 ImGui、Win32 窗口或 GPU 依赖；PhysX SDK 仅作可选验证与 RepX 适配。
- UI 只调用公开数据接口；采集和解码错误必须可见。保持 64 位 ID、时间戳和生命周期 generation，不把原始地址当成可解引用指针。
- 每章提交包含实际功能、适当测试、文档和验证记录。只提交本章完成文件，避免夹带并行章节未完成实现。
- 对不支持、通道未采集、当前范围无事件分别显示明确状态。不得用空表格或固定示例替代真实功能。
