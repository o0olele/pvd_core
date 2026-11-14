#include <pvd/protocol.hpp>
#ifdef PVD_CONSUMER_TRANSPORT
#include <pvd/capture.hpp>
#endif
#ifdef PVD_CONSUMER_RENDER
#include <pvd/mesh_builder.hpp>
#endif

#include <array>
#include <fstream>
#include <iostream>

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
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input");
        pvd::StreamDecoder decoder;
        std::array<std::uint8_t, 16384> chunk{};
        while (input.read(reinterpret_cast<char*>(chunk.data()), chunk.size()) || input.gcount())
            decoder.feed(chunk.data(), static_cast<std::size_t>(input.gcount()));
        if (input.bad()) throw std::runtime_error("input read failed");
        decoder.finish();
#ifdef PVD_CONSUMER_RENDER
        pvd::Session session(*decoder.header(), decoder.events());
        auto state = session.frames().empty() ? session.latest() : session.snapshot_at_frame(0);
        pvd::render::Extractor extractor;
        const auto frame = extractor.extract(session.metadata(), state);
        for (const auto& shape : frame.instances) {
            const auto triangles = pvd::render::tessellate(shape.geometry);
            if (triangles.positions.empty() || triangles.indices.empty())
                throw std::runtime_error("installed render module generated an empty shape");
        }
        std::cout << "render_instances=" << frame.instances.size() << '\n';
#endif
        std::cout << "version=" << decoder.header()->stream_version
                  << " groups=" << decoder.group_count()
                  << " events=" << decoder.events().size() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
