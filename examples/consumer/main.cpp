#include <pvd/protocol.hpp>
#include <pvd/writer.hpp>
#ifdef PVD_CONSUMER_TRANSPORT
#include <pvd/capture.hpp>
#endif
#ifdef PVD_CONSUMER_RENDER
#include <pvd/mesh_builder.hpp>
#include <pvd/deformables.hpp>
#endif
#ifdef PVD_CONSUMER_ANALYSIS
#include <pvd/profile_curves.hpp>
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
        const auto frame = extractor.extract(session.metadata(), state);
        for (const auto& shape : frame.instances) {
            const auto triangles = pvd::render::tessellate(shape.geometry);
            if (triangles.positions.empty() || triangles.indices.empty())
                throw std::runtime_error("installed render module generated an empty shape");
        }
        std::cout << "render_instances=" << frame.instances.size() << '\n';
        auto deformable = pvd::deformables::extract(session.metadata(), state);
        std::cout << "cloths=" << deformable.cloths.size()
                  << " particle_clouds=" << deformable.particle_clouds.size() << '\n';
#endif
#ifdef PVD_CONSUMER_ANALYSIS
        pvd::profile::Index profile(session);
        pvd::memory::Index memory(session);
        pvd::errors::Index errors(session);
        pvd::analysis::ProfileCurves curves(session);
        if (!session.frames().empty()) {
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
