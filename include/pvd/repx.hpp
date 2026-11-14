#pragma once
#include "pvd/session.hpp"
#include <functional>
#include <iosfwd>
#include <limits>

#if defined(_WIN32) && !defined(PVD_REPX_STATIC)
#if defined(PVD_REPX_BUILD)
#define PVD_REPX_API __declspec(dllexport)
#else
#define PVD_REPX_API __declspec(dllimport)
#endif
#else
#define PVD_REPX_API
#endif

namespace pvd::repx {
struct ObjectKey {
  std::uint64_t id = 0, generation = 0;
  bool operator<(const ObjectKey &o) const {
    return std::tie(id, generation) < std::tie(o.id, o.generation);
  }
};
struct AttachmentKey {
  ObjectKey scene, actor, shape;
  bool operator<(const AttachmentKey &o) const {
    return std::tie(scene, actor, shape) < std::tie(o.scene, o.actor, o.shape);
  }
};
enum class Selection { all_actors, objects, attachments };
enum class UnsupportedPolicy { reject, allow_partial };
enum class Severity { warning, error };
struct Diagnostic {
  Severity severity = Severity::error;
  ObjectKey object;
  std::string property, message;
};
struct Options {
  Selection selection = Selection::all_actors;
  // Selected/visible callers pass their immutable actor/shape keys here.
  // Selecting an attached shape includes its owning actors and dependencies.
  std::vector<ObjectKey> objects;
  // Exact scene/actor/shape attachments, including all three generations.
  // Other owners of a shared shape and other shapes on an actor stay excluded.
  std::vector<AttachmentKey> attachments;
  std::optional<ObjectKey> scene;
  UnsupportedPolicy unsupported = UnsupportedPolicy::reject;
  std::size_t max_objects = 1024u * 1024u;
  std::size_t max_vertices = 4u * 1024u * 1024u;
  std::size_t max_triangles = 8u * 1024u * 1024u;
  std::size_t max_heightfield_samples = 4u * 1024u * 1024u;
  std::size_t max_xml_bytes = 256u * 1024u * 1024u;
  std::function<bool()> cancelled;
};
struct ObjectMapping {
  ObjectKey source;
  // Inline exclusive shapes have no collection-level serial ID in SDK RepX.
  std::uint64_t serial_id = 0;
  std::string type;
  std::uint64_t owner_serial_id = 0;
  std::size_t shape_index = 0;
};
struct Result {
  std::string xml;
  std::string version = "3.4.2";
  std::size_t actor_count = 0, shape_count = 0, material_count = 0,
              resource_count = 0, joint_count = 0;
  std::vector<ObjectMapping> objects;
  std::vector<Diagnostic> diagnostics;
  bool complete = true;
};
class ExportCancelled : public std::exception {
public:
  const char *what() const noexcept override { return "RepX export cancelled"; }
};
class ExportError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
// PhysX 3.4 RepX XML containing selected rigid actors and their material/shape/
// mesh dependencies and eligible six-type extension joints. No SDK, ImGui,
// device, or scene simulation dependency. Joint endpoints never widen actors.
// Unknown/unsupported requested content is rejected by default. Partial output
// requires explicit opt-in and carries diagnostics plus XML provenance.
PVD_REPX_API Result export_snapshot(const Metadata &, const Snapshot &,
                                    const Options & = {});
PVD_REPX_API Result export_session(
    const Session &,
    std::size_t event_count = std::numeric_limits<std::size_t>::max(),
    const Options & = {});
PVD_REPX_API void write(std::ostream &, const Result &);
// UTF-8 destination; atomic publication, no overwrite by default. Cancellation
// or failure leaves an existing destination untouched.
PVD_REPX_API void save_file(const std::string &utf8_path, const Result &,
                            bool overwrite = false,
                            std::function<bool()> cancelled = {});
} // namespace pvd::repx
