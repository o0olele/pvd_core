#pragma once

#include "pvd/export.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pvd {

inline constexpr std::uint32_t stream_magic = 837150850u;
inline constexpr std::uint32_t legacy_container_magic = 0x36101bb0u;
inline constexpr std::uint32_t supported_stream_version = 1;
enum class ByteOrder { little, big };

class DecodeError : public std::runtime_error {
public:
    DecodeError(std::string message, std::uint64_t offset)
        : std::runtime_error(std::move(message) + " at byte " + std::to_string(offset)), offset_(offset) {}
    std::uint64_t offset() const noexcept { return offset_; }
private:
    std::uint64_t offset_;
};

struct WireName { std::uint32_t namespace_handle = 0, name_handle = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Color { std::uint8_t r = 0, g = 0, b = 0, a = 255; };
using Bytes = std::vector<std::uint8_t>;

struct StreamHeader {
    std::uint32_t stream_version = 1;
    std::uint64_t timestamp_numerator = 0, timestamp_denominator = 1;
    std::uint32_t stream_flags = 0;
    ByteOrder byte_order = ByteOrder::little;
    std::uint8_t pointer_width() const noexcept { return (stream_flags & 1u) ? 8 : 4; }
};
struct GroupHeader {
    std::uint32_t data_size = 0, num_events = 0;
    std::uint64_t stream_id = 0, timestamp = 0;
};

// Strings are SDK length-prefixed C strings: the final NUL is validated and removed.
// Property/message data remains opaque, in its original wire byte order.
struct StringHandleEvent { std::string string; std::uint32_t handle = 0; };
struct CreateClass { WireName name; };
struct DeriveClass { WireName parent, child; };
struct NameHandleValue { std::uint32_t name = 0, value = 0; };
struct CreateProperty {
    WireName class_name;
    std::uint32_t name = 0, semantic = 0;
    WireName datatype_name;
    std::uint8_t property_type = 0;
    std::vector<NameHandleValue> values;
};
struct StreamPropMessageArg {
    std::uint32_t property_name = 0;
    WireName datatype_name;
    std::uint32_t message_offset = 0, byte_size = 0;
};
struct CreatePropertyMessage {
    WireName class_name, message_name;
    std::vector<StreamPropMessageArg> message_entries;
    std::uint32_t message_byte_size = 0;
};
struct CreateInstance { WireName class_name; std::uint64_t instance_id = 0; };
struct SetPropertyValue {
    std::uint64_t instance_id = 0;
    std::uint32_t property_name = 0;
    WireName incoming_type_name;
    std::uint32_t num_items = 0;
    Bytes data;
};
struct BeginSetPropertyValue {
    std::uint64_t instance_id = 0;
    std::uint32_t property_name = 0;
    WireName incoming_type_name;
};
struct AppendPropertyValueData { Bytes data; std::uint32_t num_items = 0; };
struct EndSetPropertyValue {};
struct SetPropertyMessage { std::uint64_t instance_id = 0; WireName message_name; Bytes data; };
struct BeginPropertyMessageGroup { WireName msg_name; };
struct SendPropertyMessageFromGroup { std::uint64_t instance = 0; Bytes data; };
struct EndPropertyMessageGroup {};
struct DestroyInstance { std::uint64_t instance_id = 0; };
struct PushBackObjectRef { std::uint64_t instance_id = 0; std::uint32_t property = 0; std::uint64_t object_ref = 0; };
struct RemoveObjectRef { std::uint64_t instance_id = 0; std::uint32_t property = 0; std::uint64_t object_ref = 0; };
struct BeginSection { std::uint64_t section_id = 0; std::uint32_t name = 0; std::uint64_t timestamp = 0; };
struct EndSection { std::uint64_t section_id = 0; std::uint32_t name = 0; std::uint64_t timestamp = 0; };
struct SetPickable { std::uint64_t instance_id = 0; bool pickable = false; };
struct SetColor { std::uint64_t instance_id = 0; Color color; };
struct SetIsTopLevel { std::uint64_t instance_id = 0; bool is_top_level = false; };
struct SetCamera { std::string name; Vec3 position, up, target; };
struct AddProfileZone { std::uint64_t instance_id = 0; std::string name; };
struct AddProfileZoneEvent {
    std::uint64_t instance_id = 0;
    std::string name;
    std::uint16_t event_id = 0;
    bool compile_time_enabled = false;
};
struct StreamEndEvent { std::string name; };
struct ErrorMessage { std::uint32_t code = 0; std::string message, file; std::uint32_t line = 0; };
struct OriginShift { std::uint64_t instance_id = 0; Vec3 shift; };

// Alternative index + 1 is the PhysX 3.4 wire event tag (1..28).
using Payload = std::variant<StringHandleEvent, CreateClass, DeriveClass, CreateProperty,
    CreatePropertyMessage, CreateInstance, SetPropertyValue, BeginSetPropertyValue,
    AppendPropertyValueData, EndSetPropertyValue, SetPropertyMessage,
    BeginPropertyMessageGroup, SendPropertyMessageFromGroup, EndPropertyMessageGroup,
    DestroyInstance, PushBackObjectRef, RemoveObjectRef, BeginSection, EndSection,
    SetPickable, SetColor, SetIsTopLevel, SetCamera, AddProfileZone, AddProfileZoneEvent,
    StreamEndEvent, ErrorMessage, OriginShift>;
struct Event {
    std::uint64_t stream_id = 0, timestamp = 0, byte_offset = 0;
    Payload payload;
    std::uint8_t tag() const noexcept { return static_cast<std::uint8_t>(payload.index() + 1); }
};

struct GroupBoundary {
    std::uint64_t byte_offset = 0, end_offset = 0;
    std::size_t event_count = 0; // Cumulative count after this complete group.
};

struct DecodeLimits {
    std::size_t max_group_bytes = 64u * 1024u * 1024u;
    std::size_t max_blob_bytes = 64u * 1024u * 1024u;
    std::size_t max_string_bytes = 1024u * 1024u;
    std::size_t max_array_entries = 1024u * 1024u;
    std::size_t max_events_per_group = 1024u * 1024u;
    std::size_t max_total_events = 10u * 1024u * 1024u;
};

class StreamDecoder {
public:
    explicit PVD_CORE_API StreamDecoder(DecodeLimits limits = {});
    PVD_CORE_API void feed(const std::uint8_t* bytes, std::size_t size);
    void feed(const Bytes& bytes) { feed(bytes.data(), bytes.size()); }
    // EOF validates any buffered header/group. A StreamEndEvent is not required at EOF.
    PVD_CORE_API void finish();
    const std::optional<StreamHeader>& header() const noexcept { return header_; }
    // Only events not yet extracted by take_events() are retained here.
    const std::vector<Event>& events() const noexcept { return events_; }
    PVD_CORE_API std::vector<Event> take_events();
    PVD_CORE_API std::vector<GroupBoundary> take_group_boundaries();
    std::size_t event_count() const noexcept { return decoded_count_; }
    std::size_t group_count() const noexcept { return group_count_; }
    std::uint64_t bytes_received() const noexcept { return bytes_received_; }
    // Absolute prefix containing only the validated header and complete groups.
    // This remains usable after a feed/finish error to recover a raw capture.
    std::uint64_t committed_bytes() const noexcept { return committed_bytes_; }
private:
    void process();
    DecodeLimits limits_;
    std::optional<StreamHeader> header_;
    Bytes buffer_;
    std::vector<Event> events_;
    std::vector<GroupBoundary> group_boundaries_;
    std::uint64_t buffer_offset_ = 0, bytes_received_ = 0, committed_bytes_ = 0;
    std::size_t group_count_ = 0, decoded_count_ = 0;
    bool finished_ = false, failed_ = false;
};

} // namespace pvd
