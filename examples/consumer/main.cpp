#include <pvd/protocol.hpp>
#include <pvd/writer.hpp>
#include <pvd/clips.hpp>
#include <pvd/coordinates.hpp>
#ifdef PVD_CONSUMER_TRANSPORT
#include <pvd/capture.hpp>
#endif
#ifdef PVD_CONSUMER_RENDER
#include <pvd/mesh_builder.hpp>
#include <pvd/deformables.hpp>
#include <pvd/scene_queries.hpp>
#include <pvd/body_overlays.hpp>
#include <pvd/coordinate_frames.hpp>
#include <pvd/constraint_builder.hpp>
#endif
#ifdef PVD_CONSUMER_ANALYSIS
#include <pvd/profile_curves.hpp>
#include <pvd/frame_timing.hpp>
#include <pvd/memory.hpp>
#include <pvd/errors.hpp>
#endif
#ifdef PVD_CONSUMER_REPX
#include <pvd/repx.hpp>
#endif

#include <array>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pvd_external_consumer <raw-capture.pxd2>\n";
        return 2;
    }
    try {
#ifdef PVD_CONSUMER_TRANSPORT
        pvd::CaptureOptions settings;
        settings.port = 0;
        pvd::TcpCapture capture(settings);
        capture.start();
        capture.cancel();
        capture.wait();
        if (capture.status().state != pvd::CaptureState::cancelled)
            throw std::runtime_error("installed TCP module cancellation failed");
        const auto polled = capture.try_poll_from(0);
        if (!polled || !polled->events.empty() || !polled->caught_up())
            throw std::runtime_error("installed nonblocking capture poll failed");
#endif
        std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input");
        pvd::StreamDecoder decoder;
        std::array<std::uint8_t, 16384> chunk{};
        while (input.read(reinterpret_cast<char*>(chunk.data()), chunk.size()) || input.gcount())
            decoder.feed(chunk.data(), static_cast<std::size_t>(input.gcount()));
        if (input.bad()) throw std::runtime_error("input read failed");
        decoder.finish();
        pvd::Session session(*decoder.header(), decoder.events());
        auto state = session.frames().empty() ? session.latest() : session.snapshot_at_frame(0);
        const auto view = session.snapshot_view_at_event(state.event_count);
        if (view.event_count != state.event_count || view.instances.size() != state.instances.size())
            throw std::runtime_error("installed immutable snapshot view differs from full state");
        if (session.estimated_view_cache_bytes() > pvd::SessionOptions{}.max_view_cache_bytes)
            throw std::runtime_error("installed derived cache exceeds its estimated budget");
        if (!state.instances.empty()) {
            const auto &entry = *state.instances.begin();
            const auto selected = session.snapshot_selected_at_event(state.event_count, {entry.first});
            const auto single = session.instance_at_event(state.event_count, entry.first);
            const auto *shared = view.instance(entry.first);
            if (!shared || !single || selected.instances.size() != 1 ||
                shared->class_name != entry.second.class_name ||
                shared->generation != entry.second.generation ||
                shared->properties.size() != entry.second.properties.size() ||
                single->properties.size() != shared->properties.size())
                throw std::runtime_error("installed selected/view instance query failed");
            for (const auto &property : entry.second.properties) {
                const auto *value = shared->property(property.first);
                if (!value || value->raw_bytes() != property.second.raw_bytes() ||
                    value->reference_ids != property.second.reference_ids)
                    throw std::runtime_error("installed immutable view property differs");
            }
            if (view.select({entry.first}).instances.size() != 1 ||
                view.materialize().instances.size() != state.instances.size())
                throw std::runtime_error("installed view materialization failed");
            const auto shared_subset = view.select_view({entry.first, entry.first});
            if (shared_subset.instances.size() != 1 || shared_subset.instance(entry.first) != shared)
                throw std::runtime_error("installed view projection must share immutable instances");
        }
        // These are installed core APIs even with the viewer/SDK disabled.
        if (!session.frames().empty()) {
            const auto &marker = session.frames().front();
            const auto *scene = state.instance(marker.scene_id);
            if (!scene) throw std::runtime_error("native frame has no scene");
            auto catalog = pvd::clips::Catalog::create(session, {scene->id, scene->generation});
            const auto restored = pvd::clips::restore_catalogs(session, {catalog.state()});
            if (restored.size() != 1 || !restored.front().catalog)
                throw std::runtime_error("installed core clip catalog validation failed");
            std::cout << "clip_frames=" << catalog.selected()->frames.count() << '\n';
        }
        for (const auto &entry : state.instances) {
            if (const auto *pose = entry.second.property("GlobalPose")) {
                if (!pvd::coordinates::native_transform(*pose))
                    throw std::runtime_error("installed native coordinate accessor failed");
                break;
            }
        }
        std::ostringstream saved(std::ios::binary);
        const auto written = pvd::writing::write_session(saved, session);
        const auto bytes = saved.str();
        pvd::StreamDecoder replay;
        replay.feed(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        replay.finish();
        if (replay.events().size() != written.event_count)
            throw std::runtime_error("installed raw writer roundtrip failed");
#ifdef PVD_CONSUMER_RENDER
        pvd::render::Extractor extractor;
        const auto frame = extractor.extract(session.metadata(), view);
        for (const auto& shape : frame.instances) {
            const auto triangles = pvd::render::tessellate(shape.geometry);
            if (triangles.positions.empty() || triangles.indices.empty())
                throw std::runtime_error("installed render module generated an empty shape");
        }
        std::cout << "render_instances=" << frame.instances.size() << '\n';
        pvd::render::BodyOverlayOptions overlay_options;
        overlay_options.show_linear_velocity = true;
        const auto overlays = pvd::render::build_body_overlays(frame, view, overlay_options);
        pvd::render::CoordinateFrameOptions coordinate_options;
        coordinate_options.actor = true;
        const auto axes = pvd::render::build_coordinate_frames(frame, view, coordinate_options);
        if (!axes.complete) throw std::runtime_error("installed coordinate-frame extraction failed");
        std::cout << "linear_velocity_vectors=" << overlays.linear_vectors
                  << " overlay_diagnostics=" << overlays.diagnostics.size() << '\n';
        if (!frame.instances.empty()) {
            const auto &shape = frame.instances.front();
            const auto geometry = extractor.extract_geometry(session.metadata(), view,
                {shape.geometry_id, shape.geometry_generation});
            if (!geometry.geometry) throw std::runtime_error("installed standalone geometry extraction failed");
        }
        if (!session.frames().empty()) {
            const auto &marker = session.frames().front();
            pvd::render::SceneQueryIndex query_index;
            const auto queries = query_index.extract(session, marker);
            auto debug_options = pvd::render::DebugOptions{};
            debug_options.joint_axis_length = 0;
            const auto debug = pvd::render::extract_debug(session, marker, debug_options);
            const auto constraints = pvd::render::build_constraint_lines(debug.constraints);
            std::cout << "scene_queries=" << queries.queries.size()
                      << " query_collections=" << queries.collections.size()
                      << " constraint_lines=" << constraints.lines.size() << '\n';
        }
        auto deformable = pvd::deformables::extract(session.metadata(), view);
        std::cout << "cloths=" << deformable.cloths.size()
                  << " particle_clouds=" << deformable.particle_clouds.size() << '\n';
#endif
#ifdef PVD_CONSUMER_ANALYSIS
        pvd::profile::Index profile(session);
        pvd::memory::Index memory(session);
        pvd::errors::Index errors(session);
        pvd::analysis::ProfileCurves curves(session);
        if (!session.frames().empty()) {
            pvd::analysis::FrameTimingRequest timing_request;
            timing_request.source.scene_id = session.frames().front().scene_id;
            timing_request.metric = pvd::analysis::FrameTimingMetric::ticks;
            const auto timing = pvd::analysis::sample_frame_timing(session, timing_request);
            if (!timing.complete || timing.samples.empty())
                throw std::runtime_error("installed native frame timing API failed");
            pvd::analysis::ProfileCurveRequest request;
            request.source.scene_id = session.frames().front().scene_id;
            auto series = curves.sample(request);
            std::ostringstream csv;
            pvd::analysis::write_profile_curve_csv(csv, series);
            std::cout << "profile_curve_samples=" << series.samples.size() << '\n';
        }
        auto summary = memory.summary_at_event(session.event_count());
        std::cout << "profile_groups=" << profile.aggregate().size()
                  << " memory_groups=" << summary.groups.size() << '\n';
#endif
#ifdef PVD_CONSUMER_REPX
        pvd::repx::Options options;
        options.unsupported = pvd::repx::UnsupportedPolicy::allow_partial;
        auto xml = pvd::repx::export_snapshot(session.metadata(), state, options);
        if (xml.actor_count == 0 || xml.xml.empty())
            throw std::runtime_error("installed RepX exporter produced no actors");
        std::cout << "repx_actors=" << xml.actor_count
                  << " repx_xml_bytes=" << xml.xml.size() << '\n';
#endif
        std::cout << "version=" << decoder.header()->stream_version
                  << " groups=" << decoder.group_count()
                  << " events=" << decoder.events().size() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
