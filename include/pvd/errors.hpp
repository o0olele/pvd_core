#pragma once
#include "pvd/analysis_export.hpp"
#include "pvd/session.hpp"
#include <iosfwd>
#include <limits>
namespace pvd::errors {
struct Entry {
  std::size_t event_index = 0;
  std::uint64_t stream_id = 0, timestamp = 0, byte_offset = 0;
  std::uint32_t code = 0, line = 0;
  std::string message, file;
};
struct Filter {
  std::size_t begin_event_count = 0,
              end_event_count = std::numeric_limits<std::size_t>::max();
  std::optional<std::uint32_t> code;
  std::optional<std::uint64_t> stream_id;
  std::string text, file;
};
PVD_ANALYSIS_API const char *code_name(std::uint32_t code) noexcept;
class Index {
public:
  PVD_ANALYSIS_API explicit Index(const Session &);
  PVD_ANALYSIS_API explicit Index(const std::vector<pvd::Event> &);
  PVD_ANALYSIS_API void
  update(const Session &,
         std::size_t event_count = std::numeric_limits<std::size_t>::max());
  const std::vector<Entry> &entries() const noexcept { return entries_; }
  PVD_ANALYSIS_API std::vector<Entry> select(Filter filter = {},
                                             std::size_t offset = 0,
                                             std::size_t limit = 1000) const;
  PVD_ANALYSIS_API std::size_t matching_count(Filter filter = {}) const;
  PVD_ANALYSIS_API void write_csv(std::ostream &, Filter filter = {}) const;

private:
  std::vector<Entry> entries_;
  std::size_t processed_events_ = 0;
  const Session *session_ = nullptr;
};
} // namespace pvd::errors
