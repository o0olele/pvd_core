#include "pvd/render.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
void need(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
struct Expected {
  std::uint64_t id;
  bool dynamic;
  float value[7];
};
void near(float a, float b) {
  need(std::isfinite(a) &&
           std::abs(a - b) <= 1e-5f * std::max(1.f, std::abs(b)),
       "SDK stress pose mismatch");
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(argc == 3, "usage: physx34_stress capture.pxd2 expected.csv");
    std::ifstream file(argv[2]);
    need(bool(file), "oracle open");
    std::string line;
    std::getline(file, line);
    std::map<std::size_t, std::vector<Expected>> expected;
    while (std::getline(file, line)) {
      std::replace(line.begin(), line.end(), ',', ' ');
      std::istringstream row(line);
      std::size_t frame;
      unsigned dynamic;
      Expected value{};
      need(bool(row >> frame >> value.id >> dynamic), "oracle row");
      value.dynamic = dynamic != 0;
      for (float &f : value.value)
        need(bool(row >> f), "oracle pose");
      expected[frame].push_back(value);
    }
    auto session = pvd::Session::load_file(argv[1]);
    need(session.frames().size() == expected.size(),
         "native frame count differs");
    std::size_t checked = 0;
    auto verify = [&](std::size_t frame) {
      auto snapshot = session.snapshot_at_frame(frame);
      for (const auto &value : expected.at(frame)) {
        auto *actor = snapshot.instance(value.id);
        need(actor && actor->generation == 0, "stress actor lifetime");
        need(actor->class_name.name ==
                 (value.dynamic ? "PxRigidDynamic" : "PxRigidStatic"),
             "stress actor class");
        auto *property = actor->property("GlobalPose");
        auto pose = property ? property->as_transform() : std::nullopt;
        need(bool(pose), "stress actor pose");
        const float actual[] = {pose->position.x, pose->position.y,
                                pose->position.z, pose->rotation.x,
                                pose->rotation.y, pose->rotation.z,
                                pose->rotation.w};
        for (unsigned i = 0; i < 7; ++i)
          near(actual[i], value.value[i]);
        ++checked;
      }
      if (frame == 0 || frame + 1 == expected.size()) {
        pvd::render::Extractor extractor;
        auto scene = extractor.extract(session.metadata(), snapshot);
        need(scene.diagnostics.empty() &&
                 scene.instances.size() == expected.at(frame).size(),
             "stress render lost actors/shapes");
      }
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
      verify(frame);
    for (std::size_t frame = expected.size(); frame-- > 0;)
      verify(frame);
    std::cout << "SDK stress oracle: " << checked
              << " actor/frame states checked forward/reverse\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
