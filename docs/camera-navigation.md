# 相机导航与投影设置

View 弹窗提供 Orbit（围绕目标旋转）和 First person（保留眼睛位置、原地转头）控制器。右键拖动转头/环绕，中键平移，滚轮靠近/远离，遵循显示上轴和左右手约定。

悬停视口时，W/S 沿视线移动，A/D 沿屏幕左右，Q/E 沿显示上轴，Shift 是两倍速度。速度单位为世界单位/秒，对角移动归一化；单帧最多采用 0.1 秒，避免界面暂停后的跳跃。编辑框、弹窗及 Ctrl/Alt/Super 快捷键不会驱动相机。

Look at point 输入目标并 Apply，保留眼睛位置。Center on visible/selected 使用实际 bounds 中心。Near/Far clip 与垂直 FOV 可以编辑；Fit 和滚轮会为新距离重新计算 clipping。非法输入显示错误并保留原视图。这些控制不写入录制属性或 SDK。

导航模式、速度和灵敏度按会话及全局默认保存。工作区 v3 在 v2 的 62 字节偏好记录后追加 17 字节（u8 模式、两个 double）；读取 v1/v2 时采用 Orbit、10 units/s、0.008 rad/logical-pixel。测试保留旧 writer 实际生成的 491 字节 v2 文件，核对手系、MSAA、颜色和检查格式不会在迁移中丢失。

`camera_controller.hpp/.cpp` 是 UI-free viewer model 的数值操作；`camera_panel.cpp` 是 ImGui 适配。测试覆盖六个正负上轴、两种手系和控制器、原地转头、屏幕方向、速度/Shift/对角、非法输入强状态保护，以及真实 ImGui 1x/1.5x/2x 的模式选择、按键和编辑输入隔离。
