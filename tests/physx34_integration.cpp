#include <pvd/session.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Expected {
    std::size_t step;
    std::string name;
    std::uint64_t id;
    bool alive;
    pvd::Transform pose;
};
std::vector<Expected> load_csv(const char* path) {
    std::ifstream input(path);
    require(bool(input), "cannot open expected CSV");
    std::string line;
    std::getline(input, line);
    require(line.find("step,time_seconds,object_name,instance_id,alive,px,py,pz,qx,qy,qz,qw") == 0,
            "unexpected expected CSV schema");
    std::vector<Expected> rows;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t start = 0;
        for (;;) {
            const auto end = line.find(',', start);
            fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        require(fields.size() == 12, "invalid expected CSV row");
        Expected row{};
        row.step = std::stoull(fields[0]); row.name = fields[2]; row.id = std::stoull(fields[3]);
        row.alive = std::stoi(fields[4]) != 0;
        if (row.alive) {
            row.pose.position = {std::stof(fields[5]), std::stof(fields[6]), std::stof(fields[7])};
            row.pose.rotation = {std::stof(fields[8]), std::stof(fields[9]), std::stof(fields[10]), std::stof(fields[11])};
        }
        rows.push_back(std::move(row));
    }
    require(!rows.empty(), "empty expected CSV");
    return rows;
}
void near(float actual, float expected, const std::string& context) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= 0.00005f,
            context + ": " + std::to_string(actual) + " != " + std::to_string(expected));
}
std::size_t verify(const pvd::Session& session, const std::vector<Expected>& rows,
                   const std::vector<std::size_t>& after_fetch, bool frame_markers, bool reverse) {
    std::size_t checked = 0;
    std::map<std::size_t, std::vector<Expected>> by_step;
    for (const auto& row : rows) if (row.step) by_step[row.step].push_back(row);
    auto frame_check = [&](std::size_t step, const std::vector<Expected>& expected) {
        const auto state = frame_markers ? session.snapshot_at_frame(step - 1)
            : session.snapshot_at_event(after_fetch.at(step - 1));
        for (const auto& row : expected) {
            const auto* object = state.instance(row.id);
            const auto context = "step=" + std::to_string(step) + " object=" + row.name;
            require(bool(object) == row.alive, context + " lifecycle differs");
            if (!object) { ++checked; continue; }
            const auto* name = object->property("Name");
            require(name && name->as_string() == std::optional<std::string>(row.name), context + " name differs");
            const auto* value = object->property("GlobalPose");
            require(value != nullptr, context + " missing GlobalPose");
            const auto pose = value->as_transform();
            require(bool(pose), context + " GlobalPose is not a transform");
            near(pose->position.x, row.pose.position.x, context + " px");
            near(pose->position.y, row.pose.position.y, context + " py");
            near(pose->position.z, row.pose.position.z, context + " pz");
            near(pose->rotation.x, row.pose.rotation.x, context + " qx");
            near(pose->rotation.y, row.pose.rotation.y, context + " qy");
            near(pose->rotation.z, row.pose.rotation.z, context + " qz");
            near(pose->rotation.w, row.pose.rotation.w, context + " qw");
            ++checked;
        }
    };
    if (reverse) for (auto it = by_step.rbegin(); it != by_step.rend(); ++it) frame_check(it->first, it->second);
    else for (const auto& item : by_step) frame_check(item.first, item.second);
    return checked;
}
std::vector<std::size_t> after_fetch_markers(const pvd::Session& session) {
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::string> strings;
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < session.events().size(); ++i) {
        const auto& event = session.events()[i];
        if (const auto* text = std::get_if<pvd::StringHandleEvent>(&event.payload))
            strings[{event.stream_id, text->handle}] = text->string;
        if (const auto* end = std::get_if<pvd::EndSection>(&event.payload)) {
            const auto text = strings.find({event.stream_id, end->name});
            if (text != strings.end() && text->second == "validation_after_fetch") {
                require(!session.frames().empty() && end->section_id == session.frames().front().scene_id,
                        "after-fetch marker uses an unexpected scene");
                result.push_back(i + 1);
            }
        }
    }
    return result;
}
}

int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "usage: integration capture.pxd2 after-fetch.csv frame-boundary.csv\n"; return 2; }
    try {
        const auto expected = load_csv(argv[2]);
        const auto frame_expected = load_csv(argv[3]);
        pvd::SessionOptions options;
        options.checkpoint_interval = 32;
        auto session = pvd::Session::load_file(argv[1], options);
        const auto max_step = std::max_element(expected.begin(), expected.end(),
            [](const auto& a, const auto& b) { return a.step < b.step; })->step;
        require(max_step >= 10 && session.frames().size() == max_step, "simulation/frame count differs");
        const auto markers = after_fetch_markers(session);
        require(markers.size() == max_step, "after-fetch marker count differs");
        require(session.checkpoint_count() > 0, "physical capture did not exercise checkpoints");
        const auto forward = verify(session, expected, markers, false, false)
            + verify(session, frame_expected, markers, true, false);
        const auto backward = verify(session, expected, markers, false, true)
            + verify(session, frame_expected, markers, true, true);
        pvd::SessionOptions no_checkpoints;
        no_checkpoints.max_checkpoints = 0;
        auto baseline = pvd::Session::load_file(argv[1], no_checkpoints);
        const auto replayed = verify(baseline, expected, markers, false, true)
            + verify(baseline, frame_expected, markers, true, true);
        const auto eof = session.latest();
        for (const auto& row : expected)
            require(!eof.instance(row.id), "SDK actor cleanup was not reflected at EOF: " + row.name);
        std::cout << "PhysX 3.4 verified " << session.frames().size() << " frames, "
                  << forward << " forward and " << backward << " reverse object states, "
                  << replayed << " checkpoint-free states; " << session.events().size() << " events\n";
    } catch (const std::exception& error) {
        std::cerr << "PhysX integration: " << error.what() << '\n';
        return 1;
    }
}
