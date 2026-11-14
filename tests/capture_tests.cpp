#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "pvd/capture.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace pvd;
using namespace std::chrono_literals;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket no_socket = INVALID_SOCKET;
void close_socket(Socket s) { if (s != no_socket) closesocket(s); }
struct Runtime { Runtime() { WSADATA data{}; check(!WSAStartup(MAKEWORD(2,2), &data), "test WSAStartup"); } ~Runtime() { WSACleanup(); } };
#else
using Socket = int;
constexpr Socket no_socket = -1;
void close_socket(Socket s) { if (s != no_socket) ::close(s); }
struct Runtime {};
#endif
struct Client {
    Socket socket = no_socket;
    explicit Client(std::uint16_t port) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        check(socket != no_socket, "test client socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            close_socket(socket); socket = no_socket;
            throw std::runtime_error("test client connect");
        }
        int yes = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef _WIN32
        DWORD timeout = 2000;
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{2,0};
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }
    ~Client() { close(); }
    void close() { close_socket(socket); socket = no_socket; }
    void reset() {
        linger immediate{1,0};
        setsockopt(socket,SOL_SOCKET,SO_LINGER,reinterpret_cast<const char*>(&immediate),sizeof(immediate));
        close();
    }
    bool send(const Bytes& bytes, std::size_t fragment = 0) {
        if (!fragment) fragment = bytes.size();
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = static_cast<int>(std::min(fragment, bytes.size() - done));
#if defined(MSG_NOSIGNAL)
            const auto result = ::send(socket, reinterpret_cast<const char*>(bytes.data() + done), count, MSG_NOSIGNAL);
#else
            const auto result = ::send(socket, reinterpret_cast<const char*>(bytes.data() + done), count, 0);
#endif
            if (result <= 0) return false;
            done += static_cast<std::size_t>(result);
        }
        return true;
    }
};
struct Wire {
    Bytes data;
    void n(std::uint64_t value, unsigned width) {
        for (unsigned i=0; i<width; ++i) data.push_back(static_cast<std::uint8_t>(value >> (8*i)));
    }
    void u8(std::uint8_t v) { n(v,1); }
    void u32(std::uint32_t v) { n(v,4); }
    void u64(std::uint64_t v) { n(v,8); }
    void append(const Bytes& b) { data.insert(data.end(),b.begin(),b.end()); }
    void string(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()+1));
        data.insert(data.end(),value.begin(),value.end()); u8(0);
    }
    void handle(unsigned id,const char* text) { u8(1); string(text); u32(id); }
    void name(unsigned ns,unsigned name) { u32(ns); u32(name); }
};
Bytes header() { Wire w; w.u32(stream_magic); w.u32(1); w.u64(1); w.u64(1); w.u32(1); return w.data; }
Bytes group(const Wire& body,unsigned count) {
    Wire w; w.u32(static_cast<std::uint32_t>(body.data.size())); w.u32(count); w.u64(1); w.u64(0); w.append(body.data); return w.data;
}
Bytes joined(std::initializer_list<Bytes> parts) { Wire w; for (const auto& p:parts) w.append(p); return w.data; }
struct Fixture {
    Bytes schema, frame1, frame2, chunk_begin, chunk_data, chunk_end, message_begin, message_data, message_end;
    Fixture() {
        Wire w;
        w.handle(1,"test"); w.handle(2,"Node"); w.handle(3,"samples");
        w.handle(4,"physx3"); w.handle(5,"PvdF32"); w.handle(6,"frame");
        w.handle(7,"value"); w.handle(8,"PvdU32"); w.handle(9,"Update");
        w.u8(2); w.name(1,2);
        w.u8(4); w.name(1,2); w.u32(3); w.u32(0); w.name(4,5); w.u8(2); w.u32(0);
        w.u8(4); w.name(1,2); w.u32(7); w.u32(0); w.name(4,8); w.u8(1); w.u32(0);
        w.u8(5); w.name(1,2); w.name(1,9); w.u32(1); w.u32(7); w.name(4,8); w.u32(0); w.u32(4); w.u32(4);
        w.u8(6); w.name(1,2); w.u64(42);
        schema=group(w,14);
        frame1=frame(5); frame2=frame(9);
        w.data.clear(); w.u8(8); w.u64(42); w.u32(3); w.name(4,5); chunk_begin=group(w,1);
        w.data.clear(); w.u8(9); w.u32(8); w.u32(0x3f800000); w.u32(0x40000000); w.u32(2); chunk_data=group(w,1);
        w.data.clear(); w.u8(10); chunk_end=group(w,1);
        w.data.clear(); w.u8(12); w.name(1,9); message_begin=group(w,1);
        w.data.clear(); w.u8(13); w.u64(42); w.u32(4); w.u32(11); message_data=group(w,1);
        w.data.clear(); w.u8(14); message_end=group(w,1);
    }
    static Bytes frame(unsigned value) {
        Wire w; w.u8(18); w.u64(42); w.u32(6); w.u64(value);
        w.u8(7); w.u64(42); w.u32(7); w.name(4,8); w.u32(1); w.u32(4); w.u32(value);
        w.u8(19); w.u64(42); w.u32(6); w.u64(value+1);
        return group(w,3);
    }
    Bytes safe() const { return joined({header(),schema,frame1}); }
    Bytes all() const { return joined({safe(),chunk_begin,chunk_data,chunk_end,message_begin,message_data,message_end,frame2}); }
};
CaptureOptions options(const std::filesystem::path& path = {}) {
    CaptureOptions value; value.port=0; value.poll_interval=5ms; value.read_buffer_bytes=7;
    value.recording_path=path.string(); value.session_options.checkpoint_interval=2;
    return value;
}
template<class Predicate> void eventually(TcpCapture& capture,Predicate predicate,const char* message) {
    const auto until=std::chrono::steady_clock::now()+5s;
    while (std::chrono::steady_clock::now()<until) {
        if (predicate(capture.status())) return;
        std::this_thread::sleep_for(2ms);
    }
    capture.cancel(); capture.wait(); throw std::runtime_error(message);
}
CaptureStatus finished(TcpCapture& capture) {
    eventually(capture,[](const CaptureStatus& s){return s.terminal();},"capture timeout");
    capture.wait(); return capture.status();
}
Bytes read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); check(bool(file),"recorded file exists");
    return Bytes(std::istreambuf_iterator<char>(file),{});
}
std::uint64_t value(const Snapshot& snapshot) {
    const auto* object=snapshot.instance(42); check(object,"live object 42");
    const auto* property=object->property("value"); check(property,"live scalar property");
    return property->as_uint().value();
}
void recovered(TcpCapture& capture,const std::filesystem::path& file,const Bytes& prefix) {
    check(read(file)==prefix,"recording is exactly the last recoverable prefix");
    auto replay=Session::load_file(file.string());
    const auto status=capture.status();
    check(replay.event_count()==status.recoverable_event_count,"paired recoverable event/byte cursor");
    check(replay.frames().size()==1 && value(replay.snapshot_at_frame(0))==5,"reopened recovered frame");
    check(value(capture.snapshot_at_event(status.recoverable_event_count))==5,"live safe-prefix snapshot equals reopened recording");
}
void happy(const std::filesystem::path& directory,const Fixture& fixture) {
    const auto file=directory/"complete.pxd2";
    TcpCapture capture(options(file)); capture.start();
    check(capture.status().state==CaptureState::listening && capture.status().port!=0,"synchronous ephemeral listen");
    Client client(capture.status().port);
    const auto h=header();
    check(client.send(Bytes(h.begin(),h.begin()+13),1),"send fragmented partial header");
    eventually(capture,[](const CaptureStatus& s){return s.wire_bytes==13;},"partial header progress");
    check(!capture.header() && capture.latest().instances.empty(),"no partial header published");
    check(client.send(joined({Bytes(h.begin()+13,h.end()),fixture.schema,fixture.frame1}),1),"send fragmented first frame");
    eventually(capture,[](const CaptureStatus& s){return s.frame_count==1;},"live first frame");
    auto copy=capture.latest(); copy.instances.clear();
    auto metadata=capture.metadata(); metadata.classes.clear();
    check(value(capture.latest())==5 && !capture.metadata().classes.empty(),"query copies do not mutate session");
    struct Observer {
        TcpCapture& capture; std::atomic<bool> running{true},failed{false}; std::thread thread;
        explicit Observer(TcpCapture& c):capture(c),thread([this]{
            try { while(running.load()) { (void)capture.metadata(); (void)capture.frames(); (void)capture.latest();
                (void)capture.snapshot_at_frame(0); std::this_thread::yield(); } }
            catch (...) {failed=true;}
        }) {}
        ~Observer(){running=false;thread.join();}
    } observer(capture);
    check(client.send(joined({fixture.chunk_begin,fixture.chunk_data,fixture.chunk_end,
                             fixture.message_begin,fixture.message_data,fixture.message_end,fixture.frame2}),3),"send cross-group transactions");
    client.close(); const auto status=finished(capture);
    check(status.state==CaptureState::completed && status.errors.empty(),"clean EOF completes");
    check(status.wire_bytes==fixture.all().size() && status.recorded_bytes==status.wire_bytes &&
          status.recoverable_bytes==status.wire_bytes && status.discarded_tail_bytes==0,"complete byte accounting");
    check(status.frame_count==2 && value(capture.snapshot_at_frame(0))==5 && value(capture.snapshot_at_frame(1))==9,"historical frame snapshots");
    check(capture.snapshot_at_frame(1,42,0).event_count==capture.snapshot_at_frame(0).event_count,"scene frame query");
    check(read(file)==fixture.all(),"complete raw recording is byte-for-byte exact");
    auto replay=Session::load_file(file.string());
    check(replay.event_count()==status.event_count && value(replay.latest())==9,"completed recording reopens");
    check(!observer.failed,"concurrent queries remain valid");
}
void truncations(const std::filesystem::path& directory,const Fixture& fixture) {
    for (int kind=0;kind<5;++kind) {
        const auto file=directory/("truncated-"+std::to_string(kind)+".pxd2");
        auto settings=options(file);
        if (kind==3) settings.read_buffer_bytes=4096;
        TcpCapture capture(settings); capture.start(); Client client(capture.status().port);
        Bytes tail;
        if (kind==0) tail=Bytes(fixture.chunk_begin.begin(),fixture.chunk_begin.begin()+10);
        if (kind==1) tail=joined({fixture.chunk_begin,fixture.chunk_data});
        if (kind==2) tail=joined({fixture.message_begin,fixture.message_data});
        if (kind==3) { Wire bad; bad.u8(255); tail=group(bad,1); }
        if (kind==4) tail=Bytes(fixture.chunk_begin.begin(),fixture.chunk_begin.begin()+10);
        check(client.send(joined({fixture.safe(),tail}),kind==3?0:5),"send interrupted fixture");
        if (kind==4) {
            eventually(capture,[&](const CaptureStatus& s){return s.wire_bytes==fixture.safe().size()+tail.size();},"bytes received before reset");
            client.reset();
        } else client.close();
        const auto status=finished(capture);
        check(status.state==(kind==3?CaptureState::failed:CaptureState::truncated),"wire/transaction interruption status");
        check(!status.errors.empty() && status.discarded_tail_bytes==tail.size(),"discarded malformed/incomplete tail reported");
        recovered(capture,file,fixture.safe());
    }
    const auto file=directory/"header-cut.pxd2";
    TcpCapture capture(options(file)); capture.start(); Client client(capture.status().port);
    const auto h=header(); check(client.send(Bytes(h.begin(),h.begin()+10)),"partial header send");
    client.close(); const auto status=finished(capture);
    check(status.state==CaptureState::truncated && !status.header_complete && status.recoverable_bytes==0 &&
          status.discarded_tail_bytes==10 && read(file).empty(),"no header means zero recoverable bytes");
}
void without_recording(const Fixture& fixture) {
    TcpCapture capture(options()); capture.start(); Client client(capture.status().port);
    check(client.send(fixture.all()),"send in-memory capture"); client.close();
    const auto status=finished(capture);
    check(status.state==CaptureState::completed && status.recorded_bytes==0 &&
          status.recoverable_bytes==fixture.all().size() && status.recoverable_event_count==status.event_count,
          "non-recording mode still exposes paired recoverable cursor");
    check(value(capture.latest())==9,"in-memory capture has live state");
}
void cancellations(const std::filesystem::path& directory,const Fixture& fixture) {
    const auto before=std::chrono::steady_clock::now();
    { TcpCapture capture(options()); capture.start(); capture.cancel(); capture.wait();
      check(capture.status().state==CaptureState::cancelled,"cancel during accept"); }
    { TcpCapture capture(options()); capture.start(); Client client(capture.status().port);
      eventually(capture,[](const CaptureStatus& s){return s.state==CaptureState::receiving;},"accepted silent client");
      capture.cancel(); capture.wait(); check(capture.status().state==CaptureState::cancelled,"cancel silent recv"); }
    { TcpCapture capture(options()); capture.start(); } // Destructor cancels and joins.
    check(std::chrono::steady_clock::now()-before<2s,"idle cancellation and destructor are prompt");
    const auto file=directory/"cancelled-transaction.pxd2";
    TcpCapture capture(options(file)); capture.start(); Client client(capture.status().port);
    const auto bytes=joined({fixture.safe(),fixture.chunk_begin,fixture.chunk_data});
    check(client.send(bytes),"send cancellation fixture");
    eventually(capture,[&](const CaptureStatus& s){return s.wire_bytes==bytes.size();},"all bytes before cancellation");
    capture.cancel(); capture.wait();
    const auto status=capture.status();
    check(status.state==CaptureState::cancelled && status.discarded_tail_bytes==bytes.size()-fixture.safe().size(),"cancelled transaction tail");
    recovered(capture,file,fixture.safe());
}
void setup_failures(const std::filesystem::path& directory) {
    const auto file=directory/"existing.pxd2";
    { std::ofstream stream(file); stream<<"keep existing bytes"; }
    const auto original=read(file);
    TcpCapture first(options()); first.start();
    auto conflict=options(file); conflict.port=first.status().port;
    TcpCapture second(conflict);
    bool rejected=false; try {second.start();} catch(const std::exception&) {rejected=true;}
    check(rejected && second.status().state==CaptureState::failed && read(file)==original,"port collision preserves existing recording");
    TcpCapture existing(options(file)); rejected=false;
    try {existing.start();} catch(const std::exception&) {rejected=true;}
    check(rejected && existing.status().state==CaptureState::failed && read(file)==original,"exclusive recording conflict preserves bytes");
    first.cancel(); first.wait();
    auto bad=options(); bad.read_buffer_bytes=0; TcpCapture invalid(bad); rejected=false;
    try {invalid.start();} catch(const std::invalid_argument&) {rejected=true;}
    check(rejected && invalid.status().state==CaptureState::failed,"invalid options fail synchronously");
    bad=options(); bad.session_options.decode_limits.max_group_bytes=std::numeric_limits<std::size_t>::max();
    TcpCapture invalid_limits(bad); rejected=false;
    try {invalid_limits.start();} catch(const std::invalid_argument&) {rejected=true;}
    check(rejected && invalid_limits.status().state==CaptureState::failed,"invalid decoder limits do not terminate host");
    rejected=false; try {first.start();} catch(const std::logic_error&) {rejected=true;}
    check(rejected,"one capture cannot be restarted or concatenate connections");
}
void bounded_pending(const std::filesystem::path& directory,const Fixture& fixture) {
    const auto file=directory/"bounded.pxd2";
    auto settings=options(file); settings.max_pending_record_bytes=128; settings.read_buffer_bytes=32;
    TcpCapture capture(settings); capture.start(); Client client(capture.status().port);
    // Schema and first frame each exceed 128 as a group; establish the schema
    // in smaller complete groups while preserving exactly the same events.
    StreamDecoder split; split.feed(fixture.safe()); split.finish();
    Wire safe; safe.append(header());
    // Re-encode fixture metadata as individual wire events using their exact
    // validated original ranges. This only changes group framing.
    const auto source=fixture.safe(); const auto& events=split.events();
    for (std::size_t i=0;i<events.size();++i) {
        const auto start=static_cast<std::size_t>(events[i].byte_offset);
        std::size_t end=i+1<events.size()?static_cast<std::size_t>(events[i+1].byte_offset):source.size();
        // The two original groups have a 24-byte group header between them.
        if (i+1<events.size() && events[i+1].byte_offset>events[i].byte_offset &&
            start<28+fixture.schema.size() && end>=28+fixture.schema.size()) end=28+fixture.schema.size();
        Wire body; body.data=Bytes(source.begin()+static_cast<std::ptrdiff_t>(start),source.begin()+static_cast<std::ptrdiff_t>(end));
        safe.append(group(body,1));
    }
    check(client.send(safe.data),"send bounded fixture prefix");
    eventually(capture,[&](const CaptureStatus& s){return s.recoverable_bytes==safe.data.size();},"bounded fixture safe prefix");
    const auto tail=joined({fixture.chunk_begin,fixture.chunk_data,fixture.chunk_data,fixture.chunk_data});
    (void)client.send(tail); const auto status=finished(capture); client.close();
    check(status.state==CaptureState::failed && !status.errors.empty() &&
          status.errors.back().find("max_pending_record_bytes")!=std::string::npos,"bounded pending transaction fails explicitly");
    check(status.discarded_tail_bytes<=128 && read(file)==safe.data,"pending raw tail stays bounded and recording remains safe");
    auto replay=Session::load_file(file.string()); check(value(replay.latest())==5,"bounded failure recording reopens");
}
} // namespace

int main() {
    try {
        Runtime runtime;
        const auto directory=std::filesystem::current_path()/("capture-tests-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(directory),"create unique test output directory");
        Fixture fixture;
        happy(directory,fixture); truncations(directory,fixture); cancellations(directory,fixture);
        without_recording(fixture);
        setup_failures(directory); bounded_pending(directory,fixture);
        std::cout<<"TCP capture tests passed; recovery artifacts: "<<directory.string()<<'\n';
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"TCP capture test failure: "<<error.what()<<'\n'; return 1;
    }
}
