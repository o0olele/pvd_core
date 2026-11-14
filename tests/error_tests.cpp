#include <iostream>
#include <pvd/errors.hpp>
#include <sstream>
#include <stdexcept>
namespace {
using namespace pvd;
void check(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
void run() {
  std::vector<Event> events{
      {1, 10, 100,
       ErrorMessage{2, "Bad, \"value\"\nUTF8 \xe5\x90\x8d", "c:\\a,b.cpp", 42}},
      {2, 20, 200, StreamEndEvent{"done"}},
      {2, 30, 300, ErrorMessage{128, "Slow", "worker.cpp", 7}},
      {1, 40, 400, ErrorMessage{0xdeadbeef, "Future code", "other.cpp", 9}}};
  errors::Index i(events);
  check(i.entries().size() == 3, "error count incorrect");
  check(i.entries()[0].event_index == 0 && i.entries()[0].timestamp == 10 &&
            i.entries()[0].byte_offset == 100 && i.entries()[0].line == 42,
        "error metadata lost");
  check(i.entries().back().code == 0xdeadbeef &&
            std::string(errors::code_name(0xdeadbeef)) == "Unknown code",
        "unknown error changed");
  const std::uint32_t codes[] = {0, 1, 2, 4, 8, 16, 32, 64, 128};
  for (auto c : codes)
    check(std::string(errors::code_name(c)) != "Unknown code",
          "SDK error code missing");
  errors::Filter f;
  f.end_event_count = 2;
  check(i.matching_count(f) == 1, "range end not exclusive");
  f = {};
  f.stream_id = 2;
  f.text = "PERFORMANCE";
  check(i.select(f).size() == 1, "stream/code-name filter incorrect");
  f = {};
  f.file = "A,B.CPP";
  check(i.select(f).size() == 1, "case-insensitive file filter incorrect");
  f = {};
  f.code = 128;
  check(i.select(f).size() == 1, "code filter incorrect");
  check(i.select({}, 1, 1)[0].code == 128 && i.select({}, 0, 0).empty() &&
            i.select({}, 99, 1).empty(),
        "pagination incorrect");
  f = {};
  f.begin_event_count = 3;
  f.end_event_count = 1;
  bool rejected = false;
  try {
    i.select(f);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "reversed range accepted");
  std::ostringstream csv;
  i.write_csv(csv);
  check(csv.str().find("\"Bad, \"\"value\"\"\nUTF8") != std::string::npos &&
            csv.str().find("\"Unknown code\"") != std::string::npos &&
            csv.str().find("\r\n") != std::string::npos,
        "CSV escape lost");
  Session s(StreamHeader{}, events);
  errors::Index live(std::vector<Event>{});
  for (std::size_t n = 0; n <= events.size(); ++n) {
    live.update(s, n);
    live.update(s, n);
    errors::Filter range;
    range.end_event_count = n;
    check(live.entries().size() == i.matching_count(range),
          "incremental prefix duplicated/error missing");
  }
  live.update(s, 0);
  check(live.entries().size() == 3, "old prefix rewound error index");
  Session streaming(StreamHeader{});
  errors::Index append(std::vector<Event>{});
  for (const auto &e : events) {
    streaming.append({e});
    append.update(streaming);
  }
  check(append.entries().size() == 3, "live append errors lost");
  bool different = false;
  try {
    live.update(streaming);
  } catch (const std::invalid_argument &) {
    different = true;
  }
  check(different, "different sessions combined");
}
} // namespace
int main() {
  try {
    run();
    std::cout << "error tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
