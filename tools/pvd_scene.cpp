#include <iomanip>
#include <iostream>
#include <locale>
#include <pvd/debug_render.hpp>
#include <pvd/mesh_builder.hpp>
#include <sstream>

namespace {
using namespace pvd;
using namespace pvd::render;
std::string quote(const std::string &text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\')
      out << '\\' << c;
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << unsigned(c);
    else
      out << c;
  }
  return out.str() + '"';
}
std::uint64_t number(const std::string &text) {
  if (text.empty() || text.front() == '-')
    throw std::invalid_argument("expected a nonnegative integer");
  std::size_t used = 0;
  const auto n = std::stoull(text, &used,
                             text.size() > 2 && text[0] == '0' &&
                                     (text[1] == 'x' || text[1] == 'X')
                                 ? 16
                                 : 10);
  if (used != text.size())
    throw std::invalid_argument("invalid integer: " + text);
  return n;
}
const char *kind(GeometryKind k) {
  switch (k) {
  case GeometryKind::box:
    return "box";
  case GeometryKind::sphere:
    return "sphere";
  case GeometryKind::capsule:
    return "capsule";
  case GeometryKind::plane:
    return "plane";
  case GeometryKind::convex_mesh:
    return "convex_mesh";
  case GeometryKind::triangle_mesh:
    return "triangle_mesh";
  case GeometryKind::height_field:
    return "height_field";
  }
  return "unknown";
}
void vector(Vec3 v) {
  std::cout << '[' << v.x << ',' << v.y << ',' << v.z << ']';
}
void pose(const Transform &t) {
  std::cout << "{\"position\":";
  vector(t.position);
  std::cout << ",\"rotation_xyzw\":[" << t.rotation.x << ',' << t.rotation.y
            << ',' << t.rotation.z << ',' << t.rotation.w << "]}";
}
void geometry(const Geometry &shape) {
  std::cout << "{\"type\":" << quote(kind(geometry_kind(shape)));
  std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Box>) {
          std::cout << ",\"half_extents\":";
          vector(value.half_extents);
        } else if constexpr (std::is_same_v<T, Sphere>)
          std::cout << ",\"radius\":" << value.radius;
        else if constexpr (std::is_same_v<T, Capsule>)
          std::cout << ",\"radius\":" << value.radius
                    << ",\"half_height\":" << value.half_height
                    << ",\"axis\":\"X\"";
        else if constexpr (std::is_same_v<T, Plane>)
          std::cout << ",\"infinite\":true,\"normal\":[1,0,0]";
        else {
          std::cout << ",\"resource_id\":"
                    << quote(std::to_string(value.mesh ? value.mesh->resource.id
                                                       : 0))
                    << ",\"resource_generation\":"
                    << (value.mesh ? value.mesh->resource.generation : 0)
                    << ",\"vertices\":"
                    << (value.mesh ? value.mesh->vertices.size() : 0)
                    << ",\"triangles\":"
                    << (value.mesh ? value.mesh->indices.size() / 3 : 0);
          if constexpr (std::is_same_v<T, HeightField>)
            std::cout << ",\"rows\":" << value.rows
                      << ",\"columns\":" << value.columns << ",\"scale\":["
                      << value.row_scale << ',' << value.height_scale << ','
                      << value.column_scale << ']';
          else {
            std::cout << ",\"scale\":";
            vector(value.scale.scale);
            std::cout << ",\"scale_rotation_xyzw\":[" << value.scale.rotation.x
                      << ',' << value.scale.rotation.y << ','
                      << value.scale.rotation.z << ',' << value.scale.rotation.w
                      << ']';
          }
        }
      },
      shape);
  std::cout << '}';
}
void write_json(const Frame &frame, const CacheStats &cache,
                const std::optional<DebugResult> &debug) {
  std::cout << "{\n\"event_count\":" << frame.event_count
            << ",\"coordinate_system\":\"native_PhysX\",\"instances\":[";
  bool first = true;
  for (const auto &item : frame.instances) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "\n{\"actor_id\":" << quote(std::to_string(item.actor_id))
              << ",\"actor_generation\":" << item.actor_generation
              << ",\"shape_id\":" << quote(std::to_string(item.shape_id))
              << ",\"shape_generation\":" << item.shape_generation
              << ",\"scene_id\":" << quote(std::to_string(item.scene_id))
              << ",\"world_pose\":";
    pose(item.world_pose);
    std::cout << ",\"geometry\":";
    geometry(item.geometry);
    std::cout << ",\"shape_flags\":" << item.flags.raw
              << ",\"trigger\":" << (item.flags.trigger ? "true" : "false")
              << ",\"kinematic\":" << (item.actor.kinematic ? "true" : "false")
              << ",\"world_bounds\":";
    if (item.world_bounds) {
      std::cout << "{\"minimum\":";
      vector(item.world_bounds->minimum);
      std::cout << ",\"maximum\":";
      vector(item.world_bounds->maximum);
      std::cout << '}';
    } else
      std::cout << "null";
    std::cout << '}';
  }
  std::cout << "\n],\"cache\":{\"entries\":" << cache.entries
            << ",\"hits\":" << cache.hits << ",\"misses\":" << cache.misses
            << "},\"diagnostics\":[";
  first = true;
  for (const auto &d : frame.diagnostics) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "{\"code\":" << int(d.code)
              << ",\"object_id\":" << quote(std::to_string(d.object_id))
              << ",\"property\":" << quote(d.property)
              << ",\"message\":" << quote(d.message) << '}';
  }
  std::cout << ']';
  if (debug) {
    std::cout << ",\"debug\":{\"complete\":"
              << (debug->complete ? "true" : "false")
              << ",\"contacts_available\":"
              << (debug->contacts_available ? "true" : "false")
              << ",\"contacts\":" << debug->contacts.size()
              << ",\"points\":" << debug->points.size()
              << ",\"lines\":" << debug->lines.size()
              << ",\"triangles\":" << debug->triangles.size()
              << ",\"texts\":" << debug->texts.size()
              << ",\"constraints\":" << debug->constraints.size()
              << ",\"diagnostics\":[";
    first = true;
    for (const auto &d : debug->diagnostics) {
      if (!first)
        std::cout << ',';
      first = false;
      std::cout << "{\"code\":" << int(d.code) << ",\"event\":" << d.event_index
                << ",\"message\":" << quote(d.message) << '}';
    }
    std::cout << "]}";
  }
  std::cout << "\n}\n";
}
void write_obj(const Frame &frame, MeshBuildOptions options) {
  std::cout
      << "# Native PhysX coordinates; finite preview for analytic planes\n";
  std::uint64_t offset = 1;
  for (const auto &item : frame.instances) {
    const auto mesh = tessellate(item.geometry, options);
    std::cout << "o actor_" << item.actor_id << '_' << item.actor_generation
              << "_shape_" << item.shape_id << '_' << item.shape_generation
              << '\n';
    for (auto v : mesh.positions) {
      v = transform_point(item.world_pose, v);
      std::cout << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    }
    for (auto n : mesh.normals) {
      n = rotate(item.world_pose.rotation, n);
      std::cout << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
    }
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
      std::cout << 'f';
      for (std::size_t j = 0; j < 3; ++j) {
        const auto id = offset + mesh.indices[i + j];
        std::cout << ' ' << id << "//" << id;
      }
      std::cout << '\n';
    }
    offset += mesh.positions.size();
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || std::string(argv[1]) == "--help") {
    std::cout
        << "usage: pvd_scene capture.pxd2 [--frame N | --event N] [--scene "
           "ID]\n"
           "                 [--debug [--debug-begin N]] [--obj] "
           "[--plane-half-extent X]\n"
           "Default output: JSON. --obj writes world-space OBJ to stdout.\n";
    return argc < 2 ? 2 : 0;
  }
  try {
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(9);
    std::optional<std::size_t> frame_index, event_count, debug_begin;
    ExtractOptions extract_options;
    MeshBuildOptions mesh_options;
    bool obj = false, debug_enabled = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg(argv[i]);
      if (arg == "--frame" && i + 1 < argc)
        frame_index = static_cast<std::size_t>(number(argv[++i]));
      else if (arg == "--event" && i + 1 < argc)
        event_count = static_cast<std::size_t>(number(argv[++i]));
      else if (arg == "--scene" && i + 1 < argc)
        extract_options.scene_id = number(argv[++i]);
      else if (arg == "--debug-begin" && i + 1 < argc)
        debug_begin = static_cast<std::size_t>(number(argv[++i]));
      else if (arg == "--plane-half-extent" && i + 1 < argc) {
        std::size_t used = 0;
        const std::string text(argv[++i]);
        mesh_options.plane_half_extent = std::stof(text, &used);
        if (used != text.size())
          throw std::invalid_argument("invalid plane extent");
      } else if (arg == "--obj")
        obj = true;
      else if (arg == "--debug")
        debug_enabled = true;
      else
        throw std::invalid_argument("unknown or incomplete option: " + arg);
    }
    if (frame_index && event_count)
      throw std::invalid_argument("choose --frame or --event");
    auto session = Session::load_file(argv[1]);
    if (!frame_index && !event_count && !session.frames().empty())
      frame_index = session.frames().size() - 1;
    auto snapshot = frame_index
                        ? session.snapshot_at_frame(*frame_index)
                        : session.snapshot_at_event(
                              event_count.value_or(session.event_count()));
    Extractor extractor;
    const auto extracted =
        extractor.extract(session.metadata(), snapshot, extract_options);
    std::optional<DebugResult> debug;
    if (debug_enabled) {
      DebugRange range;
      range.end_event_count = snapshot.event_count;
      range.scene_id = extract_options.scene_id;
      if (debug_begin)
        range.begin_event_count = *debug_begin;
      else if (frame_index)
        range.begin_event_count =
            session.frames().at(*frame_index).begin_event_count;
      else
        throw std::invalid_argument(
            "--debug with --event requires --debug-begin to avoid accumulating "
            "prior frames");
      if (!range.scene_id && frame_index)
        range.scene_id = session.frames().at(*frame_index).scene_id;
      debug = extract_debug(snapshot, session.metadata(), session.channels(),
                            range);
    }
    if (obj) {
      write_obj(extracted, mesh_options);
      for (const auto &diagnostic : extracted.diagnostics)
        std::cerr << diagnostic.message << '\n';
    } else
      write_json(extracted, extractor.cache_stats(), debug);
  } catch (const std::exception &error) {
    std::cerr << "pvd_scene: " << error.what() << '\n';
    return 1;
  }
}
