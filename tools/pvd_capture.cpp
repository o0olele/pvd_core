#include <pvd/capture.hpp>

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void signal_handler(int) { stop_requested = 1; }
unsigned long long number(const std::string& text) {
    if (text.empty() || text.front() == '-') throw std::invalid_argument("expected a nonnegative integer");
    std::size_t used = 0;
    const auto result = std::stoull(text, &used);
    if (used != text.size()) throw std::invalid_argument("invalid integer: " + text);
    return result;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::cout << "usage: pvd_capture output.pxd2 [--bind 127.0.0.1] [--port 5425] [--seconds N]\n"
                     "Accept one PhysX PVD connection. Ctrl+C stops capture. Existing files are never overwritten.\n";
        return argc < 2 ? 2 : 0;
    }
    try {
        pvd::CaptureOptions options;
        options.recording_path = argv[1];
        unsigned long long seconds = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--bind" && i + 1 < argc) options.address = argv[++i];
            else if (arg == "--port" && i + 1 < argc) {
                const auto port = number(argv[++i]);
                if (port > 65535) throw std::invalid_argument("port must be in [0, 65535]");
                options.port = static_cast<std::uint16_t>(port);
            } else if (arg == "--seconds" && i + 1 < argc) {
                seconds = number(argv[++i]);
                if (seconds > 86400) throw std::invalid_argument("seconds must be <= 86400");
            } else throw std::invalid_argument("unknown or incomplete option: " + arg);
        }
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        pvd::TcpCapture capture(options);
        capture.start();
        const auto initial = capture.status();
        std::cout << "Listening on " << initial.address << ':' << initial.port
                  << "; recording " << options.recording_path << std::endl;
        const auto started = std::chrono::steady_clock::now();
        while (!capture.status().terminal()) {
            if (stop_requested || (seconds && std::chrono::steady_clock::now() - started >= std::chrono::seconds(seconds))) {
                capture.cancel();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        capture.wait();
        const auto status = capture.status();
        std::cout << pvd::capture_state_name(status.state) << ": " << status.frame_count << " frames, "
                  << status.event_count << " events, " << status.wire_bytes << " bytes received, "
                  << status.recorded_bytes << " bytes recorded, " << status.discarded_tail_bytes << " tail bytes omitted\n";
        for (const auto& error : status.errors) std::cerr << error << '\n';
        return status.state == pvd::CaptureState::completed || status.state == pvd::CaptureState::cancelled ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "pvd_capture: " << error.what() << '\n';
        return 1;
    }
}
