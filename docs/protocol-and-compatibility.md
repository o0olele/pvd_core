# PVD 协议和录制兼容笔记

当前最重要的兼容边界是：SDK 原始事件流、查看器保存的对象模型容器，以及旧 SDK 协议转换路径需要分别建模。下面只有标注为汇编观察的内容绑定到当前 PVD3.exe；官方 SDK 布局说明绑定到其固定源码提交。

## 网络接入角色

PVD 是接收端，PhysX 应用主动建立 TCP 连接。手册 p4 与本地 PvdListener 符号一致；主窗口初始化端口在汇编中确认为 5425。

SDK 的公开入口包括 `PxDefaultPvdSocketTransportCreate`、`PxDefaultPvdFileTransportCreate`、`PxCreatePvd` 和 `PxPvd::connect`。二进制含 PxPvdSDK 符号，说明客户端相关公共组件也被链接；这不表示查看器整体源码已经公开。

## 原始流识别

本地 `PvdCommStreamEventReader::isPvdStream` 位于 `0x1400B8BE0`，于 `0x1400B8BE8` 比较 `0x31E5E882`，未匹配时交换字节后再比。证据为 `analysis/symbols/is-pvd-stream-disassembly.txt`。这与 PhysX 3.4 官方源码的 stream ID 十进制 `837150850` 一致。

这项观察确认识别常量及字节顺序候选，不足以证明所有字段均可交换、全部协议版本均被支持，也不证明任意给定文件的完整性。

## 官方 3.4 协议布局

参考固定为官方仓库 `NVIDIAGameWorks/PhysX-3.4` 提交 `5e42a5f112351a223c19c17bb331e6c55037b8eb`。来源及哈希见 `analysis/official-sdk/manifest.json`；可用 `analysis/fetch_official_sdk.py` 再次获取指定文件，默认固定为本轮提交，也可以用 `--revision` 显式选择其他版本。

[固定提交的官方协议头](https://github.com/NVIDIAGameWorks/PhysX-3.4/blob/5e42a5f112351a223c19c17bb331e6c55037b8eb/PxShared/src/pvd/src/PxPvdCommStreamEvents.h#L294) 与本地参考文件 `PxPvdCommStreamEvents.h:294` 的初始化结构按字段序列化：

| 偏移 | 大小 | 字段 | 该源码含义 |
| --- | --- | --- | --- |
| 0 | 4 | stream ID | `0x31E5E882` |
| 4 | 4 | stream version | 1 |
| 8 | 8 | timestamp numerator | 时间换算分子 |
| 16 | 8 | timestamp denominator | 时间换算分母 |
| 24 | 4 | stream flags | bit 0 为发送端 Is64BitPtr |

28 字节是源码中逐字段序列化的长度，不是 C++ 对象的 sizeof。x86/x64 原生数值写出路径见 `PxPvdCommStreamTypes.h:113`；解析器应依据流识别与版本决定字节序，不能按接收程序自身指针宽度解码发送端数据。

`EventGroup` 的序列化头为 24 字节：`u32 dataSize, u32 numEvents, u64 streamId, u64 timestamp`；后跟 `dataSize` 字节的事件载荷。来源 `PxPvdCommStreamEvents.h:329`。该大小来自字段相加，不含 C++ vptr/对齐。事件类型通过一个 u8 序列化（同文件 `:235`），不能按 C++ enum 的默认 4 字节读取。

本地 `stream-reader-create-disassembly.txt` 中的初始化读取顺序，与上述 28 字节布局吻合；解析路径也有 24 字节 group 读取和 stream ID 分派证据。尚未确证全部版本拒绝规则。group 不保证恰好等于一帧，frame / section / scene / stream 的对应关系仍需要录制样本核对。

官方事件可分为几个领域：

| 领域 | 示例 |
| --- | --- |
| 类型和字符串元数据 | StringHandleEvent、CreateClass、DeriveClass、CreateProperty、CreatePropertyMessage |
| 对象生命周期 | CreateInstance、DestroyInstance |
| 属性更新 | SetPropertyValue、分块 Begin/Append/End、消息组 |
| 引用图 | PushBackObjectRef、RemoveObjectRef |
| 时间区段 | BeginSection、EndSection |
| 显示状态 | SetPickable、SetColor、SetIsTopLevel、SetCamera、OriginShift |
| 分析通道与结束 | AddProfileZone、AddProfileZoneEvent、ErrorMessage、StreamEndEvent |

它提供的是类型化对象/属性流，而非固定格式的“每帧刚体矩阵数组”。源实现中的字符串句柄、类继承、消息批处理和数组更新必须保留；只渲染 transform 的读取器无法替代属性检查和历史查询。

官方文件传输实现 `PxPvdDefaultFileTransport.cpp:66` 将收到的字节交给文件 buffer 写出，因此 SDK 直接记录的输入是研究原始流的重要基准。这不意味着查看器的 Save 命令也输出完全相同的布局。

## 查看器容器候选

本地内部 `Client::load` 在 `0x1400B1B60`，于 `0x1400B1BD1` 比较另一个常量 `0x36101BB0`。`Client::save` 在 `0x1400B23A0` 的对应写入路径提供了双向交叉证据：

| 偏移或位置 | 内容 | 确定程度 |
| --- | --- | --- |
| 0 | u32 `0x36101BB0` | load 比较与 save 写入均确认 |
| 4 | u32 `1` | save 写入确认；解释为格式版本是候选语义 |
| 8 | u32 第一个元数据块长度 | 两端读写和内存块序列化路径支持 |
| 12 | u32 第二个元数据块长度 | 同上 |
| 16 起 | 对象模型元数据块与通信元数据块 | 相关序列化符号、顺序与读取路径支持 |
| 元数据之后 | 对齐至下一 4096 字节边界，再保存/载入事件流 | align/pad 与 load 位置取整交叉支持 |

证据为 `analysis/symbols/client-load-disassembly.txt`、`client-save-disassembly.txt` 和 `symbol-findings.txt`，函数身份由 MAP 绑定。完整元数据编码、事件页布局、版本规则、压缩使用范围、索引和错误处理尚未全部确定。此表是局部格式恢复，尚不能据此发布可靠的兼容编码器。

手册、当前文件对话框和两条 magic 路径共同说明：扩展名应当只是用户提示，格式识别需要头部探测和结构校验。当前保存对话框也使用 `.pxd2`，所以不能仅按扩展名假定 raw 与容器一一对应，更不能通过改后缀实现转换。

## 版本覆盖

本地 MAP 包含 `PvdCommStreamParsingConverters.obj` 的大量符号，且手册分别描述 3.0–3.3 和 3.4+ 接口，支持存在旧协议适配的判断；转换器的准确支持矩阵仍需逐分支和样本验证。官方 3.4 源码中的 version 1 不是当前查看器完整支持列表。

OmniPVD 是另一条应独立设计的适配路径。官方文档描述 `.ovd` 录制与 USD 转换，并指出其当前记录系统没有实时流式显示。因此不能把 OVD reader 当作旧 PVD TCP/.pxd2 的即插即用替代。[NVIDIA OmniPVD 文档](https://docs.omniverse.nvidia.com/kit/docs/omni_physics/latest/extensions/ux/source/omni.physx.pvd/docs/dev_guide/physx_visual_debugger.html)

## 下一阶段需要的验证

1. 用已知 SDK 生成最小 raw 录制：一个 scene、静态平面、动态 box；记录首帧、删除对象、数组/引用变化及多帧状态。
2. 让原查看器读取同一录制并另存，保留两份文件，比较头部、元数据、页边界和事件索引。
3. 分别启用 debug、profile、memory 和 contacts/queries/constraints，验证未采集与采集后为空的区别。
4. 加入多个 scene/stream、32 位发送端、旧 SDK 样本和异常截断文件，逐项建立版本覆盖表。
5. 对同一帧比较对象数、类型、属性原始值、对象引用、变换、profile/memory 总量，验证顺序回放、随机 seek、倒放结果一致。

首轮静态分析时尚无这些样本。后续 C++ 内核已使用本地 PhysX 3.4 SDK 生成真实 raw 捕获，并完成帧状态、对象生命周期和自定义事件对照，详情见 [内核与验证说明](kernel.md)。旧查看器另存容器和旧 SDK 分支仍需独立样本，不由 raw 流验证结果推定兼容。
