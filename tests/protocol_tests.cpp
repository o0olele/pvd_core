#include "pvd/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace pvd;
void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
struct Writer {
    ByteOrder order = ByteOrder::little;
    Bytes data;
    explicit Writer(ByteOrder byte_order = ByteOrder::little) : order(byte_order) {}
    void number(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            const auto shift = (order == ByteOrder::little ? i : width - i - 1) * 8;
            data.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void u8(std::uint8_t x) { number(x, 1); }
    void u16(std::uint16_t x) { number(x, 2); }
    void u32(std::uint32_t x) { number(x, 4); }
    void u64(std::uint64_t x) { number(x, 8); }
    void f32(float x) { std::uint32_t bits; std::memcpy(&bits, &x, 4); u32(bits); }
    void vec(float x, float y, float z) { f32(x); f32(y); f32(z); }
    void name(std::uint32_t ns = 11, std::uint32_t nm = 12) { u32(ns); u32(nm); }
    void string(const std::string& x) {
        u32(x.empty() ? 0u : static_cast<std::uint32_t>(x.size() + 1));
        data.insert(data.end(), x.begin(), x.end());
        if (!x.empty()) u8(0);
    }
    void blob() { u32(4); u8(0x12); u8(0x34); u8(0x56); u8(0x78); }
    void append(const Bytes& x) { data.insert(data.end(), x.begin(), x.end()); }
};
constexpr std::uint64_t iid = 0x8877665544332211ull;
Writer header(ByteOrder order = ByteOrder::little) {
    Writer w(order); w.u32(stream_magic); w.u32(1); w.u64(100); w.u64(3); w.u32(1); return w;
}
Bytes group(const Writer& body, std::uint32_t count, std::uint64_t stream = 0x1122334455667788ull,
            std::uint64_t timestamp = 0xffeeddccbbaa9988ull) {
    Writer w(body.order); w.u32(static_cast<std::uint32_t>(body.data.size())); w.u32(count);
    w.u64(stream); w.u64(timestamp); w.append(body.data); return w.data;
}
Writer all_events(ByteOrder order) {
    Writer w(order);
    w.u8(1); w.string("RigidDynamic"); w.u32(12);
    w.u8(2); w.name();
    w.u8(3); w.name(1, 2); w.name(3, 4);
    w.u8(4); w.name(); w.u32(20); w.u32(21); w.name(22, 23); w.u8(2);
        w.u32(2); w.u32(24); w.u32(0xffffffffu); w.u32(25); w.u32(123);
    w.u8(5); w.name(); w.name(30, 31); w.u32(2);
        w.u32(32); w.name(33, 34); w.u32(4); w.u32(8);
        w.u32(35); w.name(36, 37); w.u32(12); w.u32(16); w.u32(28);
    w.u8(6); w.name(); w.u64(iid);
    w.u8(7); w.u64(iid); w.u32(40); w.name(41, 42); w.u32(5); w.blob();
    w.u8(8); w.u64(iid); w.u32(43); w.name(44, 45);
    w.u8(9); w.blob(); w.u32(6);
    w.u8(10);
    w.u8(11); w.u64(iid); w.name(46, 47); w.blob();
    w.u8(12); w.name(48, 49);
    w.u8(13); w.u64(iid); w.blob();
    w.u8(14);
    w.u8(15); w.u64(iid);
    w.u8(16); w.u64(iid); w.u32(50); w.u64(51);
    w.u8(17); w.u64(iid); w.u32(52); w.u64(53);
    w.u8(18); w.u64(54); w.u32(55); w.u64(56);
    w.u8(19); w.u64(57); w.u32(58); w.u64(59);
    w.u8(20); w.u64(iid); w.u8(7); // Match SDK nonzero bool semantics.
    w.u8(21); w.u64(iid); w.u8(60); w.u8(61); w.u8(62); w.u8(63);
    w.u8(22); w.u64(iid); w.u8(0);
    w.u8(23); w.string("camera"); w.vec(1.25f, -2.5f, 3.75f); w.vec(0, 1, 0); w.vec(-1, -2, -3);
    w.u8(24); w.u64(iid); w.string("zone");
    w.u8(25); w.u64(iid); w.string("simulate"); w.u16(0xabcd); w.u8(1);
    w.u8(26); w.string("StreamEnd");
    w.u8(27); w.u32(64); w.string("message"); w.string("file.cpp"); w.u32(65);
    w.u8(28); w.u64(iid); w.vec(100, -200, 300);
    return w;
}
Bytes capture(const Writer& body, std::uint32_t count) {
    auto w = header(body.order); w.append(group(body, count)); return w.data;
}
template<class F> void error_at(F&& fn, std::uint64_t offset, const char* contains = "") {
    try { fn(); }
    catch (const DecodeError& error) {
        check(error.offset() == offset, "wrong error offset");
        check(std::string(error.what()).find(contains) != std::string::npos, "wrong error reason");
        return;
    }
    throw std::runtime_error("expected DecodeError");
}
void verify_all(const StreamDecoder& decoder, ByteOrder order) {
    check(decoder.header().has_value(), "missing header");
    const auto& h = *decoder.header();
    check(h.byte_order == order && h.pointer_width() == 8, "header order/pointer width");
    check(h.timestamp_numerator == 100 && h.timestamp_denominator == 3, "timestamp conversion header");
    const auto& e = decoder.events();
    check(e.size() == 28 && decoder.group_count() == 1, "all 28 events committed");
    for (std::size_t i = 0; i < e.size(); ++i) {
        check(e[i].tag() == i + 1, "event tag mapping");
        check(e[i].stream_id == 0x1122334455667788ull && e[i].timestamp == 0xffeeddccbbaa9988ull, "event group metadata");
        check(i == 0 ? e[i].byte_offset == 52 : e[i].byte_offset > e[i-1].byte_offset, "absolute event offsets");
    }
    check(std::get<StringHandleEvent>(e[0].payload).string == "RigidDynamic" && std::get<StringHandleEvent>(e[0].payload).handle == 12, "string fields");
    check(std::get<CreateClass>(e[1].payload).name.namespace_handle == 11, "class name");
    check(std::get<DeriveClass>(e[2].payload).child.name_handle == 4, "derive order");
    const auto& prop = std::get<CreateProperty>(e[3].payload);
    check(prop.property_type == 2 && prop.values.size() == 2 && prop.values[0].value == 0xffffffffu && prop.values[1].name == 25, "property array fields");
    const auto& msg = std::get<CreatePropertyMessage>(e[4].payload);
    check(msg.message_entries.size() == 2 && msg.message_entries[1].datatype_name.name_handle == 37 && msg.message_entries[0].message_offset == 4 && msg.message_byte_size == 28, "property message entries");
    check(std::get<CreateInstance>(e[5].payload).instance_id == iid, "64 bit instance ID");
    const auto& value = std::get<SetPropertyValue>(e[6].payload);
    check(value.property_name == 40 && value.num_items == 5 && value.incoming_type_name.name_handle == 42 && value.data == Bytes({0x12,0x34,0x56,0x78}), "property bytes not swapped");
    check(std::get<BeginSetPropertyValue>(e[7].payload).property_name == 43, "begin property");
    check(std::get<AppendPropertyValueData>(e[8].payload).num_items == 6, "append data then count order");
    check(std::get<SetPropertyMessage>(e[10].payload).message_name.name_handle == 47, "set property message");
    check(std::get<BeginPropertyMessageGroup>(e[11].payload).msg_name.namespace_handle == 48, "begin message group");
    check(std::get<SendPropertyMessageFromGroup>(e[12].payload).instance == iid, "send message instance");
    check(std::get<DestroyInstance>(e[14].payload).instance_id == iid, "destroy instance");
    check(std::get<PushBackObjectRef>(e[15].payload).object_ref == 51 && std::get<RemoveObjectRef>(e[16].payload).property == 52, "object refs");
    check(std::get<BeginSection>(e[17].payload).timestamp == 56 && std::get<EndSection>(e[18].payload).section_id == 57, "section fields");
    check(std::get<SetPickable>(e[19].payload).pickable && std::get<SetColor>(e[20].payload).color.a == 63 && !std::get<SetIsTopLevel>(e[21].payload).is_top_level, "bool/color fields");
    const auto& camera = std::get<SetCamera>(e[22].payload);
    check(camera.name == "camera" && camera.position.y == -2.5f && camera.target.z == -3, "camera floats");
    check(std::get<AddProfileZone>(e[23].payload).name == "zone", "profile zone");
    check(std::get<AddProfileZoneEvent>(e[24].payload).event_id == 0xabcd && std::get<AddProfileZoneEvent>(e[24].payload).compile_time_enabled, "profile event u16");
    check(std::get<StreamEndEvent>(e[25].payload).name == "StreamEnd", "end event string");
    check(std::get<ErrorMessage>(e[26].payload).line == 65 && std::get<ErrorMessage>(e[26].payload).file == "file.cpp", "error message strings");
    check(std::get<OriginShift>(e[27].payload).shift.y == -200, "origin shift");
}
void all_and_fragmented() {
    for (auto order : {ByteOrder::little, ByteOrder::big}) {
        const auto data = capture(all_events(order), 28);
        for (std::size_t split = 0; split <= data.size(); ++split) {
            StreamDecoder decoder;
            decoder.feed(data.data(), split);
            if (split < data.size()) check(decoder.events().empty(), "incomplete group must not commit events");
            decoder.feed(data.data() + split, data.size() - split); decoder.finish(); verify_all(decoder, order);
        }
        StreamDecoder single_byte;
        for (const auto& byte : data) single_byte.feed(&byte, 1);
        single_byte.finish(); verify_all(single_byte, order);
        check(single_byte.bytes_received() == data.size(), "received byte count");
        single_byte.finish(); // Idempotent EOF.
        error_at([&] { single_byte.feed(nullptr, 0); }, data.size(), "after finish");
    }
}
void malformed_and_limits() {
    Writer good; good.u8(10);
    const auto data = capture(good, 1);
    for (std::size_t n = 0; n < data.size(); ++n) {
        if (n == 28) continue; // A complete header with no event groups is a valid empty capture.
        StreamDecoder decoder; decoder.feed(data.data(), n);
        error_at([&] { decoder.finish(); }, n, "truncated");
        check(decoder.events().empty(), "truncated group atomicity");
    }
    auto bad = header(); bad.data[0] = 0;
    error_at([&] { StreamDecoder decoder; decoder.feed(bad.data); }, 0, "magic");
    for (auto order : {ByteOrder::little, ByteOrder::big}) {
        Writer legacy(order); legacy.u32(legacy_container_magic);
        error_at([&] { StreamDecoder d; d.feed(legacy.data); }, 0, "legacy");
        auto unsupported = header(order); unsupported.data[order == ByteOrder::little ? 4 : 7] = 2;
        error_at([&] { StreamDecoder d; d.feed(unsupported.data); }, 4, "version");
    }
    auto denom = header(); std::fill(denom.data.begin() + 16, denom.data.begin() + 24, 0);
    error_at([&] { StreamDecoder d; d.feed(denom.data); }, 16, "denominator");
    auto flags = header(); flags.data[24] = 2;
    error_at([&] { StreamDecoder d; d.feed(flags.data); }, 24, "flags");
    for (auto tag : {0, 29, 255}) {
        Writer body; body.u8(static_cast<std::uint8_t>(tag));
        error_at([&] { StreamDecoder d; d.feed(capture(body, 1)); }, 52, "cannot be skipped");
    }
    Writer two; two.u8(10); two.u8(255);
    StreamDecoder atomic;
    auto prefix = header(); prefix.append(group(good, 1)); atomic.feed(prefix.data);
    error_at([&] { atomic.feed(group(two, 2)); }, 78, "unsupported event");
    check(atomic.events().size() == 1 && atomic.group_count() == 1, "retain prior groups, reject whole failing group");
    error_at([&] { atomic.feed(good.data); }, atomic.bytes_received(), "failed state");
    error_at([&] { StreamDecoder d; d.feed(capture(two, 1)); }, 53, "trailing bytes");
    error_at([&] { StreamDecoder d; d.feed(capture(good, 2)); }, 32, "cannot fit");
    Writer missing; missing.u8(6); missing.u64(0); // Needs two u32 handles and then u64 ID.
    error_at([&] { StreamDecoder d; d.feed(capture(missing, 1)); }, 61, "truncated field");
    Writer unterminated; unterminated.u8(1); unterminated.u32(2); unterminated.u8('a'); unterminated.u8('b'); unterminated.u32(5);
    error_at([&] { StreamDecoder d; d.feed(capture(unterminated, 1)); }, 58, "NUL terminator");
    Writer embedded; embedded.u8(1); embedded.u32(3); embedded.u8(0); embedded.u8('a'); embedded.u8(0); embedded.u32(5);
    error_at([&] { StreamDecoder d; d.feed(capture(embedded, 1)); }, 57, "embedded NUL");
    Writer giant; giant.u8(1); giant.u32(0xffffffffu);
    error_at([&] { StreamDecoder d; d.feed(capture(giant, 1)); }, 53, "string length");
    Writer array; array.u8(4); array.name(); array.u32(0); array.u32(0); array.name(); array.u8(1); array.u32(0xffffffffu);
    error_at([&] { StreamDecoder d; d.feed(capture(array, 1)); }, 78, "array count");
    DecodeLimits limit; limit.max_group_bytes = 0;
    error_at([&] { StreamDecoder d(limit); d.feed(data); }, 28, "byte size");
    limit = {}; limit.max_events_per_group = 0;
    error_at([&] { StreamDecoder d(limit); d.feed(data); }, 32, "group count");
    limit = {}; limit.max_total_events = 1;
    auto twice = header(); twice.append(group(good, 1)); twice.append(group(good, 1));
    error_at([&] { StreamDecoder d(limit); d.feed(twice.data); }, 57, "total event");
    Writer blob; blob.u8(13); blob.u64(1); blob.blob();
    limit = {}; limit.max_blob_bytes = 3;
    error_at([&] { StreamDecoder d(limit); d.feed(capture(blob, 1)); }, 61, "blob length");
}
void bounded_batches() {
    Writer body; body.u8(10);
    auto input = header();
    for (std::uint64_t i = 0; i < 200; ++i) input.append(group(body, 1, i, i * 2));
    DecodeLimits limits; limits.max_group_bytes = 1;
    StreamDecoder decoder(limits); decoder.feed(input.data); decoder.finish();
    check(decoder.events().size() == 200 && decoder.group_count() == 200, "large input batches with tiny pending buffer");
    check(decoder.events().back().stream_id == 199 && decoder.events().back().byte_offset == 52 + 199 * 25, "cross-batch offsets and stream IDs");
    auto empty = header(); Writer no_events; empty.append(group(no_events, 0));
    StreamDecoder zero; zero.feed(empty.data); zero.finish(); check(zero.events().empty() && zero.group_count() == 1, "empty group");
    auto p32 = header(); p32.data[24] = 0; StreamDecoder narrow; narrow.feed(p32.data); narrow.finish();
    check(narrow.header()->pointer_width() == 4, "32-bit pointer metadata");
    StreamDecoder invalid; bool threw = false;
    try { invalid.feed(nullptr, 1); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "null nonempty input rejected");
}

void drain_and_committed_prefix() {
    Writer one; one.u8(10);
    Writer two; two.u8(10); two.u8(14);
    const auto first_group = group(one, 1, 101, 201);
    const auto second_group = group(two, 2, 102, 202);
    const auto start = header().data;
    StreamDecoder decoder;
    check(decoder.committed_bytes() == 0 && decoder.event_count() == 0,
          "new decoder has no committed bytes or events");
    check(decoder.take_events().empty() && decoder.take_group_boundaries().empty(), "empty drains");
    decoder.feed(start.data(), 27);
    check(decoder.committed_bytes() == 0 && !decoder.header(), "partial header is not committed");
    decoder.feed(start.data() + 27, 1);
    check(decoder.committed_bytes() == 28 && decoder.header(), "validated header commits independently");
    check(decoder.take_group_boundaries().empty(), "stream header is not a group boundary");
    decoder.feed(first_group.data(), first_group.size() - 1);
    check(decoder.committed_bytes() == 28 && decoder.event_count() == 0 && decoder.events().empty(),
          "incomplete group must not advance committed bytes or events");
    check(decoder.take_group_boundaries().empty(), "partial group has no boundary");
    decoder.feed(first_group.data() + first_group.size() - 1, 1);
    check(decoder.committed_bytes() == 53 && decoder.event_count() == 1 && decoder.group_count() == 1,
          "complete first group updates cumulative counters");
    const auto* allocation = decoder.events().data();
    auto extracted = decoder.take_events();
    check(extracted.size() == 1 && extracted.data() == allocation && extracted[0].byte_offset == 52,
          "take_events transfers storage without copying events");
    check(decoder.events().empty() && decoder.take_events().empty(), "events are removed exactly once");
    auto boundaries = decoder.take_group_boundaries();
    check(boundaries.size() == 1 && boundaries[0].byte_offset == 28 && boundaries[0].end_offset == 53 && boundaries[0].event_count == 1,
          "first complete group boundary");
    check(decoder.take_group_boundaries().empty(), "group boundaries drain exactly once");
    check(decoder.header() && decoder.event_count() == 1 && decoder.group_count() == 1 && decoder.committed_bytes() == 53,
          "drain preserves header and cumulative metadata");
    decoder.feed(second_group.data(), second_group.size() - 1);
    check(decoder.events().empty() && decoder.event_count() == 1 && decoder.committed_bytes() == 53,
          "new incomplete group remains invisible after a drain");
    decoder.feed(second_group.data() + second_group.size() - 1, 1);
    extracted = decoder.take_events();
    check(extracted.size() == 2 && extracted[0].stream_id == 102 && extracted[0].byte_offset == 77 && extracted[1].byte_offset == 78,
          "later drain contains only new complete group events with absolute offsets");
    boundaries = decoder.take_group_boundaries();
    check(boundaries.size() == 1 && boundaries[0].byte_offset == 53 && boundaries[0].end_offset == 79 && boundaries[0].event_count == 3,
          "later boundary retains cumulative rather than undrained event count");
    decoder.finish();
    check(decoder.committed_bytes() == 79 && decoder.bytes_received() == 79 && decoder.event_count() == 3 && decoder.group_count() == 2,
          "EOF and draining preserve cumulative counters");
    check(decoder.take_events().empty() && decoder.take_group_boundaries().empty(), "drains remain usable after finish");

    // A single feed validates two good groups, then fails in the middle of a third.
    Writer bad; bad.u8(10); bad.u8(255);
    auto mixed = header(); mixed.append(first_group); mixed.append(second_group); mixed.append(group(bad, 2));
    StreamDecoder failed;
    error_at([&] { failed.feed(mixed.data); }, 104, "unsupported event");
    check(failed.header() && failed.committed_bytes() == 79 && failed.event_count() == 3 && failed.group_count() == 2,
          "same-feed failure preserves precisely the previously validated prefix");
    extracted = failed.take_events(); boundaries = failed.take_group_boundaries();
    check(extracted.size() == 3 && boundaries.size() == 2 && boundaries.back().end_offset == 79 && boundaries.back().event_count == 3,
          "bad group contributes neither staged events nor a group boundary");
    check(failed.take_events().empty() && failed.take_group_boundaries().empty(), "failed decoder drains exactly once");
    Bytes recovered_bytes(mixed.data.begin(), mixed.data.begin() + static_cast<std::ptrdiff_t>(failed.committed_bytes()));
    StreamDecoder recovered; recovered.feed(recovered_bytes); recovered.finish();
    check(recovered.events().size() == extracted.size() && recovered.event_count() == 3 && recovered.committed_bytes() == 79,
          "the reported committed prefix is independently decodable after recovery");
    for (std::size_t i = 0; i < extracted.size(); ++i)
        check(recovered.events()[i].byte_offset == extracted[i].byte_offset && recovered.events()[i].tag() == extracted[i].tag(),
              "recovered events match the successful prefix");

    // A malformed first group still allows the validated header to be saved.
    auto header_bad = header(); header_bad.append(group(bad, 2));
    StreamDecoder header_only;
    error_at([&] { header_only.feed(header_bad.data); }, 53, "unsupported event");
    check(header_only.committed_bytes() == 28 && header_only.event_count() == 0 && header_only.take_events().empty() && header_only.take_group_boundaries().empty(),
          "header stays committed even when first group fails in same feed");
    auto invalid_header = header(); invalid_header.data[4] = 2;
    StreamDecoder invalid;
    error_at([&] { invalid.feed(invalid_header.data); }, 4, "version");
    check(invalid.committed_bytes() == 0 && !invalid.header(), "rejected header commits nothing");

    // EOF in either the next group header or its payload must not include the tail.
    for (const std::size_t trailing_size : {std::size_t{15}, second_group.size() - 1}) {
        auto truncated = header(); truncated.append(first_group);
        truncated.data.insert(truncated.data.end(), second_group.begin(), second_group.begin() + static_cast<std::ptrdiff_t>(trailing_size));
        StreamDecoder partial; partial.feed(truncated.data);
        check(partial.committed_bytes() == 53 && partial.event_count() == 1, "partial tail stays beyond committed prefix");
        error_at([&] { partial.finish(); }, truncated.data.size(), "truncated");
        check(partial.committed_bytes() == 53 && partial.take_events().size() == 1, "EOF error retains valid events and byte prefix");
        Bytes safe(truncated.data.begin(), truncated.data.begin() + static_cast<std::ptrdiff_t>(partial.committed_bytes()));
        StreamDecoder repair; repair.feed(safe); repair.finish();
        check(repair.event_count() == 1 && repair.committed_bytes() == 53, "truncated recording can be recovered by saved prefix");
    }

    // Draining must never reset the security limit's cumulative decoded count.
    DecodeLimits limits; limits.max_total_events = 2;
    StreamDecoder limited(limits); limited.feed(start);
    limited.feed(first_group); check(limited.take_events().size() == 1, "first limited event drained");
    limited.feed(first_group); check(limited.take_events().size() == 1, "second limited event drained");
    error_at([&] { limited.feed(first_group); }, 82, "total event count");
    check(limited.event_count() == 2 && limited.events().empty() && limited.committed_bytes() == 78 && limited.group_count() == 2,
          "take_events cannot bypass cumulative event limit");
    boundaries = limited.take_group_boundaries();
    check(boundaries.size() == 2 && boundaries[0].event_count == 1 && boundaries[1].event_count == 2,
          "event and boundary queues can be drained independently");

    Writer none; auto empty_group = header(); empty_group.append(group(none, 0));
    StreamDecoder empty; empty.feed(empty_group.data); empty.finish(); boundaries = empty.take_group_boundaries();
    check(empty.event_count() == 0 && empty.group_count() == 1 && empty.committed_bytes() == 52 && boundaries.size() == 1 && boundaries[0].event_count == 0,
          "zero-event complete group still advances committed bytes and emits boundary");

    StreamDecoder values; values.feed(capture(all_events(ByteOrder::big), 28)); values.finish();
    const auto* blob_allocation = std::get<SetPropertyValue>(values.events()[6].payload).data.data();
    extracted = values.take_events();
    check(std::get<SetPropertyValue>(extracted[6].payload).data.data() == blob_allocation && values.event_count() == 28,
          "extracting typed events does not duplicate payload buffers or reset count");
}

} // namespace
int main() {
    try { all_and_fragmented(); malformed_and_limits(); bounded_batches(); drain_and_committed_prefix();
        std::cout << "protocol tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "protocol tests: " << error.what() << '\n'; return 1; }
}
