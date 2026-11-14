# PhysX 3.4 原点平移与坐标视图

`OriginShift` 不是附加的 UI 相机偏移。SDK 先移动场景中的原生数据，再发一个包含 scene ID 和 shift 向量的事件。它不会在同一时刻重新发送所有受影响属性。保留旧 `GlobalPose` 并只显示累计偏移，会使 static、sleeping 与之后刷新的 awake 对象处于不同坐标系。

## 本地 SDK 证据

权威源码为 `D:/Code/PhysX-3.4-master`：

- `PhysX_3.4/Source/PhysX/src/NpScene.cpp:3088`、`:3109`：`shiftOrigin` 在禁止 simulation buffering 的条件下，先移动 rigid static、rigid dynamic 和所有 articulation link，再调用 Scb scene、scene-query manager 和 volume cache 的平移；debug render buffer 随后移动。
- `PhysX_3.4/Source/PhysX/src/buffering/ScbScene.cpp:994`：底层 scene 完成平移后，发送 PVD origin-shift event。
- `PhysX_3.4/Source/PhysX/src/PvdMetaDataPvdBinding.cpp:1475`、`:1514`：周期 pose 更新只覆盖 dynamic/link，稳定 sleeping 对象不重发，static 不在此更新中。
- 同文件 `:2140`：cloth 周期更新发送 ParticleBuffer 和 sleeping 状态，没有周期 GlobalPose 更新。`SimulationController/src/cloth/ScClothCore.cpp:1135` 实际平移 cloth 的 motion translation。
- `PhysX_3.4/Source/PhysXExtensions/src/ExtJoint.h:354`：world 端的 LocalPose 随 shift 改变，并标记 constraint dirty；下一次 `NpScene.cpp:1992` 的 joint 更新发送全属性。
- `PhysX_3.4/Source/PhysX/src/NpPvdSceneQueryCollector.cpp:126` 将查询时的 origin/pose/hit 按值保存，collector 没有 shift 处理。`NpScene.cpp:2267` 在 fetch 时发布并清空它；一次 publication 可以混合 shift 前后的查询。

独立 producer 为 `examples/physx34_origin_shift_capture.cpp`。getter oracle 直接读取 SDK 对象，CSV 中 `query_input` 行明确为 API 输入，`@WorldParticle` 为 SDK pose.transform(local particle)。没有通过被测 Session/Extractor 生成期望值。

真实审计包含 2 scenes、8 phase markers 与 66 次 shift（包括大 float 坐标上的连续 64 次小 shift）。getter/input CSV 字段数由 producer 输出；第一次 shift 为 `(100,2,-3)`：

| 对象 | shift 后立即的 SDK x | PVD 保留的 raw x | 下一次 fetch 后 raw x |
|---|---:|---:|---:|
| static | 0 | 100 | 100 |
| awake dynamic | 2 | 102 | 2 |
| sleeper | 4 | 104 | 104 |
| awake link | 6 | 106 | 6 |
| sleeping link | 8 | 108 | 108 |
| awake cloth | 10 | 110 | 110 |
| sleeping cloth | 13 | 113 | 113 |
| particle | 20 | 120 | 20 |
| world joint 的空端 anchor | 2 | 102 | 2 |

另一 scene 的 static x 保持 100。显式 setter 刷新 static/cloth 后，第二次 shift 会再次作用于新的值。

## raw 与 native 坐标读取

`PropertyValue::raw_bytes()`、`as_transform()`、`as_vec3()` 和 Session 原始 events 保留输入协议，行为不变。显式原生坐标 API 为：

```cpp
#include <pvd/coordinates.hpp>
auto pose = pvd::coordinates::native_transform(*object.property("GlobalPose"));
auto point = pvd::coordinates::native_vec3(positions, item);
const auto& bytes = pvd::coordinates::native_bytes(value);
auto normalized_value = pvd::coordinates::native_value(value);
```

Session 只为已确认的 PhysX 3.4 原生类/字段维护不可变 shadow：rigid/link/cloth 的 GlobalPose、particle-system/fluid 的 Positions 与 ProjectionPlane、六类 extension joint 的 world 空端 LocalPose。ProjectionPlane 在实流中为 PxVec4，xyz 为 normal，w 为 distance；每次按 SDK `distance += normal.dot(shift)` 更新。它不会平移 Shape.LocalPose、CMassLocalPose、速度、cloth 局部 particle buffer 或任意自定义类。

scene 归属按 OriginShift 当时的关系判断。Rigid/cloth/particle 使用 Scene；link 可沿 Parent 链到 articulation；extension joint 使用非空 actor 端定位 scene。后续迁移 scene 不会追溯改变旧决定，ID generation 复用也不继承旧 shadow。

每个 owner 与相关 property 有 epoch，跨 shift 的同字节重发仍是有效刷新；同一 epoch 的重复赋值仍可压缩。shadow 对每一次 shift 分别执行 float 减法，而不是最后减去累计总量。例如大坐标上的连续小 shift 可能每次都被 float 舍入掉，不能用最后的数学总量替代。shadow 数量受保留 property payload 数量约束，不保留无限长的坐标 journal；历史 snapshot/checkpoint 共享不可变数据。

累计 `Snapshot.origin_shifts` 用于保留显式协议事实，不应再统一减到 native 读取结果。native EndSection 的原有采样时序保持不变，它仍早于 SDK 的 dynamic pose 周期消息。

## 保存与中段裁剪

完整 save 重写原始事件，保留 wire 数据。中段 crop 需要重新建立状态，其 bootstrap 顺序为：

1. 恢复对象 lifetime skeleton 与全部 ownership/reference 属性。
2. 恢复 scene 的累计 OriginShift。
3. 写入当前 native 坐标的规范化属性，以及其余保留属性。
4. 接上原始 `[begin,end)` 事件切片，不更改该切片的事件或 payload。

因此源 raw GlobalPose 为 100、native 为 0 时，crop bootstrap 的 raw GlobalPose 会明确变为 0。物理等价应比较显式 native 视图；不能声称 crop 的 bootstrap raw 属性仍与源录制相同。后续原始 setter/message/shift 应继续产生相同 native 状态。

六个 scene query publication 数组也记录写入 epoch，但其 opaque payload 不被改写。若 crop 起点已经 shift、collector 尚未下次发布，这些旧数组放在 bootstrap OriginShift 前；它们与其他非世界属性的顺序不被误当成新的当前坐标 publication。下一次原始 publication 因此仍会保留相同的 ambiguous-coordinate 诊断。SDK 测试包含这个中间边界，而非仅测试从原点 shift 事件之前开始裁剪。

## Scene query 的不可恢复边界

真实 SDK 在同一 publication 中发送了 shift 前 `(100,5,-10)` 与 shift 后 `(0,3,-7)` 的两次 raycast。每项没有 origin epoch、单独时间戳或可信调用顺序。不能按 publication 的最后坐标系统一修正这些 raw 参数。

`SceneQuery::coordinates_ambiguous` 和 `SceneQueryCode::ambiguous_coordinates` 标记 collection/publication 区间跨 shift 的情况。原始 query、hit、geometry 和 ranges 仍可检查；派生 paths、normals、volume previews 与 Fit 被抑制。该判定是保守的：即使应用只在 shift 后调用 query，协议也没有足够信息证明这一点。

## Recorded camera

`PxPvdSceneClient::updateCamera` 在 API 名称上属于 scene client，但 `SetCamera` 只有 name、position、up 和 target，没有 scene ID。`PxPvdCommStreamEvents.h:864` 与 `PxPvdSceneClient.h:103` 将其定义为 PVD render window 的相机更新；原生 Scene metadata 没有 Cameras 集合。

真实 fixture 证实，shift 不会自动发送新的 SetCamera；只有显式 updateCamera 才改变对应名字的值。当前保留命名 camera 的 raw 值，不推测其 scene 归属或自动平移。相机的 up 仍按实际向量解释。

## 验证边界

`tests/coordinates_tests.cpp` 覆盖 LE/BE、32/64 位指针、同字节跨 epoch 刷新、同 epoch 压缩、迁移、ID 复用、checkpoint/倒放、crop 后继续原事件、完整 save 和连续小 shift 的逐次 float 舍入。

`tests/physx34_origin_shift.cpp` 以 getter CSV 对照 native snapshot、render、cloth 与 particles，并检查 mixed-query 诊断、camera、full save/crop 与原始区间 wire fidelity；启用 `PVD_ORIGIN_TEST_ANALYSIS` 时，还逐 phase 对照 native/wire 曲线及 diff。当前独立 MSVC Debug /W4 测试通过 9,444 项精确 float/getter 检查，不依赖增大的浮点容差。严格选区 RepX 的 first-shift/end/crop 三个变体也已通过真实 SDK load → official XML serialize → reload，对照 120/127/127 项独立 getter 字段；其中大 float 小 shift 的精确性由前述逐字面 float getter 检查约束。

Inspector 默认显示 native 值并标记 `[native]`，tooltip 明确展示 last-wire raw 值。曲线和 diff 默认使用 native 视图，API 可显式选择 wire，CSV 记录 coordinate_space。diff 还分别标记 wire_data_changed 和仅由坐标平移引起的 coordinates_changed；没有伪造原始属性写入。
