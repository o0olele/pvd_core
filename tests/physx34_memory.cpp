// Consumes a real PhysX SDK recording; never links to the producer or SDK.
#include <fstream>
#include <iostream>
#include <map>
#include <pvd/errors.hpp>
#include <pvd/memory.hpp>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream in(line);
  std::string s;
  while (std::getline(in, s, ','))
    out.push_back(s);
  return out;
}
using Key = std::pair<std::uint64_t, std::uint64_t>;
struct Expected {
  std::uint64_t address, generation, size;
  std::string type, file;
  std::uint32_t line;
  bool active;
};
std::map<unsigned, std::vector<Expected>> expected(const std::string &file) {
  std::ifstream in(file);
  check(bool(in), "open allocation expectations");
  std::string line;
  std::getline(in, line);
  std::map<unsigned, std::vector<Expected>> rows;
  while (std::getline(in, line)) {
    auto c = split(line);
    check(c.size() == 8, "allocation CSV columns");
    rows[std::stoul(c[0])].push_back(
        {std::stoull(c[1]), std::stoull(c[2]), std::stoull(c[3]), c[4], c[5],
         static_cast<std::uint32_t>(std::stoul(c[6])), std::stoul(c[7]) != 0});
  }
  return rows;
}
std::map<unsigned, std::vector<std::uint64_t>>
summaries(const std::string &file) {
  std::ifstream in(file);
  check(bool(in), "open summary expectations");
  std::string line;
  std::getline(in, line);
  std::map<unsigned, std::vector<std::uint64_t>> rows;
  while (std::getline(in, line)) {
    auto c = split(line);
    check(c.size() == 7, "summary CSV columns");
    for (std::size_t i = 1; i < c.size(); ++i)
      rows[std::stoul(c[0])].push_back(std::stoull(c[i]));
  }
  return rows;
}
bool fixture(const memory::Allocation &a) {
  return a.type_name && a.type_name->rfind("MemoryFixture.", 0) == 0;
}
void verify(memory::Index &index, const std::vector<Expected> &rows,
            const std::vector<std::uint64_t> &summary, std::size_t prefix,
            std::uint64_t &observed_peak) {
  auto history = index.history_at_event(
      prefix, {}, 0, std::numeric_limits<std::size_t>::max());
  std::map<Key, memory::Allocation> actual;
  for (const auto &a : history)
    if (fixture(a))
      actual.emplace(Key{a.address, a.generation}, a);
  check(actual.size() == rows.size(),
        "real allocation history count mismatch at prefix " +
            std::to_string(prefix));
  std::uint64_t current = 0, allocated = 0, freed = 0, alloc_count = 0,
                free_count = 0;
  for (const auto &row : rows) {
    auto it = actual.find({row.address, row.generation});
    check(it != actual.end(), "SDK address/generation missing");
    const auto &a = it->second;
    check(a.size == row.size && a.type_name == row.type && a.file == row.file &&
              a.line == row.line,
          "SDK allocation fields differ from allocator callback");
    check((a.end_reason == memory::EndReason::none) == row.active,
          "SDK allocation active state mismatch");
    check(row.active ? !a.ended_event_count
                     : (a.ended_event_count && *a.ended_event_count <= prefix &&
                        a.end_reason == memory::EndReason::freed),
          "future end leaked or real free lost");
    allocated += a.size;
    ++alloc_count;
    if (row.active)
      current += a.size;
    else {
      freed += a.size;
      ++free_count;
    }
  }
  auto snapshot =
      index.snapshot_at_event(prefix, memory::GroupBy::type_file_line);
  std::uint64_t grouped_current = 0, grouped_allocated = 0, grouped_freed = 0,
                grouped_alloc_count = 0, grouped_free_count = 0;
  for (const auto &group : snapshot.groups)
    if (group.type_name && group.type_name->rfind("MemoryFixture.", 0) == 0) {
      grouped_current += group.stats.current_bytes;
      grouped_allocated += group.stats.allocated_bytes;
      grouped_freed += group.stats.freed_bytes;
      grouped_alloc_count += group.stats.allocation_count;
      grouped_free_count += group.stats.free_count;
      check(group.stats.peak_bytes == group.stats.allocated_bytes,
            "SDK fixture per-type peak mismatch");
    }
  observed_peak = std::max(observed_peak, current);
  check(summary == std::vector<std::uint64_t>{current, observed_peak, allocated,
                                              freed, alloc_count, free_count},
        "allocation callback cumulative/peak mismatch");
  check(current == grouped_current && allocated == grouped_allocated &&
            freed == grouped_freed && alloc_count == grouped_alloc_count &&
            free_count == grouped_free_count,
        "real group statistics mismatch");
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 4,
          "usage: physx34_memory capture.pxd2 expected.csv summary.csv");
    auto session = Session::load_file(argv[1]);
    auto rows = expected(argv[2]);
    auto summary = summaries(argv[3]);
    check(rows.size() == 5 && summary.size() == 5,
          "five real SDK phases required");
    std::map<unsigned, std::size_t> phases;
    for (const auto &marker : session.sections())
      if (marker.name.rfind("memory_validation_", 0) == 0)
        phases[std::stoul(marker.name.substr(18))] = marker.event_count;
    check(phases.size() == 5, "phase markers missing");
    memory::Index index(session), live;
    errors::Index errors(session), live_errors(std::vector<Event>{});
    check(!index.allocations().empty(), "real memory channel empty");
    for (const auto &d : index.diagnostics())
      check(d.code == memory::DiagnosticCode::unknown_free,
            "unexpected malformed/unsupported real SDK memory data: " +
                d.message);
    std::uint64_t peak = 0, live_peak = 0;
    for (const auto &entry : phases) {
      auto p = entry.first;
      auto prefix = entry.second;
      verify(index, rows.at(p), summary.at(p), prefix, peak);
      live.update(session, prefix);
      live.update(session, prefix);
      verify(live, rows.at(p), summary.at(p), prefix, live_peak);
      live_errors.update(session, prefix);
      errors::Filter f;
      f.end_event_count = prefix;
      f.text = "MemoryFixture phase";
      auto selected = errors.select(f);
      check(selected.size() == p + 1, "SDK error prefix count mismatch");
      auto &e = selected.back();
      check(e.code == (p % 2 ? 128u : 2u) && e.line == 100 + p &&
                e.file == "memory_fixture.cpp" &&
                e.message == "MemoryFixture phase " + std::to_string(p),
            "SDK typed error fields mismatch");
      check(live_errors.matching_count(f) == selected.size(),
            "SDK incremental error mismatch");
    }
    // Reverse seeks use the same already-decoded dictionary/lifetime history.
    for (auto it = phases.rbegin(); it != phases.rend(); ++it) {
      std::uint64_t ignored = summary.at(it->first)[1];
      verify(index, rows.at(it->first), summary.at(it->first), it->second,
             ignored);
    }
    live.update(session);
    check(live.events().size() == index.events().size() &&
              live.allocations().size() == index.allocations().size(),
          "SDK incremental decode duplicated memory");
    std::cout << "real SDK memory/error passed: " << session.event_count()
              << " outer events, " << index.events().size()
              << " memory events, " << index.allocations().size()
              << " allocations, 5 prefixes, 2 address reuses, peak " << peak
              << " fixture bytes; " << index.diagnostics().size()
              << " source diagnostics\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
