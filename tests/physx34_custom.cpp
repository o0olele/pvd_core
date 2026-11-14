#include <pvd/session.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Expected {
    std::size_t step = 0;
    std::string name;
    std::uint64_t id = 0, value = 0, parent = 0;
    bool alive = false;
    pvd::Vec3 position;
    std::size_t children_count = 0;
    std::vector<float> samples;
};
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
std::vector<Expected> load_expected(const char* path) {
    std::ifstream input(path);
    require(bool(input), "cannot open custom expected CSV");
    std::string line; std::getline(input, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    require(line == "step,object_name,instance_id,alive,value,x,y,z,parent_id,children_count,sample0,sample1,sample2", "custom expected CSV schema differs");
    std::vector<Expected> rows;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto cells = fields(line);
        require(cells.size() == 13, "custom expected CSV must have 13 columns");
        Expected row;
        row.step = std::stoull(cells[0]); row.name = cells[1]; row.id = std::stoull(cells[2]);
        row.alive = std::stoi(cells[3]) != 0;
        if (row.alive) {
            row.value = std::stoull(cells[4]);
            row.position = {std::stof(cells[5]), std::stof(cells[6]), std::stof(cells[7])};
            row.parent = std::stoull(cells[8]); row.children_count = std::stoull(cells[9]);
            for (std::size_t i = 10; i < 13; ++i) if (!cells[i].empty()) row.samples.push_back(std::stof(cells[i]));
        }
        rows.push_back(std::move(row));
    }
    require(rows.size() == 24, "custom CSV must describe two objects over 12 frames");
    return rows;
}
const pvd::PropertyValue& property(const pvd::InstanceState& instance, const std::string& name) {
    const auto* value = instance.property(name);
    require(value != nullptr, "object " + std::to_string(instance.id) + " is missing property " + name);
    return *value;
}
void near(float actual, float expected, const std::string& context) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 0.00001f,
            context + ": " + std::to_string(actual) + " != " + std::to_string(expected));
}
void verify_row(const pvd::Snapshot& state, const Expected& expected, std::uint64_t child_id) {
    const auto context = "custom frame=" + std::to_string(expected.step) + " object=" + expected.name;
    const auto* object = state.instance(expected.id);
    require(bool(object) == expected.alive, context + " lifecycle differs");
    if (!object) return;
    require(property(*object, "name").as_string() == std::optional<std::string>(expected.name), context + " StringHandle/string resolution differs");
    require(property(*object, "value").as_uint() == std::optional<std::uint64_t>(expected.value), context + " message uint value differs");
    const auto position = property(*object, "position").as_vec3();
    require(bool(position), context + " message Vec3 type differs");
    near(position->x, expected.position.x, context + " x"); near(position->y, expected.position.y, context + " y"); near(position->z, expected.position.z, context + " z");
    const auto* parent = object->property("parent");
    if (expected.parent) require(parent && parent->as_object_ref() == std::optional<std::uint64_t>(expected.parent), context + " parent reference differs");
    else require(!parent || parent->as_object_ref() == std::optional<std::uint64_t>(0), context + " unexpected parent reference");
    const auto* children = object->property("children");
    if (expected.children_count) {
        require(children && children->item_count == expected.children_count, context + " children count differs");
        require(children->as_object_ref(0) == std::optional<std::uint64_t>(child_id), context + " pushed reference differs");
    } else require(!children || children->item_count == 0, context + " removed reference remains");
    if (!expected.samples.empty()) {
        const auto& samples = property(*object, "samples");
        require(samples.item_count == 3 && samples.raw_bytes().size() == 12, context + " sample array shape differs");
        for (std::size_t i = 0; i < 3; ++i) {
            const auto value = samples.as_float(i); require(bool(value), context + " sample type differs");
            near(*value, expected.samples[i], context + " sample" + std::to_string(i));
        }
        bool out_of_range = false; try { (void)samples.as_float(3); } catch (const std::out_of_range&) { out_of_range = true; }
        require(out_of_range, context + " sample indexing should stop at array end");
    }
}
void same_snapshot(const pvd::Snapshot& actual, const pvd::Snapshot& baseline) {
    require(actual.event_count == baseline.event_count && actual.instances.size() == baseline.instances.size(), "checkpoint/replay snapshot shape differs");
    for (const auto& entry : actual.instances) {
        const auto* rhs = baseline.instance(entry.first); const auto& lhs = entry.second;
        require(rhs && lhs.class_name == rhs->class_name && lhs.generation == rhs->generation && lhs.creating_stream == rhs->creating_stream, "checkpoint/replay object identity differs");
        require(lhs.properties.size() == rhs->properties.size() && lhs.pickable == rhs->pickable && lhs.top_level == rhs->top_level, "checkpoint/replay object flags differ");
        require(lhs.color.has_value() == rhs->color.has_value(), "checkpoint/replay object color differs");
        if (lhs.color) require(lhs.color->r == rhs->color->r && lhs.color->g == rhs->color->g && lhs.color->b == rhs->color->b && lhs.color->a == rhs->color->a, "checkpoint/replay color channels differ");
        for (const auto& item : lhs.properties) {
            const auto* value = rhs->property(item.first); const auto& original = item.second;
            require(value && original.incoming_type == value->incoming_type && original.item_count == value->item_count && original.raw_bytes() == value->raw_bytes() && original.text == value->text && original.reference_ids == value->reference_ids && original.byte_order == value->byte_order && original.pointer_width == value->pointer_width && original.opaque == value->opaque,
                    "checkpoint/replay property differs: " + item.first);
        }
    }
    require(actual.cameras.empty() && baseline.cameras.empty() && actual.origin_shifts.empty() && baseline.origin_shifts.empty(), "custom fixture unexpectedly contains camera/origin state");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: custom_integration custom_capture.pxd2 custom_expected.csv\n"; return 2; }
    try {
        const auto expected = load_expected(argv[2]);
        std::map<std::size_t, std::vector<Expected>> frames;
        std::uint64_t root_id = 0, child_id = 0;
        for (const auto& row : expected) {
            frames[row.step].push_back(row);
            if (row.name == "fixture_root") root_id = row.id;
            else if (row.name == "fixture_child") child_id = row.id;
            else throw std::runtime_error("unknown custom fixture object name");
        }
        require(root_id && child_id && root_id != child_id && frames.size() == 12, "invalid custom fixture IDs/frames");
        pvd::SessionOptions accelerated; accelerated.checkpoint_interval = 7; accelerated.max_checkpoints = 64;
        auto session = pvd::Session::load_file(argv[1], accelerated);
        pvd::SessionOptions unindexed; unindexed.max_checkpoints = 0;
        auto baseline = pvd::Session::load_file(argv[1], unindexed);
        require(session.frames().size() == 12 && baseline.frames().size() == 12, "custom section/frame count differs");
        require(session.checkpoint_count() > 1 && baseline.checkpoint_count() == 0, "checkpoint configurations were not exercised");
        const pvd::QualifiedName node{"pvd_core_fixture", "Node"};
        const pvd::QualifiedName derived{"pvd_core_fixture", "DerivedNode"};
        const pvd::QualifiedName update{"pvd_core_fixture", "NodeUpdate"};
        require(session.metadata().is_a(derived, node), "derived-class relation was not recovered");
        require(session.metadata().find_property(derived, "samples") != nullptr, "inherited property was not recovered");
        const auto message = session.metadata().messages.find(update);
        require(message != session.metadata().messages.end() && message->second.byte_size == 16 && message->second.arguments.size() == 2, "property message metadata differs");
        require(message->second.arguments[0].offset == 0 && message->second.arguments[0].byte_size == 4 && message->second.arguments[1].offset == 4 && message->second.arguments[1].byte_size == 12, "property message byte offsets differ");
        std::set<unsigned> tags; for (const auto& event : session.events()) tags.insert(event.tag());
        for (auto tag : {3u, 5u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 16u, 17u, 18u, 19u})
            require(tags.count(tag) != 0, "SDK fixture did not exercise required wire tag " + std::to_string(tag));
        const auto& first = session.frames().front();
        const auto initial = session.snapshot_at_event(first.begin_event_count);
        const auto* root = initial.instance(root_id);
        require(root != nullptr, "root missing before first frame");
        const auto& initial_samples = property(*root, "samples");
        require(initial_samples.item_count == 3 && initial_samples.raw_bytes().size() == 12, "chunked property did not combine array fragments");
        for (std::size_t i = 0; i < 3; ++i) require(initial_samples.as_float(i) == std::optional<float>(float(i + 1)), "initial chunked array content differs");
        same_snapshot(initial, baseline.snapshot_at_event(first.begin_event_count));
        std::size_t comparisons = 0;
        auto frame_check = [&](std::size_t step, const std::vector<Expected>& rows) {
            const auto index = step - 1;
            const auto state = session.snapshot_at_frame(index);
            const auto plain = baseline.snapshot_at_frame(index);
            const auto& marker = session.frames()[index];
            require(marker.scene_id == root_id && marker.index == index, "custom frame scene/stream association differs");
            same_snapshot(state, plain);
            same_snapshot(state, session.snapshot_at_frame(marker.stream_id, root_id, index));
            for (const auto& row : rows) { verify_row(state, row, child_id); verify_row(plain, row, child_id); ++comparisons; }
        };
        for (const auto& frame : frames) frame_check(frame.first, frame.second);
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) frame_check(it->first, it->second);
        require(session.latest().instances.empty() && baseline.latest().instances.empty(), "custom instance cleanup was not replayed");
        const pvd::ObjectLifetime* child_lifetime = nullptr;
        const pvd::ObjectLifetime* root_lifetime = nullptr;
        for (const auto& life : session.lifetimes()) {
            if (life.id == child_id) child_lifetime = &life;
            if (life.id == root_id) root_lifetime = &life;
        }
        require(child_lifetime && root_lifetime && child_lifetime->destroyed_event && root_lifetime->destroyed_event, "custom lifetimes missing destroy boundaries");
        require(*child_lifetime->destroyed_event < session.frames()[9].begin_event_count, "child was not destroyed before frame 10");
        require(*root_lifetime->destroyed_event >= session.frames().back().event_count, "root was destroyed before final frame");
        std::cout << "PhysX 3.4 custom verified 12 frames, " << comparisons << " forward/reverse object states, "
                  << session.checkpoint_count() << " checkpoints, raw replay equivalence, strings, refs, arrays, chunked values and messages; " << session.events().size() << " events\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PhysX custom integration: " << error.what() << '\n'; return 1;
    }
}
