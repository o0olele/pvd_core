# 持续跟随录制相机

原版手册第 6 页把 Camera 菜单分为固定命令和 scene cameras，第 9 页说明应用通过 PVD SDK（包括 APEX）提供的相机可以替代自由相机；恢复的 UI 还包含 Default / Clip camera。证据保存在 `analysis/manual/manual-text.txt`、`manual-ui-findings.txt` 与 `ui-strings.json`。这些材料证明相机入口与自由/录制模式，未用于推测任何未发送的投影参数。

PhysX 3.4 的 `PxPvdSceneClient::updateCamera` 最终发送 `SetCamera{name, position, up, target}`，没有 camera object ID/generation、scene ID、FOV、near/far 或插值参数。对应源码是本地 SDK 的 `PxShared/src/pvd/src/PxPvdCommStreamEvents.h:864` 及 `PhysX_3.4/Include/pvd/PxPvdSceneClient.h:103`。同名消息覆盖前值；协议没有删除 camera 的事件。已有值在后续 snapshot 中保留，seek 到首次消息之前才会缺失，不能据此伪造 camera 生命周期。

`viewer/recorded_camera.hpp/.cpp` 为 UI/Win32/GPU/SDK 无依赖的跟随模型。选择 Recorded camera 会绑定精确名称；每次 seek/advance 后，现有 `updateTrackedCamera` host hook 会优先更新该命名相机的 eye/target/up。它直接使用对应 snapshot，支持正放和倒放，不插值、不预测，也不根据 OriginShift 或当前 scene 猜补 camera 坐标。任意有效 up 向量及 roll 使用 OrbitCamera 的原始 look-at 基底。FOV、clip range 和 display handedness 保留用户设置。

模式约定：

- Clip camera 与 Track selected 互斥。前者读取录制值，后者保留用户相对所选对象的 orbit/pan/zoom 偏移。
- Free camera / Stop tracking 释放持续跟随。Apply once 应用当前 snapshot 一次，之后 seek 不再改动视图。
- 手动 orbit、pan、zoom、WASD、look-at、up、Fit 或 bookmark 操作前，host 调用 `notifyManualCameraInput` 释放 Clip camera；此 hook 不改变对象跟随的既有偏移语义。未通知的外部 pose 改动也会在下次更新时被检测并释放。仅修改投影参数不会退出 Clip camera。
- 名称在当前 prefix 缺失或相机参数无效时，保持最后有效视图、显示 missing/invalid 诊断并保留绑定；seek 到有有效数据的 prefix 会自动恢复。
- Document/Session 来源更换时停止旧绑定并保留 source-changed 诊断，不把相同名称自动套用到另一录制。

主机在启动 Clip camera 后将 slot 的 camera_ready 标为 true，避免第一次 viewport auto-fit 覆盖录制选择。此模式属于查看器临时控制状态，不修改 source events、Snapshot，也不扩展 bookmark/workspace 存储语义。

公共入口为 `followRecordedCamera`、`updateRecordedCameraFollow`、`releaseRecordedCamera` 与 `notifyManualCameraInput`。`SceneToolState::recorded_follow` 包含绑定来源、名称、当前状态、采样 prefix、原始 camera sample 与诊断；`tracking_object()` 单独表示对象跟随，`tracking()` 表示任一持续模式。

验证包括 `tests/recorded_camera_tests.cpp` 的 LE/BE 模型、正反 seek、播放、缺失、invalid 与同名恢复、source replacement、手动释放、投影保留、原始事件字节/已发布 Frame 不变与一次应用。可选参数为真实 origin-shift SDK fixture，核对显式 Camera 更新与不推测原点偏移。`tests/scene_tools_tests.cpp` 还用真实 ImGui 1060/420 宽鼠标点击选择、Free、Apply once、模式互斥与来源切换诊断；popup 等待首次实际布局稳定后点击实际控件边界。
