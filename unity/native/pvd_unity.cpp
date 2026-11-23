#include "pvd_unity.h"

#include "pvd/capture.hpp"
#include "pvd/mesh_builder.hpp"
#include "pvd/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

namespace render = pvd::render;

static_assert(sizeof(pu_instance) == 80 && offsetof(pu_instance, mesh_id) == 48);
static_assert(sizeof(pu_vertex) == 24 && offsetof(pu_vertex, nx) == 12);
static_assert(sizeof(float) == sizeof(uint32_t));
static_assert(static_cast<uint32_t>(pvd::CaptureState::idle) == 0 &&
              static_cast<uint32_t>(pvd::CaptureState::listening) == 1 &&
              static_cast<uint32_t>(pvd::CaptureState::receiving) == 2 &&
              static_cast<uint32_t>(pvd::CaptureState::completed) == 3 &&
              static_cast<uint32_t>(pvd::CaptureState::cancelled) == 4 &&
              static_cast<uint32_t>(pvd::CaptureState::truncated) == 5 &&
              static_cast<uint32_t>(pvd::CaptureState::failed) == 6);

thread_local std::array<char, 1024> last_error{};

template <class Operation> int invoke(Operation &&operation) noexcept {
    last_error[0] = '\0';
    try {
        operation();
        return 0;
    } catch (const std::exception &error) {
        std::snprintf(last_error.data(), last_error.size(), "%s", error.what());
    } catch (...) {
        std::snprintf(last_error.data(), last_error.size(), "Unknown PVD playback error");
    }
    return -1;
}

uint32_t checked_count(std::size_t count) {
    if (count > std::numeric_limits<uint32_t>::max())
        throw std::length_error("PVD playback count exceeds the C interface limit");
    return static_cast<uint32_t>(count);
}

uint32_t float_bits(float value) {
    uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Retain the immutable geometry behind each exported mesh ID. A historical
// seek can decode a resource again after its properties changed, so compare
// mesh contents when the extractor gives us a different resource pointer.
struct MeshKey {
    render::GeometryKind kind;
    std::shared_ptr<const render::MeshData> resource;
    std::array<uint32_t, 7> parameters{};

    bool operator<(const MeshKey &other) const {
        if (kind != other.kind)
            return kind < other.kind;
        if (parameters != other.parameters)
            return parameters < other.parameters;
        if (resource == other.resource)
            return false;
        if (!resource || !other.resource)
            return !resource;
        if (!(resource->resource == other.resource->resource))
            return resource->resource < other.resource->resource;
        if (resource->indices != other.resource->indices)
            return resource->indices < other.resource->indices;
        if (resource->vertices.size() != other.resource->vertices.size())
            return resource->vertices.size() < other.resource->vertices.size();
        const auto position = [](const pvd::Vec3 &v) { return std::tie(v.x, v.y, v.z); };
        const auto mismatch = std::mismatch(resource->vertices.begin(), resource->vertices.end(),
            other.resource->vertices.begin(), [&](const pvd::Vec3 &a, const pvd::Vec3 &b) {
                return position(a) == position(b);
            });
        return mismatch.first != resource->vertices.end() &&
            position(*mismatch.first) < position(*mismatch.second);
    }
};

MeshKey mesh_key(const render::Geometry &geometry) {
    MeshKey key{render::geometry_kind(geometry), {}};
    std::visit([&key](const auto &shape) {
        using Shape = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Shape, render::Box>) {
            const auto &half = shape.half_extents;
            key.parameters = {float_bits(half.x), float_bits(half.y), float_bits(half.z)};
        } else if constexpr (std::is_same_v<Shape, render::Sphere>) {
            key.parameters = {float_bits(shape.radius)};
        } else if constexpr (std::is_same_v<Shape, render::Capsule>) {
            key.parameters = {float_bits(shape.radius), float_bits(shape.half_height)};
        } else if constexpr (std::is_same_v<Shape, render::ConvexMesh> ||
                             std::is_same_v<Shape, render::TriangleMesh>) {
            key.resource = shape.mesh;
            const auto &scale = shape.scale.scale;
            const auto &rotation = shape.scale.rotation;
            key.parameters = {float_bits(scale.x), float_bits(scale.y), float_bits(scale.z),
                              float_bits(rotation.x), float_bits(rotation.y),
                              float_bits(rotation.z), float_bits(rotation.w)};
        } else if constexpr (std::is_same_v<Shape, render::HeightField>) {
            key.resource = shape.mesh;
            key.parameters = {float_bits(shape.row_scale), float_bits(shape.height_scale),
                              float_bits(shape.column_scale)};
        }
    }, geometry);
    return key;
}

std::optional<render::TriangleBuffer> build_mesh(const render::Geometry &geometry) {
    try {
        auto triangles = render::tessellate(geometry);
        if (!triangles.positions.empty() && !triangles.indices.empty())
            return triangles;
    } catch (const std::invalid_argument &) {
        // Match the viewer in skipping an invalid shape while replay continues.
    } catch (const std::length_error &) {
        // The data can exceed the tessellator's per-shape allocation budget.
    }
    return std::nullopt;
}

struct MeshRecord {
    render::Geometry geometry;
    uint32_t vertex_count, index_count;
};

pvd::SessionOptions playback_options() {
    pvd::SessionOptions options;
    options.max_checkpoints = 0;
    return options;
}

const pvd::QualifiedName material_class{"physx3", "PxMaterial"};

struct LiveSource {
    explicit LiveSource(pvd::CaptureOptions options)
        : receiver(std::make_unique<pvd::TcpCapture>(std::move(options))) {
        receiver->start();
        status = receiver->status();
    }

    std::unique_ptr<pvd::TcpCapture> receiver;
    pvd::CaptureStatus status;
    uint32_t imported_frame_count = 0;
    std::vector<uint64_t> preview_event_counts;
};

struct Playback {
    explicit Playback(const char *path)
        : session(std::make_unique<pvd::Session>(pvd::Session::load_file(path, playback_options()))) {
        index_lifetimes();
    }

    explicit Playback(pvd::CaptureOptions options) : live(std::in_place, std::move(options)) {}

    void index_lifetimes() {
        const auto &lifetimes = session->lifetimes();
        for (; indexed_lifetimes < lifetimes.size(); ++indexed_lifetimes) {
            const auto &life = lifetimes[indexed_lifetimes];
            if (life.class_name != material_class)
                spatial_ids.insert(life.id);
        }
    }

    uint32_t frame_count() const {
        if (live)
            return live->imported_frame_count;
        return checked_count(std::max(std::size_t{1}, session->frames().size()));
    }

    LiveSource &live_source() {
        if (!live)
            throw std::logic_error("PVD session is an offline capture");
        return *live;
    }

    void poll_live() {
        auto &source = live_source();
        if (!source.receiver)
            return;

        const auto cursor = session ? session->event_count() : 0;
        auto delta = source.receiver->try_poll_from(cursor, 8192, 8u * 1024u * 1024u);
        if (!delta)
            return;

        if (!session && delta->header)
            session = std::make_unique<pvd::Session>(*delta->header, playback_options());
        if (session && !delta->events.empty()) {
            session->append(std::move(delta->events));
            index_lifetimes();
            // The import budget can land inside a property transaction. Keep
            // the last published frame count until the transaction closes.
            if (session->at_transaction_boundary()) {
                source.imported_frame_count = checked_count(session->frames().size());
                if (source.imported_frame_count == 0)
                    source.preview_event_counts.push_back(session->event_count());
            }
        }

        const bool drained = delta->caught_up();
        source.status = std::move(delta->status);
        if (!source.status.terminal())
            return;
        if (!drained) {
            source.status.state = pvd::CaptureState::receiving;
            return;
        }

        if (session)
            session->finish();
        // The indexed copy retains the history; release the receive-side one.
        source.receiver.reset();
    }

    void stop_live() {
        auto &source = live_source();
        if (source.receiver) {
            source.receiver->cancel();
            source.receiver->wait();
        }
    }

    const MeshRecord &mesh(uint32_t id) const {
        if (id == 0 || id > meshes.size())
            throw std::out_of_range("PVD mesh ID is out of range");
        return meshes[id - 1];
    }

    uint32_t register_mesh(const render::Geometry &geometry) {
        auto [entry, inserted] = mesh_ids.try_emplace(mesh_key(geometry), 0);
        if (!inserted)
            return entry->second;
        try {
            auto triangles = build_mesh(geometry);
            if (!triangles)
                return 0;
            const auto id = checked_count(meshes.size() + 1);
            const auto vertex_count = checked_count(triangles->positions.size());
            const auto index_count = checked_count(triangles->indices.size());
            meshes.push_back({geometry, vertex_count, index_count});
            try {
                staged_meshes.emplace(id, std::move(*triangles));
            } catch (...) {
                meshes.pop_back();
                throw;
            }
            entry->second = id;
            return id;
        } catch (...) {
            mesh_ids.erase(entry);
            throw;
        }
    }

    void prepare(uint32_t index) {
        if (index >= frame_count())
            throw std::out_of_range("PVD frame index is out of range");

        const auto &frames = session->frames();
        prepare_at_event(frames.empty() ? session->event_count() : frames[index].event_count);
    }

    void prepare_live_preview(uint64_t event_count) {
        const auto &prefixes = live_source().preview_event_counts;
        if (!std::binary_search(prefixes.begin(), prefixes.end(), event_count))
            throw std::out_of_range("PVD live preview prefix was not published");

        prepare_at_event(static_cast<std::size_t>(event_count));
    }

    void prepare_at_event(std::size_t event_count) {
        if (prepared_event_count == event_count)
            return;

        const auto snapshot = session->snapshot_view_at_event(event_count);
        active_spatial_ids.clear();
        for (auto id : spatial_ids)
            if (const auto *object = snapshot.instance(id); object && object->class_name != material_class)
                active_spatial_ids.push_back(id);

        auto frame = extractor.extract(session->metadata(), snapshot.select_view(active_spatial_ids));
        staged_meshes.clear();
        next_instances.clear();
        next_instances.reserve(frame.instances.size());
        for (const auto &shape : frame.instances) {
            const auto mesh_id = register_mesh(shape.geometry);
            if (mesh_id == 0)
                continue;
            const auto &p = shape.world_pose.position;
            const auto &q = shape.world_pose.rotation;
            next_instances.push_back({shape.scene_id, shape.scene_generation,
                                      shape.actor_id, shape.actor_generation,
                                      shape.shape_id, shape.shape_generation, mesh_id,
                                      p.x, p.y, p.z, q.x, q.y, q.z, q.w});
        }
        instances.swap(next_instances);
        prepared_event_count = event_count;
    }

    std::unique_ptr<pvd::Session> session;
    std::optional<LiveSource> live;
    render::Extractor extractor;
    std::size_t indexed_lifetimes = 0;
    std::set<uint64_t> spatial_ids;
    std::vector<uint64_t> active_spatial_ids;
    std::map<MeshKey, uint32_t> mesh_ids;
    std::vector<MeshRecord> meshes;
    // Reuse the first tessellation for transfer. Once Unity has its own Mesh,
    // retain only the native geometry and ID for later seeks and occasional copies.
    std::map<uint32_t, render::TriangleBuffer> staged_meshes;
    std::vector<pu_instance> instances, next_instances;
    std::optional<std::size_t> prepared_event_count;
};

Playback &playback(void *session) {
    if (!session)
        throw std::invalid_argument("Missing PVD session");
    return *static_cast<Playback *>(session);
}

} // namespace

PU_API uint32_t PU_CALL pu_abi_version(void) noexcept {
    return 3;
}

PU_API int PU_CALL pu_open(const char *path, void **session) noexcept {
    return invoke([&] {
        if (!session)
            throw std::invalid_argument("Missing PVD session output");
        *session = nullptr;
        if (!path || !*path)
            throw std::invalid_argument("Missing PVD capture path");
        *session = new Playback(path);
    });
}

PU_API int PU_CALL pu_listen(const char *address, uint16_t port, void **session) noexcept {
    return invoke([&] {
        if (!session)
            throw std::invalid_argument("Missing PVD session output");
        *session = nullptr;
        if (!address || !*address)
            throw std::invalid_argument("Missing PVD listen address");
        pvd::CaptureOptions options;
        options.address = address;
        options.port = port;
        options.session_options = playback_options();
        *session = new Playback(std::move(options));
    });
}

PU_API int PU_CALL pu_get_live_status(void *session, uint32_t *state, uint32_t *frame_count,
                                      uint16_t *bound_port, uint64_t *wire_bytes) noexcept {
    return invoke([&] {
        if (!state || !frame_count || !bound_port || !wire_bytes)
            throw std::invalid_argument("Missing PVD live status output");
        auto &capture = playback(session);
        capture.poll_live();
        const auto &live = capture.live_source();
        *state = static_cast<uint32_t>(live.status.state);
        *frame_count = live.imported_frame_count;
        *bound_port = live.status.port;
        *wire_bytes = live.status.wire_bytes;
    });
}

PU_API int PU_CALL pu_get_live_preview_event_count(void *session, uint64_t *event_count) noexcept {
    return invoke([&] {
        if (!event_count)
            throw std::invalid_argument("Missing PVD live preview event count output");
        const auto &prefixes = playback(session).live_source().preview_event_counts;
        *event_count = prefixes.empty() ? 0 : prefixes.back();
    });
}

PU_API int PU_CALL pu_prepare_live_preview(void *session, uint64_t event_count,
                                            uint32_t *instance_count) noexcept {
    return invoke([&] {
        if (!instance_count)
            throw std::invalid_argument("Missing PVD instance count output");
        auto &capture = playback(session);
        capture.prepare_live_preview(event_count);
        *instance_count = checked_count(capture.instances.size());
    });
}

PU_API int PU_CALL pu_stop_live(void *session) noexcept {
    return invoke([&] { playback(session).stop_live(); });
}

PU_API const char *PU_CALL pu_live_error(void *session) noexcept {
    const auto *capture = static_cast<const Playback *>(session);
    if (!capture || !capture->live || capture->live->status.errors.empty())
        return nullptr;
    return capture->live->status.errors.back().c_str();
}

PU_API void PU_CALL pu_close(void *session) noexcept {
    delete static_cast<Playback *>(session);
}

PU_API int PU_CALL pu_get_frame_count(void *session, uint32_t *count) noexcept {
    return invoke([&] {
        if (!count)
            throw std::invalid_argument("Missing PVD frame count output");
        *count = playback(session).frame_count();
    });
}

PU_API int PU_CALL pu_prepare_frame(void *session, uint32_t frame, uint32_t *instance_count) noexcept {
    return invoke([&] {
        if (!instance_count)
            throw std::invalid_argument("Missing PVD instance count output");
        auto &capture = playback(session);
        capture.prepare(frame);
        *instance_count = checked_count(capture.instances.size());
    });
}

PU_API int PU_CALL pu_copy_instances(void *session, pu_instance *instances, uint32_t count) noexcept {
    return invoke([&] {
        const auto &capture = playback(session);
        if (!capture.prepared_event_count)
            throw std::logic_error("No PVD frame or live preview has been prepared");
        if (count != capture.instances.size() || (count && !instances))
            throw std::invalid_argument("PVD instance buffer does not match the prepared frame");
        if (count)
            std::copy(capture.instances.begin(), capture.instances.end(), instances);
    });
}

PU_API int PU_CALL pu_get_mesh_counts(void *session, uint32_t mesh_id,
                                     uint32_t *vertex_count, uint32_t *index_count) noexcept {
    return invoke([&] {
        if (!vertex_count || !index_count)
            throw std::invalid_argument("Missing PVD mesh count output");
        const auto &mesh = playback(session).mesh(mesh_id);
        *vertex_count = mesh.vertex_count;
        *index_count = mesh.index_count;
    });
}

PU_API int PU_CALL pu_copy_mesh(void *session, uint32_t mesh_id, pu_vertex *vertices,
                               uint32_t vertex_count, uint32_t *indices, uint32_t index_count) noexcept {
    return invoke([&] {
        auto &capture = playback(session);
        const auto &record = capture.mesh(mesh_id);
        if (!vertices || !indices || vertex_count != record.vertex_count || index_count != record.index_count)
            throw std::invalid_argument("PVD mesh buffers do not match the requested mesh");

        const auto staged = capture.staged_meshes.find(mesh_id);
        std::optional<render::TriangleBuffer> rebuilt;
        if (staged == capture.staged_meshes.end())
            rebuilt = render::tessellate(record.geometry);
        const auto &mesh = rebuilt ? *rebuilt : staged->second;
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const auto &p = mesh.positions[i];
            const auto &n = mesh.normals[i];
            vertices[i] = {p.x, p.y, p.z, n.x, n.y, n.z};
        }
        std::copy(mesh.indices.begin(), mesh.indices.end(), indices);
        if (staged != capture.staged_meshes.end())
            capture.staged_meshes.erase(staged);
    });
}

PU_API const char *PU_CALL pu_last_error(void) noexcept {
    return last_error.data();
}
