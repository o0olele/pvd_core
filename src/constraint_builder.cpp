//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Copyright (c) 2008-2018 NVIDIA Corporation. All rights reserved.
// Copyright (c) 2004-2008 AGEIA Technologies, Inc. All rights reserved.
// Copyright (c) 2001-2004 NovodeX AG. All rights reserved.

// Adapted from PhysX 3.4 CmVisualization.cpp/CmRenderOutput.cpp. Native
// line primitives only; no SDK, GPU or UI dependency.
#include "pvd/constraint_builder.hpp"
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace pvd::render {
namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr std::uint32_t red = 0xffff0000u, grey = 0xff808080u;
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
bool finite(Vec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
void need(bool ok, const char *text) {
  if (!ok)
    throw std::invalid_argument(text);
}
void pose(const Transform &t) {
  const auto &q = t.rotation;
  const double n = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z +
                   double(q.w) * q.w;
  need(finite(t.position) && std::isfinite(n) &&
           std::abs(std::sqrt(n) - 1) < 1e-4,
       "world pose must be finite with a unit quaternion (SDK tolerance 1e-4); "
       "not normalized or repaired");
}
// RenderOutput uses PxMat44(Transform), not a second actor/local transform.
Vec3 world(const Transform &t, Vec3 v) {
  const auto &q = t.rotation;
  const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
  const float xx = x2 * q.x, yy = y2 * q.y, zz = z2 * q.z, xy = x2 * q.y,
              xz = x2 * q.z, xw = x2 * q.w, yz = y2 * q.z, yw = y2 * q.w,
              zw = z2 * q.w;
  return {
      ((1 - yy - zz) * v.x + (xy - zw) * v.y) + (xz + yw) * v.z + t.position.x,
      ((xy + zw) * v.x + (1 - xx - zz) * v.y) + (yz - xw) * v.z + t.position.y,
      ((xz - yw) * v.x + (yz + xw) * v.y) + (1 - xx - yy) * v.z + t.position.z};
}
Quat product(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
          a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Vec3 basis_x(Quat q) {
  const float x2 = q.x * 2, w2 = q.w * 2;
  return {(q.w * w2) - 1 + q.x * x2, q.z * w2 + q.y * x2, -q.y * w2 + q.z * x2};
}
Vec3 rotated_x(Quat q) {
  const float w2 = q.w * q.w - .5f, dot2 = q.x * 2;
  return {2 * w2 + q.x * dot2, q.z * 2 * q.w + q.y * dot2,
          -q.y * 2 * q.w + q.z * dot2};
}
struct Lines {
  const DebugSource &source;
  std::vector<DebugLine> value;
  void add_world(Vec3 a, Vec3 b, std::uint32_t color) {
    need(finite(a) && finite(b),
         "derived world coordinates overflow or are nonfinite");
    value.push_back({source, a, b, color, color, true});
  }
  void add_local(const Transform &t, Vec3 a, Vec3 b, std::uint32_t color) {
    add_world(world(t, a), world(t, b), color);
  }
};
std::size_t count(const DebugConstraintPayload &p) {
  constexpr std::size_t counts[] = {6, 21, 22, 66, 128};
  return counts[p.index()];
}
void validate(const DebugJointFrames &p) {
  pose(p.parent);
  pose(p.child);
}
void validate(const DebugLinearLimit &p) {
  pose(p.t0);
  pose(p.t1);
  need(std::isfinite(p.value),
       "linear distance must be finite; signed negative distances are valid");
}
void validate(const DebugAngularLimit &p) {
  pose(p.t0);
  need(std::isfinite(p.lower) && std::isfinite(p.upper) && p.lower <= p.upper &&
           std::isfinite(p.upper - p.lower),
       "angular radians must be finite and ordered lower <= upper, with a "
       "finite span");
}
void validate(const DebugLimitCone &p) {
  pose(p.transform);
  need(std::isfinite(p.y_swing) && std::isfinite(p.z_swing) && p.y_swing >= 0 &&
           p.z_swing >= 0 && p.y_swing < 1 && p.z_swing < 1,
       "cone wire parameters must be finite tan(quarter-angle) in [0,1), not "
       "radians");
}
void validate(const DebugDoubleCone &p) {
  pose(p.transform);
  need(std::isfinite(p.angle) && p.angle >= 0 && p.angle < pi &&
           std::isfinite(std::tan(p.angle)),
       "double cone radians must be finite in [0,pi), with finite tangent");
}
void build(Lines &out, const DebugJointFrames &p, float scale) {
  const std::uint32_t parent[] = {0x88880000u, 0x88008800u, 0x88000088u},
                      child[] = {red, 0xff00ff00u, 0xff0000ffu};
  for (int frame = 0; frame < 2; ++frame) {
    const auto &t = frame == 0 ? p.parent : p.child;
    const float extent = scale * (frame == 0 ? 1.5f : 1.f);
    for (int axis = 0; axis < 3; ++axis) {
      Vec3 v{};
      if (axis == 0)
        v.x = extent;
      else if (axis == 1)
        v.y = extent;
      else
        v.z = extent;
      out.add_local(t, {}, v, frame == 0 ? parent[axis] : child[axis]);
    }
  }
}
void build(Lines &out, const DebugLinearLimit &p, float scale) {
  const auto color = p.active ? red : grey;
  const auto tip = add(p.t0.position, mul(basis_x(p.t0.rotation), p.value));
  out.add_world(p.t0.position, tip, color);
  const float half = pi / 4;
  const Transform r{
      product(p.t0.rotation, {0, std::sin(half), 0, std::cos(half)}), tip};
  const float step = 2 * pi / 20, radius = scale * .3f;
  float angle = 0;
  Vec3 previous{0, radius, 0};
  for (unsigned i = 0; i < 20; ++i, angle += step) {
    const Vec3 current{radius * std::sin(angle), radius * std::cos(angle), 0};
    if (i)
      out.add_local(r, previous, current, color);
    previous = current;
  }
  out.add_local(r, previous, {0, radius, 0}, color);
}
void build(Lines &out, const DebugAngularLimit &p, float scale) {
  const auto color = p.active ? red : grey;
  auto at = [&](float angle) {
    return Vec3{0, std::cos(angle) * scale, std::sin(angle) * scale};
  };
  out.add_local(p.t0, {}, at(p.lower), color);
  out.add_local(p.t0, {}, at(p.upper), color);
  const float step = (p.upper - p.lower) / 20;
  float angle = p.lower;
  Vec3 previous{};
  for (unsigned i = 0; i <= 20; ++i, angle += step) {
    auto v = at(angle);
    if (i)
      out.add_local(p.t0, previous, v, color);
    previous = v;
  }
}
void build(Lines &out, const DebugLimitCone &p, float scale) {
  const auto color = p.active ? red : grey;
  Vec3 previous{};
  for (unsigned i = 0; i <= 32; ++i) {
    const float angle = 2 * pi / 32 * static_cast<float>(i);
    const float y = -p.z_swing * std::sin(angle),
                z = p.y_swing * std::cos(angle);
    const float rv2 = y * y + z * z, inv = 1 / (1 + rv2);
    const Quat q{0, 2 * y * inv, 2 * z * inv, (1 - rv2) * inv};
    const auto v = mul(rotated_x(q), scale);
    out.add_local(p.transform, previous, v, color);
    out.add_local(p.transform, {}, v, color);
    previous = v;
  }
}
void build(Lines &out, const DebugDoubleCone &p, float scale) {
  const auto color = p.active ? red : grey;
  const float height = std::tan(p.angle), step = pi * 2 / 32;
  auto at = [&](float x, unsigned i) {
    return mul(Vec3{x, std::cos(step * static_cast<float>(i)),
                    std::sin(step * static_cast<float>(i))},
               scale);
  };
  for (float x : {height, -height}) {
    Vec3 previous = at(x, 0);
    for (unsigned i = 1; i <= 32; ++i) {
      auto v = at(x, i);
      out.add_local(p.transform, previous, v, color);
      previous = v;
    }
  }
  for (unsigned i = 0; i < 32; ++i) {
    out.add_local(p.transform, {}, at(-height, i), color);
    out.add_local(p.transform, {}, at(height, i), color);
  }
}
} // namespace
ConstraintLines
build_constraint_lines(const std::vector<DebugConstraint> &input,
                       ConstraintBuildOptions options) {
  if (!std::isfinite(options.frame_scale) || options.frame_scale < 0 ||
      !std::isfinite(options.limit_scale) || options.limit_scale < 0 ||
      !options.max_diagnostics)
    throw std::invalid_argument("constraint scales must be finite and "
                                "nonnegative; max_diagnostics must be nonzero");
  ConstraintLines result;
  auto problem = [&](ConstraintCode code, std::size_t i, const char *text) {
    result.complete = false;
    if (result.diagnostics.size() < options.max_diagnostics)
      result.diagnostics.push_back({code, input[i].source, i, text});
  };
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (i >= options.max_constraints) {
      problem(
          ConstraintCode::limit_exceeded, i,
          "constraint work budget exhausted at a complete constraint boundary");
      break;
    }
    ++result.constraints_processed;
    try {
      const auto &p = input[i].payload;
      std::visit([](const auto &v) { validate(v); }, p);
      const float scale =
          p.index() == 0 ? options.frame_scale : options.limit_scale;
      if (scale == 0)
        continue;
      const auto required = count(p);
      if (required > options.max_lines - result.lines.size()) {
        problem(ConstraintCode::limit_exceeded, i,
                "line budget exhausted before a complete constraint");
        break;
      }
      Lines staged{input[i].source, {}};
      staged.value.reserve(required);
      std::visit([&](const auto &v) { build(staged, v, scale); }, p);
      result.lines.insert(result.lines.end(), staged.value.begin(),
                          staged.value.end());
      ++result.constraints_emitted;
    } catch (const std::invalid_argument &e) {
      problem(ConstraintCode::invalid_payload, i, e.what());
    }
  }
  return result;
}
} // namespace pvd::render
