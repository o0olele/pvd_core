#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pvd/capture.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace {
void require(bool condition, const std::string& text) {
    if (!condition) throw std::runtime_error(text);
}
std::wstring quote(const std::wstring& text) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
class Producer {
    PROCESS_INFORMATION process_{};
public:
    explicit Producer(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments) {
        auto command = quote(executable.wstring());
        for (const auto& arg : arguments) command += L' ' + quote(arg);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        require(CreateProcessW(executable.wstring().c_str(), command.data(), nullptr, nullptr,
                              FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_) != 0,
                "cannot start SDK producer: " + std::to_string(GetLastError()));
    }
    bool exited() const { return WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0; }
    DWORD exit_code() const {
        DWORD code = STILL_ACTIVE;
        require(GetExitCodeProcess(process_.hProcess, &code) != 0, "cannot query SDK producer");
        return code;
    }
    ~Producer() {
        if (!exited()) { TerminateProcess(process_.hProcess, 9); WaitForSingleObject(process_.hProcess, 2000); }
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
    }
};
void equal_state(const pvd::Snapshot& a, const pvd::Snapshot& b) {
    require(a.event_count == b.event_count && a.instances.size() == b.instances.size(), "live/offline state size differs");
    for (const auto& pair : a.instances) {
        const auto* other = b.instance(pair.first);
        require(other && pair.second.class_name == other->class_name && pair.second.generation == other->generation,
                "live/offline instance identity differs");
        const auto& object = pair.second;
        require(object.properties.size() == other->properties.size() && object.pickable == other->pickable
                && object.top_level == other->top_level, "live/offline object attributes differ");
        for (const auto& item : object.properties) {
            const auto* value = other->property(item.first);
            require(value && item.second.incoming_type == value->incoming_type
                    && item.second.raw_bytes() == value->raw_bytes()
                    && item.second.item_count == value->item_count && item.second.text == value->text
                    && item.second.reference_ids == value->reference_ids && item.second.opaque == value->opaque,
                    "live/offline property differs: " + item.first);
        }
    }
}
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const auto end = line.find(',', start);
        result.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
std::size_t compare_csv(const pvd::Session& offline, const std::filesystem::path& path) {
    std::vector<std::size_t> after_fetch;
    for (const auto& section : offline.sections())
        if (section.name == "validation_after_fetch") after_fetch.push_back(section.event_count);
    std::ifstream input(path);
    require(bool(input), "cannot open SDK expected values");
    std::string line;
    std::getline(input, line);
    std::size_t checked = 0;
    std::size_t cached_step = 0;
    pvd::Snapshot state;
    while (std::getline(input, line)) {
        const auto row = fields(line);
        require(row.size() == 12, "invalid CSV from SDK producer");
        const auto step = std::stoull(row[0]);
        if (!step) continue;
        if (step != cached_step) { state = offline.snapshot_at_event(after_fetch.at(step - 1)); cached_step = step; }
        const auto* instance = state.instance(std::stoull(row[3]));
        const bool alive = std::stoi(row[4]) != 0;
        require(bool(instance) == alive, "TCP capture actor lifecycle differs from SDK");
        if (instance) {
            const auto* property = instance->property("GlobalPose");
            require(property != nullptr, "missing SDK GlobalPose");
            const auto pose = property->as_transform();
            require(bool(pose), "SDK GlobalPose type differs");
            const float actual[] = {pose->position.x, pose->position.y, pose->position.z,
                pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w};
            for (std::size_t i = 0; i < 7; ++i) {
                const auto expected = std::stof(row[5 + i]);
                require(std::isfinite(actual[i]) && std::abs(actual[i] - expected) < 0.00005f,
                        "TCP capture pose differs from SDK at step " + row[0]);
            }
        }
        ++checked;
    }
    return checked;
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        constexpr unsigned steps = 600;
        const auto unique = std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory = std::filesystem::path(argv[2]) / ("live_" + unique);
        std::filesystem::create_directories(directory);
        const auto recording = directory / "received.pxd2";
        const auto expected_prefix = directory / "sdk";
        pvd::CaptureOptions options;
        options.port = 0;
        options.recording_path = recording.string();
        options.read_buffer_bytes = 257;
        options.session_options.checkpoint_interval = 64;
        options.session_options.max_checkpoints = 128;
        pvd::TcpCapture capture(options);
        capture.start();
        Producer producer(std::filesystem::absolute(argv[1]), {L"--tcp", L"127.0.0.1",
            std::to_wstring(capture.status().port), expected_prefix.wstring(), std::to_wstring(steps), L"1"});
        std::vector<pvd::Snapshot> observed;
        std::size_t last_events = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!producer.exited() || !capture.status().terminal()) {
            require(std::chrono::steady_clock::now() < deadline, "live SDK capture timed out");
            const auto status = capture.status();
            if (status.state == pvd::CaptureState::failed)
                throw std::runtime_error(status.errors.empty() ? "capture failed" : status.errors.front());
            if (status.event_count > last_events && !status.terminal()) {
                last_events = status.event_count;
                if (observed.size() < 128) observed.push_back(capture.latest());
                if (status.frame_count) {
                    const auto old = capture.snapshot_at_frame(status.frame_count / 2);
                    if (observed.size() < 128) observed.push_back(old);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(producer.exit_code() == 0, "SDK producer exited unsuccessfully");
        capture.wait();
        const auto status = capture.status();
        require(status.state == pvd::CaptureState::completed && status.errors.empty(), "live SDK capture did not complete cleanly");
        require(status.wire_bytes == status.recorded_bytes && status.discarded_tail_bytes == 0,
                "clean SDK stream was not recorded exactly");
        require(observed.size() > 4, "did not query enough states while receiving");
        auto offline = pvd::Session::load_file(recording.string(), options.session_options);
        require(offline.events().size() == status.event_count && offline.frames().size() == steps * 2,
                "two-scene frame/event counts differ");
        const auto live_frames = capture.frames();
        require(live_frames.size() == offline.frames().size(), "live frame index differs");
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> scenes;
        for (const auto& frame : offline.frames()) ++scenes[{frame.stream_id, frame.scene_id}];
        require(scenes.size() == 2, "expected independent scene/stream frame indices");
        for (const auto& scene : scenes) require(scene.second == steps, "scene frame count differs");
        for (const auto& state : observed) equal_state(state, offline.snapshot_at_event(state.event_count));
        for (std::size_t i = 0; i < offline.frames().size(); i += 11)
            equal_state(capture.snapshot_at_frame(i), offline.snapshot_at_frame(i));
        equal_state(capture.latest(), offline.latest());
        const auto checked = compare_csv(offline, expected_prefix.string() + "_expected.csv");
        std::cout << "Live PhysX 3.4: " << steps << " steps x 2 scenes, " << offline.frames().size()
                  << " frames, " << observed.size() << " concurrent snapshots, " << checked
                  << " independent SDK object states, " << status.wire_bytes << " raw bytes; replay identical\n";
    } catch (const std::exception& error) {
        std::cerr << "Live PhysX integration: " << error.what() << '\n';
        return 1;
    }
}
