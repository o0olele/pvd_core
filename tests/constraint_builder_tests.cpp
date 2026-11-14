#include "pvd/constraint_builder.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::render;
constexpr float pi = 3.14159265358979323846f;
void check(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
void near(float a, float b, const char *text) {
  check(std::isfinite(a) && std::abs(a - b) < 3e-5f, text);
}
void near(Vec3 a, Vec3 b, const char *text) {
  near(a.x, b.x, text);
  near(a.y, b.y, text);
  near(a.z, b.z, text);
}
DebugSource source{11, 22, 33, 44, 55};
DebugConstraint event(DebugConstraintPayload p) {
  return {source, std::move(p)};
}
bool same(DebugSource a, DebugSource b) {
  return a.stream_id == b.stream_id &&
         a.channel_instance_id == b.channel_instance_id &&
         a.instance_id == b.instance_id && a.event_index == b.event_index &&
         a.payload_offset == b.payload_offset;
}
void all_types() {
  const Transform identity{};
  std::vector<DebugConstraint> input{
      event(DebugJointFrames{identity, identity}),
      event(DebugLinearLimit{identity, identity, -2, true}),
      event(DebugAngularLimit{identity, -pi / 2, pi / 2, false}),
      event(DebugLimitCone{identity, std::tan(pi / 8), .2f, true}),
      event(DebugDoubleCone{identity, pi / 4, false})};
  auto all = build_constraint_lines(input);
  check(all.complete && all.diagnostics.empty() &&
            all.constraints_processed == 5 && all.constraints_emitted == 5,
        "all five SDK constraint families");
  check(all.lines.size() == 6 + 21 + 22 + 66 + 128,
        "fixed SDK native line counts");
  for (const auto &line : all.lines)
    check(line.derived && same(line.source, source),
          "retain source and derived provenance");
  near(all.lines[0].position0, {}, "parent origin");
  near(all.lines[0].position1, {1.5f, 0, 0}, "parent frame has 1.5 scale");
  near(all.lines[3].position1, {1, 0, 0}, "child frame unit scale");
  check(all.lines[0].color0 == 0x88880000u &&
            all.lines[1].color1 == 0x88008800u &&
            all.lines[2].color1 == 0x88000088u &&
            all.lines[3].color0 == 0xffff0000u &&
            all.lines[4].color0 == 0xff00ff00u &&
            all.lines[5].color0 == 0xff0000ffu,
        "SDK parent dark and child bright ARGB");
  near(all.lines[6].position1, {-2, 0, 0}, "linear signed negative distance");
  for (std::size_t i = 7; i < 27; ++i) {
    near(all.lines[i].position0.x, -2, "linear ring center on signed x axis");
    near(all.lines[i].position1.x, -2,
         "linear ring remains perpendicular to x");
    check(all.lines[i].color0 == 0xffff0000u, "active SDK red");
  }
  near(all.lines[27].position1, {0, 0, -1}, "lower angular boundary radians");
  near(all.lines[28].position1, {0, 0, 1}, "upper angular boundary radians");
  check(all.lines[27].color0 == 0xff808080u, "inactive SDK grey");
  const auto cone = 6 + 21 + 22;
  near(
      all.lines[cone].position1, {0, 1, 0},
      "quarter-angle tangent is a ninety-degree swing, not a radian parameter");
  near(all.lines[cone].position0, {},
       "SDK cone begins with origin-to-first-boundary segment");
  near(all.lines[cone + 1].position1, all.lines[cone].position1,
       "SDK first cone spoke is intentionally repeated");
  const auto dbl = cone + 66;
  near(all.lines[dbl].position0, {1, 1, 0},
       "double cone positive circle at tan(angle)");
  near(all.lines[dbl + 32].position0, {-1, 1, 0},
       "double cone negative circle");
  near(all.lines[dbl + 64].position0, {}, "double cone spokes start at origin");
  near(all.lines[dbl + 64].position1, {-1, 1, 0},
       "double cone first negative spoke");
}
void world_and_scales() {
  const float s = std::sqrt(.5f);
  Transform t{{0, 0, s, s}, {3, 4, 5}}, other{{}, {200, -300, 400}};
  auto r = build_constraint_lines({event(DebugLinearLimit{t, other, 2, false})},
                                  ConstraintBuildOptions{2, 2});
  check(r.complete && r.lines.size() == 21, "transformed linear limit");
  near(r.lines[0].position0, {3, 4, 5}, "world origin not actor-local");
  near(r.lines[0].position1, {3, 6, 5},
       "linear shaft uses world t0 x basis, never t1");
  near(r.lines[1].position0, {2.4f, 6, 5},
       "linear ring radius scales, signed distance does not");
  r = build_constraint_lines({event(DebugDoubleCone{t, pi / 4, true})});
  near(r.lines[0].position0, {2, 5, 5},
       "world quaternion and translation applied once");
  r = build_constraint_lines({event(DebugJointFrames{t, t})},
                             ConstraintBuildOptions{2, 0});
  near(r.lines[0].position1, {3, 7, 5}, "parent scale two times SDK 1.5");
  near(r.lines[3].position1, {3, 6, 5}, "child scale two");
  r = build_constraint_lines(
      {event(DebugJointFrames{t, t}), event(DebugLinearLimit{t, t, 2, true})},
      ConstraintBuildOptions{0, 0});
  check(r.complete && r.lines.empty() && r.constraints_processed == 2,
        "zero display scales disable output");
}
void invalid_and_atomic() {
  const auto nan = std::numeric_limits<float>::quiet_NaN(),
             inf = std::numeric_limits<float>::infinity(),
             big = std::numeric_limits<float>::max();
  Transform zero{{0, 0, 0, 0}, {}}, scaled{{0, 0, 0, 2}, {}},
      badpos{{}, {nan, 0, 0}}, badquat{{0, inf, 0, 1}, {}};
  std::vector<DebugConstraint> bad{
      event(DebugJointFrames{zero, {}}),
      event(DebugJointFrames{{}, scaled}),
      event(DebugLinearLimit{{}, badpos, 1, false}),
      event(DebugAngularLimit{badquat, 0, 1, false}),
      event(DebugLinearLimit{{}, {}, inf, false}),
      event(DebugAngularLimit{{}, 1, -1, false}),
      event(DebugAngularLimit{{}, -big, big, false}),
      event(DebugLimitCone{{}, -.1f, .2f, false}),
      event(DebugLimitCone{{}, .1f, 1, false}),
      event(DebugDoubleCone{{}, -.1f, false}),
      event(DebugDoubleCone{{}, pi, false}),
      event(DebugDoubleCone{{}, nan, false})};
  auto r = build_constraint_lines(bad);
  check(!r.complete && r.lines.empty() && r.diagnostics.size() == bad.size(),
        "strict finite, unit-pose, ordered angles and cone domain validation");
  for (std::size_t i = 0; i < bad.size(); ++i)
    check(r.diagnostics[i].constraint_index == i &&
              same(r.diagnostics[i].source, source) &&
              r.diagnostics[i].code == ConstraintCode::invalid_payload,
          "precise invalid source and index");
  bad.push_back(event(DebugJointFrames{{}, {}}));
  ConstraintBuildOptions options;
  options.max_diagnostics = 1;
  r = build_constraint_lines(bad, options);
  check(!r.complete && r.diagnostics.size() == 1 && r.lines.size() == 6 &&
            r.constraints_emitted == 1,
        "bounded diagnostics keep valid later constraints");
  options = {};
  options.frame_scale = big;
  r = build_constraint_lines({event(DebugJointFrames{{}, {}}),
                              event(DebugDoubleCone{{}, pi / 4, false})},
                             options);
  check(!r.complete && r.lines.size() == 128 && r.constraints_emitted == 1,
        "overflowing geometry does not partially commit a constraint");
  r = build_constraint_lines({event(DebugAngularLimit{{}, 0, 0, false}),
                              event(DebugLimitCone{{}, 0, 0, false}),
                              event(DebugDoubleCone{{}, 0, false})});
  check(r.complete && r.lines.size() == 22 + 66 + 128,
        "finite zero/locked boundaries retain SDK degeneracies");
}
void budgets() {
  const std::vector<DebugConstraint> input{
      event(DebugJointFrames{{}, {}}),
      event(DebugLinearLimit{{}, {}, 3, false}),
      event(DebugAngularLimit{{}, 0, pi, false})};
  ConstraintBuildOptions options;
  options.max_lines = 27;
  auto r = build_constraint_lines(input, options);
  check(!r.complete && r.lines.size() == 27 && r.constraints_emitted == 2 &&
            r.diagnostics.size() == 1 &&
            r.diagnostics[0].constraint_index == 2 &&
            r.diagnostics[0].code == ConstraintCode::limit_exceeded,
        "line budget at complete constraint boundary");
  options = {};
  options.max_constraints = 1;
  r = build_constraint_lines(input, options);
  check(!r.complete && r.lines.size() == 6 && r.constraints_processed == 1 &&
            r.diagnostics[0].constraint_index == 1,
        "work budget limits the examined input prefix");
  options = {};
  options.max_lines = 5;
  r = build_constraint_lines(input, options);
  check(!r.complete && r.lines.empty(),
        "no partial frame axes on budget failure");
  options = {};
  options.max_constraints = 0;
  r = build_constraint_lines(input, options);
  check(!r.complete && r.constraints_processed == 0 && r.lines.empty(),
        "zero work budget");
  options = {};
  options.max_lines = 0;
  r = build_constraint_lines({}, options);
  check(r.complete && r.lines.empty(),
        "empty input is complete with zero line budget");
  for (int i = 0; i < 4; ++i) {
    options = {};
    if (i == 0)
      options.frame_scale = -1;
    if (i == 1)
      options.limit_scale = std::numeric_limits<float>::quiet_NaN();
    if (i == 2)
      options.frame_scale = std::numeric_limits<float>::infinity();
    if (i == 3)
      options.max_diagnostics = 0;
    bool threw = false;
    try {
      build_constraint_lines({}, options);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    check(threw, "invalid options fail before processing");
  }
}
} // namespace
int main() {
  try {
    all_types();
    world_and_scales();
    invalid_and_atomic();
    budgets();
    std::cout << "constraint builder tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
