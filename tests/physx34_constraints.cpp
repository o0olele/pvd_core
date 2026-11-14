// Optional independent oracle: calls the installed PhysX 3.4 Common DLL's
// actual CmVisualization functions; never recompiles their implementation.
#include "CmRenderOutput.h"
#include "CmVisualization.h"
#include "PxPhysicsAPI.h"
#include "pvd/constraint_builder.hpp"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace pvd;
namespace r = pvd::render;
void need(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
physx::PxTransform pose(const Transform &t) {
  return {
      physx::PxVec3(t.position.x, t.position.y, t.position.z),
      physx::PxQuat(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w)};
}
void close(float a, float b, const std::string &what) {
  need(std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= 4e-5f + std::abs(b) * 1e-5f,
       what + ": " + std::to_string(a) + " != " + std::to_string(b));
}
void close(Vec3 a, physx::PxVec3 b, const std::string &what) {
  close(a.x, b.x, what + " x");
  close(a.y, b.y, what + " y");
  close(a.z, b.z, what + " z");
}
std::size_t compare(const r::DebugConstraint &c, float frame = 1,
                    float limit = 1) {
  physx::Cm::RenderBuffer buffer;
  physx::Cm::RenderOutput output(buffer);
  std::visit(
      [&](const auto &p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, r::DebugJointFrames>)
          physx::Cm::visualizeJointFrames(output, frame, pose(p.parent),
                                          pose(p.child));
        if constexpr (std::is_same_v<T, r::DebugLinearLimit>)
          physx::Cm::visualizeLinearLimit(output, limit, pose(p.t0), pose(p.t1),
                                          p.value, p.active);
        if constexpr (std::is_same_v<T, r::DebugAngularLimit>)
          physx::Cm::visualizeAngularLimit(output, limit, pose(p.t0), p.lower,
                                           p.upper, p.active);
        if constexpr (std::is_same_v<T, r::DebugLimitCone>)
          physx::Cm::visualizeLimitCone(output, limit, pose(p.transform),
                                        p.y_swing, p.z_swing, p.active);
        if constexpr (std::is_same_v<T, r::DebugDoubleCone>)
          physx::Cm::visualizeDoubleCone(output, limit, pose(p.transform),
                                         p.angle, p.active);
      },
      c.payload);
  r::ConstraintBuildOptions options;
  options.frame_scale = frame;
  options.limit_scale = limit;
  const auto actual = r::build_constraint_lines({c}, options);
  need(actual.complete && actual.diagnostics.empty(),
       "builder rejected valid SDK constraint");
  need(actual.lines.size() == buffer.getNbLines(),
       "SDK native line count differs for type " +
           std::to_string(c.payload.index()));
  for (std::size_t i = 0; i < actual.lines.size(); ++i) {
    const auto &a = actual.lines[i];
    const auto &b = buffer.getLines()[i];
    const auto context = "SDK native line " + std::to_string(i) + " type " +
                         std::to_string(c.payload.index());
    close(a.position0, b.pos0, context + " start");
    close(a.position1, b.pos1, context + " end");
    need(a.color0 == b.color0 && a.color1 == b.color1, context + " exact ARGB");
    need(a.derived && a.source.stream_id == c.source.stream_id &&
             a.source.channel_instance_id == c.source.channel_instance_id &&
             a.source.instance_id == c.source.instance_id &&
             a.source.event_index == c.source.event_index &&
             a.source.payload_offset == c.source.payload_offset,
         context + " source provenance");
  }
  return actual.lines.size();
}
void synthetic() {
  const float s = std::sqrt(.5f);
  const Transform identity{}, rotated{{0, 0, s, s}, {3, -7, 11}};
  for (const auto &t : {identity, rotated})
    for (bool active : {false, true})
      for (float scale : {.25f, 2.f}) {
        compare({{}, r::DebugJointFrames{t, identity}}, scale, scale);
        compare({{}, r::DebugLinearLimit{t, rotated, -2.3f, active}}, scale,
                scale);
        compare({{}, r::DebugLinearLimit{t, rotated, 0, active}}, scale, scale);
        compare({{}, r::DebugAngularLimit{t, -.6f, 1.4f, active}}, scale,
                scale);
        compare({{}, r::DebugLimitCone{t, .1f, .4f, active}}, scale, scale);
        compare({{}, r::DebugDoubleCone{t, .7f, active}}, scale, scale);
        compare({{}, r::DebugDoubleCone{t, 2.f, active}}, scale, scale);
      }
}
void recorded(const std::string &path) {
  std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
  need(bool(input), "open joints capture");
  StreamDecoder decoder;
  std::array<std::uint8_t, 65536> bytes{};
  while (input) {
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    decoder.feed(bytes.data(), static_cast<std::size_t>(input.gcount()));
  }
  need(!input.bad(), "read joints capture");
  decoder.finish();
  need(bool(decoder.header()), "joints stream header");
  Session session(*decoder.header(), decoder.take_events());
  const auto debug =
      r::extract_debug(session.latest(), session.metadata(), session.channels(),
                       {0, session.event_count(), {}, {}}, {});
  std::array<std::size_t, 5> counts{};
  std::size_t lines = 0;
  for (const auto &c : debug.constraints) {
    ++counts[c.payload.index()];
    lines += compare(c);
  }
  need(!debug.constraints.empty(),
       "SDK joints capture contains no typed debug constraints");
  std::size_t frame_constraints = 0;
  for (const auto &marker : session.frames()) {
    const auto per_frame = r::extract_debug(session, marker);
    for (const auto &c : per_frame.constraints) {
      ++frame_constraints;
      compare(c);
    }
  }
  std::cout<<"Native-frame constraints "<<frame_constraints<<" of "<<debug.constraints.size()<<'\n';
  for(const auto& marker:session.frames())std::cout<<"frame "<<marker.begin_event_count<<".."<<marker.event_count<<" scene "<<marker.scene_id<<'\n';
  for(const auto& c:debug.constraints)std::cout<<"constraint event "<<c.source.event_index<<" type "<<c.payload.index()<<" owner "<<c.source.instance_id<<'\n';
  need(frame_constraints == debug.constraints.size(),
       "scene-scoped native frame range must contain exactly the recorded "
       "joint constraints, without omissions or history duplication");
  std::cout << "Real SDK constraints: " << debug.constraints.size()
            << ", native lines " << lines << ", types";
  for (auto n : counts)
    std::cout << ' ' << n;
  std::cout << "; outer frames " << session.frames().size() << '\n';
  for (auto n : counts)
    need(n != 0, "real SDK fixture must exercise all five typed constraints");
}
struct Errors : physx::PxErrorCallback {
  unsigned failures = 0;
  void reportError(physx::PxErrorCode::Enum, const char *text, const char *,
                   int) override {
    ++failures;
    std::cerr << text << '\n';
  }
};
} // namespace
int main(int argc, char **argv) {
  physx::PxDefaultAllocator allocator;
  Errors errors;
  auto *foundation =
      PxCreateFoundation(PX_FOUNDATION_VERSION, allocator, errors);
  if (!foundation)
    return 1;
  int result = 1;
  try {
    synthetic();
    if (argc > 1)
      recorded(argv[1]);
    need(errors.failures == 0, "SDK oracle emitted errors");
    std::cout << "SDK Common DLL constraint oracle passed\n";
    result = 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
  }
  foundation->release();
  return result;
}
