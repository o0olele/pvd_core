#include <pvd/session.hpp>

#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {
std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    return out.str() + '"';
}
std::uint64_t number(const std::string& text) {
    if (text.empty() || text[0] == '-') throw std::runtime_error("expected a nonnegative integer");
    std::size_t used = 0;
    const auto value = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error("invalid integer: " + text);
    return value;
}
void value_json(const pvd::PropertyValue& value) {
    std::cout << "{\"type\":" << quoted(value.incoming_type.str())
              << ",\"items\":" << value.item_count << ",\"bytes\":" << value.raw_bytes().size()
              << ",\"opaque\":" << (value.opaque ? "true" : "false");
    if (auto text = value.as_string()) std::cout << ",\"text\":" << quoted(*text);
    std::ostringstream hex;
    const auto& bytes = value.raw_bytes();
    for (std::size_t i = 0; i < bytes.size() && i < 64; ++i)
        hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    std::cout << ",\"wire_hex_prefix\":" << quoted(hex.str()) << '}';
}
void object_json(const pvd::InstanceState& instance, bool properties) {
    // IDs are strings so JSON/JavaScript consumers cannot lose 64-bit precision.
    std::cout << "{\"id\":" << quoted(std::to_string(instance.id))
              << ",\"generation\":" << instance.generation
              << ",\"class\":" << quoted(instance.class_name.str());
    if (properties) {
        std::cout << ",\"properties\":{";
        bool first = true;
        for (const auto& item : instance.properties) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << quoted(item.first) << ':';
            value_json(item.second);
        }
        std::cout << '}';
    }
    std::cout << '}';
}
void usage() {
    std::cout << "usage: pvd_inspect <SDK-raw-capture.pxd2> [--frame N] [--instance ID]\n"
                 "                   [--objects] [--properties] [--list-frames] [--latest]\n"
                 "Frames are zero-based global frame markers. Default: last complete frame.\n"
                 "Without frame markers, inspect the state at EOF. Output is JSON.\n";
}
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        usage();
        return argc < 2 ? 2 : 0;
    }
    try {
        std::optional<std::size_t> frame;
        std::optional<std::uint64_t> instance_id;
        bool objects = false, properties = false, list_frames = false, latest = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--frame" && i + 1 < argc) frame = static_cast<std::size_t>(number(argv[++i]));
            else if (arg == "--instance" && i + 1 < argc) instance_id = number(argv[++i]);
            else if (arg == "--objects") objects = true;
            else if (arg == "--properties") properties = objects = true;
            else if (arg == "--list-frames") list_frames = true;
            else if (arg == "--latest") latest = true;
            else throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        auto session = pvd::Session::load_file(argv[1]);
        if (latest && frame) throw std::runtime_error("--frame and --latest are mutually exclusive");
        if (!latest && !frame && !session.frames().empty()) frame = session.frames().size() - 1;
        const auto state = frame ? session.snapshot_at_frame(*frame) : session.latest();
        std::cout << "{\n  \"protocol_version\":" << session.header().stream_version
                  << ",\n  \"byte_order\":" << quoted(session.header().byte_order == pvd::ByteOrder::little ? "little" : "big")
                  << ",\n  \"pointer_width\":" << unsigned(session.header().pointer_width())
                  << ",\n  \"events\":" << session.events().size()
                  << ",\n  \"classes\":" << session.metadata().classes.size()
                  << ",\n  \"frames\":" << session.frames().size()
                  << ",\n  \"lifetimes\":" << session.lifetimes().size()
                  << ",\n  \"channel_packets\":" << session.channels().size()
                  << ",\n  \"checkpoints\":" << session.checkpoint_count()
                  << ",\n  \"live_instances\":" << state.instances.size()
                  << ",\n  \"event_count\":" << state.event_count;
        if (frame) std::cout << ",\n  \"selected_frame\":" << *frame;
        if (instance_id) {
            std::cout << ",\n  \"instance\":";
            if (const auto* instance = state.instance(*instance_id)) object_json(*instance, true);
            else std::cout << "null";
        } else if (objects) {
            std::cout << ",\n  \"instances\":[";
            bool first = true;
            for (const auto& item : state.instances) {
                if (!first) std::cout << ',';
                first = false;
                object_json(item.second, properties);
            }
            std::cout << ']';
        }
        if (list_frames) {
            std::cout << ",\n  \"frame_markers\":[";
            bool first = true;
            for (const auto& marker : session.frames()) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"stream_id\":" << quoted(std::to_string(marker.stream_id))
                          << ",\"scene_id\":" << quoted(std::to_string(marker.scene_id))
                          << ",\"index\":" << marker.index
                          << ",\"event_count\":" << marker.event_count << '}';
            }
            std::cout << ']';
        }
        std::cout << "\n}\n";
    } catch (const std::exception& error) {
        std::cerr << "pvd_inspect: " << error.what() << '\n';
        return 1;
    }
}
