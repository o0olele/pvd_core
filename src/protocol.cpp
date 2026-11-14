#include "pvd/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>

namespace pvd {
namespace {

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size, std::uint64_t base,
           ByteOrder order, const DecodeLimits& limits)
        : data_(data), size_(size), base_(base), order_(order), limits_(limits) {}
    std::size_t remaining() const noexcept { return size_ - position_; }
    std::uint64_t offset() const noexcept { return base_ + position_; }
    void require(std::size_t count) const {
        if (count > remaining()) throw DecodeError("truncated field inside event group", offset());
    }
    std::uint8_t u8() { require(1); return data_[position_++]; }
    std::uint16_t u16() { return integer<std::uint16_t>(); }
    std::uint32_t u32() { return integer<std::uint32_t>(); }
    std::uint64_t u64() { return integer<std::uint64_t>(); }
    bool boolean() { return u8() != 0; } // SDK bool decoding accepts any nonzero byte.
    float f32() {
        const auto bits = u32();
        float value;
        static_assert(sizeof(value) == sizeof(bits), "32-bit IEEE float required");
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    WireName name() { return {u32(), u32()}; }
    Vec3 vec3() { return {f32(), f32(), f32()}; }
    Color color() { return {u8(), u8(), u8(), u8()}; }
    std::string string() {
        const auto length_offset = offset();
        const auto count = u32();
        if (count > limits_.max_string_bytes) throw DecodeError("string length exceeds limit", length_offset);
        require(count);
        if (count == 0) return {};
        if (data_[position_ + count - 1] != 0)
            throw DecodeError("string is missing its NUL terminator", offset() + count - 1);
        const auto first = data_ + position_;
        const auto nul = std::find(first, first + count - 1, std::uint8_t{0});
        if (nul != first + count - 1)
            throw DecodeError("string contains an embedded NUL", offset() + static_cast<std::uint64_t>(nul - first));
        std::string result(reinterpret_cast<const char*>(first), count - 1);
        position_ += count;
        return result;
    }
    Bytes bytes() {
        const auto length_offset = offset();
        const auto count = u32();
        if (count > limits_.max_blob_bytes) throw DecodeError("blob length exceeds limit", length_offset);
        require(count);
        Bytes result(data_ + position_, data_ + position_ + count);
        position_ += count;
        return result;
    }
    std::uint32_t array_count(std::size_t stride) {
        const auto count_offset = offset();
        const auto count = u32();
        if (count > limits_.max_array_entries) throw DecodeError("array count exceeds limit", count_offset);
        if (count > remaining() / stride) throw DecodeError("array exceeds event group bounds", count_offset);
        return count;
    }
private:
    template<class T> T integer() {
        require(sizeof(T));
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            const auto index = order_ == ByteOrder::little ? i : sizeof(T) - i - 1;
            value = static_cast<T>(value | (static_cast<T>(data_[position_ + index]) << (i * 8)));
        }
        position_ += sizeof(T);
        return value;
    }
    const std::uint8_t* data_;
    std::size_t size_, position_ = 0;
    std::uint64_t base_;
    ByteOrder order_;
    const DecodeLimits& limits_;
};

Payload payload(Reader& r) {
    const auto tag_offset = r.offset();
    const auto tag = r.u8();
    switch (tag) {
    case 1: return StringHandleEvent{r.string(), r.u32()};
    case 2: return CreateClass{r.name()};
    case 3: return DeriveClass{r.name(), r.name()};
    case 4: {
        CreateProperty event;
        event.class_name = r.name(); event.name = r.u32(); event.semantic = r.u32();
        event.datatype_name = r.name(); event.property_type = r.u8();
        const auto count = r.array_count(8);
        event.values.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) event.values.push_back({r.u32(), r.u32()});
        return event;
    }
    case 5: {
        CreatePropertyMessage event;
        event.class_name = r.name(); event.message_name = r.name();
        const auto count = r.array_count(20);
        event.message_entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            event.message_entries.push_back({r.u32(), r.name(), r.u32(), r.u32()});
        event.message_byte_size = r.u32();
        return event;
    }
    case 6: return CreateInstance{r.name(), r.u64()};
    case 7: {
        SetPropertyValue event;
        event.instance_id = r.u64(); event.property_name = r.u32();
        event.incoming_type_name = r.name(); event.num_items = r.u32(); event.data = r.bytes();
        return event;
    }
    case 8: return BeginSetPropertyValue{r.u64(), r.u32(), r.name()};
    case 9: return AppendPropertyValueData{r.bytes(), r.u32()};
    case 10: return EndSetPropertyValue{};
    case 11: return SetPropertyMessage{r.u64(), r.name(), r.bytes()};
    case 12: return BeginPropertyMessageGroup{r.name()};
    case 13: return SendPropertyMessageFromGroup{r.u64(), r.bytes()};
    case 14: return EndPropertyMessageGroup{};
    case 15: return DestroyInstance{r.u64()};
    case 16: return PushBackObjectRef{r.u64(), r.u32(), r.u64()};
    case 17: return RemoveObjectRef{r.u64(), r.u32(), r.u64()};
    case 18: return BeginSection{r.u64(), r.u32(), r.u64()};
    case 19: return EndSection{r.u64(), r.u32(), r.u64()};
    case 20: return SetPickable{r.u64(), r.boolean()};
    case 21: return SetColor{r.u64(), r.color()};
    case 22: return SetIsTopLevel{r.u64(), r.boolean()};
    case 23: return SetCamera{r.string(), r.vec3(), r.vec3(), r.vec3()};
    case 24: return AddProfileZone{r.u64(), r.string()};
    case 25: return AddProfileZoneEvent{r.u64(), r.string(), r.u16(), r.boolean()};
    case 26: return StreamEndEvent{r.string()};
    case 27: return ErrorMessage{r.u32(), r.string(), r.string(), r.u32()};
    case 28: return OriginShift{r.u64(), r.vec3()};
    default: throw DecodeError("unsupported event tag " + std::to_string(tag) +
                               "; events have no independent length and cannot be skipped", tag_offset);
    }
}
} // namespace

StreamDecoder::StreamDecoder(DecodeLimits limits) : limits_(limits) {
    if (limits_.max_group_bytes > std::numeric_limits<std::size_t>::max() - 28)
        throw std::invalid_argument("max_group_bytes is too large");
}

void StreamDecoder::feed(const std::uint8_t* bytes, std::size_t size) {
    if (failed_) throw DecodeError("decoder is in failed state", bytes_received_);
    if (finished_) throw DecodeError("cannot feed after finish", bytes_received_);
    if (size != 0 && bytes == nullptr) throw std::invalid_argument("null input with nonzero size");
    // Bound pending bytes while still accepting arbitrarily large batches of small groups.
    const auto capacity = std::max<std::size_t>(28, limits_.max_group_bytes + 24);
    try {
        while (size != 0) {
            const auto amount = std::min(size, capacity - buffer_.size());
            if (amount == 0) throw DecodeError("pending group exceeds buffer limit", buffer_offset_);
            if (amount > std::numeric_limits<std::uint64_t>::max() - bytes_received_)
                throw DecodeError("stream offset overflow", bytes_received_);
            buffer_.insert(buffer_.end(), bytes, bytes + amount);
            bytes_received_ += amount;
            bytes += amount; size -= amount;
            process();
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void StreamDecoder::process() {
    std::size_t consumed = 0;
    if (!header_) {
        if (buffer_.size() < 4) return;
        Reader magic_reader(buffer_.data(), buffer_.size(), buffer_offset_, ByteOrder::little, limits_);
        const auto magic = magic_reader.u32();
        ByteOrder order;
        if (magic == stream_magic) order = ByteOrder::little;
        else if (magic == 0x82e8e531u) order = ByteOrder::big;
        else if (magic == legacy_container_magic || magic == 0xb01b1036u)
            throw DecodeError("unsupported legacy PVD saved container; expected an SDK raw stream", buffer_offset_);
        else throw DecodeError("invalid PVD stream magic", buffer_offset_);
        if (buffer_.size() < 28) return;
        Reader r(buffer_.data() + 4, 24, buffer_offset_ + 4, order, limits_);
        StreamHeader header;
        header.byte_order = order; header.stream_version = r.u32();
        header.timestamp_numerator = r.u64(); header.timestamp_denominator = r.u64();
        header.stream_flags = r.u32();
        if (header.stream_version != supported_stream_version)
            throw DecodeError("unsupported PVD stream version " + std::to_string(header.stream_version), buffer_offset_ + 4);
        if (header.timestamp_denominator == 0)
            throw DecodeError("zero timestamp denominator", buffer_offset_ + 16);
        if ((header.stream_flags & ~1u) != 0)
            throw DecodeError("unsupported PVD stream flags", buffer_offset_ + 24);
        header_ = header; consumed = 28;
        committed_bytes_ = buffer_offset_ + consumed;
    }
    while (buffer_.size() - consumed >= 24) {
        const auto group_offset = buffer_offset_ + consumed;
        Reader r(buffer_.data() + consumed, buffer_.size() - consumed,
                 group_offset, header_->byte_order, limits_);
        const GroupHeader group{r.u32(), r.u32(), r.u64(), r.u64()};
        if (group.data_size > limits_.max_group_bytes)
            throw DecodeError("event group byte size exceeds limit", group_offset);
        if (group.num_events > limits_.max_events_per_group)
            throw DecodeError("event group count exceeds limit", group_offset + 4);
        if (decoded_count_ > limits_.max_total_events ||
            group.num_events > limits_.max_total_events - decoded_count_)
            throw DecodeError("total event count exceeds limit", group_offset + 4);
        if (group.num_events > group.data_size)
            throw DecodeError("event count cannot fit in group byte size", group_offset + 4);
        const auto group_size = std::size_t{24} + group.data_size;
        if (buffer_.size() - consumed < group_size) break;
        Reader body(buffer_.data() + consumed + 24, group.data_size,
                    group_offset + 24, header_->byte_order, limits_);
        std::vector<Event> staged;
        staged.reserve(group.num_events);
        for (std::uint32_t i = 0; i < group.num_events; ++i) {
            const auto event_offset = body.offset();
            auto value = payload(body);
            staged.push_back({group.stream_id, group.timestamp, event_offset, std::move(value)});
        }
        if (body.remaining() != 0)
            throw DecodeError("event group has trailing bytes after declared event count", body.offset());
        // Commit only a completely validated group, including its final byte/count agreement.
        static_assert(std::is_nothrow_move_constructible<Event>::value,
                      "Event movement must not break group commit atomicity");
        group_boundaries_.push_back({group_offset, group_offset + group_size,
                                     decoded_count_ + group.num_events});
        try {
            events_.insert(events_.end(), std::make_move_iterator(staged.begin()), std::make_move_iterator(staged.end()));
        } catch (...) {
            group_boundaries_.pop_back();
            throw;
        }
        decoded_count_ += group.num_events;
        ++group_count_; consumed += group_size;
        // Advance independently of buffer compaction: a later group may throw.
        committed_bytes_ = buffer_offset_ + consumed;
    }
    if (consumed != 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        buffer_offset_ += consumed;
    }
}

std::vector<Event> StreamDecoder::take_events() {
    std::vector<Event> result;
    result.swap(events_);
    return result;
}

std::vector<GroupBoundary> StreamDecoder::take_group_boundaries() {
    std::vector<GroupBoundary> result;
    result.swap(group_boundaries_);
    return result;
}

void StreamDecoder::finish() {
    if (failed_) throw DecodeError("decoder is in failed state", bytes_received_);
    if (finished_) return;
    try {
        process();
        if (!header_) throw DecodeError("truncated PVD stream header", bytes_received_);
        if (!buffer_.empty()) throw DecodeError(buffer_.size() < 24 ? "truncated event group header" : "truncated event group payload", bytes_received_);
        finished_ = true;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

} // namespace pvd
