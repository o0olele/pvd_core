#include "pvd/errors.hpp"
#include <algorithm>
#include <ostream>
namespace pvd::errors {
namespace {
std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}
bool matches(const Entry &e, const Filter &f) {
  if (e.event_index < f.begin_event_count || e.event_index >= f.end_event_count)
    return false;
  if (f.code && e.code != *f.code)
    return false;
  if (f.stream_id && e.stream_id != *f.stream_id)
    return false;
  if (!f.file.empty() && lower(e.file).find(lower(f.file)) == std::string::npos)
    return false;
  return f.text.empty() ||
         lower(e.message + " " + e.file + " " + code_name(e.code))
                 .find(lower(f.text)) != std::string::npos;
}
void quoted(std::ostream &out, const std::string &s) {
  out << '"';
  for (char c : s) {
    if (c == '"')
      out << '"';
    out << c;
  }
  out << '"';
}
} // namespace
const char *code_name(std::uint32_t code) noexcept {
  switch (code) {
  case 0:
    return "No error";
  case 1:
    return "Debug info";
  case 2:
    return "Warning";
  case 4:
    return "Invalid parameter";
  case 8:
    return "Invalid operation";
  case 16:
    return "Out of memory";
  case 32:
    return "Internal error";
  case 64:
    return "Abort";
  case 128:
    return "Performance warning";
  default:
    return "Unknown code";
  }
}
Index::Index(const Session &s) : Index(s.events()) { session_ = &s; }
Index::Index(const std::vector<pvd::Event> &events) {
  processed_events_ = events.size();
  for (std::size_t i = 0; i < events.size(); ++i)
    if (const auto *e = std::get_if<ErrorMessage>(&events[i].payload))
      entries_.push_back({i, events[i].stream_id, events[i].timestamp,
                          events[i].byte_offset, e->code, e->line, e->message,
                          e->file});
}
void Index::update(const Session &session, std::size_t prefix) {
  if (session_ && session_ != &session)
    throw std::invalid_argument(
        "error index cannot combine different sessions");
  if (prefix == std::numeric_limits<std::size_t>::max())
    prefix = session.event_count();
  if (prefix > session.event_count())
    throw std::out_of_range("error update prefix exceeds session");
  if (processed_events_ > session.event_count())
    throw std::invalid_argument("error session history shrank");
  session_ = &session;
  const auto &events = session.events();
  while (processed_events_ < prefix) {
    auto i = processed_events_;
    if (const auto *e = std::get_if<ErrorMessage>(&events[i].payload))
      entries_.push_back({i, events[i].stream_id, events[i].timestamp,
                          events[i].byte_offset, e->code, e->line, e->message,
                          e->file});
    ++processed_events_;
  }
}

std::vector<Entry> Index::select(Filter f, std::size_t offset,
                                 std::size_t limit) const {
  if (f.begin_event_count > f.end_event_count)
    throw std::invalid_argument("invalid error event range");
  std::vector<Entry> out;
  for (const auto &e : entries_)
    if (matches(e, f)) {
      if (offset) {
        --offset;
        continue;
      }
      if (out.size() == limit)
        break;
      out.push_back(e);
    }
  return out;
}
std::size_t Index::matching_count(Filter f) const {
  if (f.begin_event_count > f.end_event_count)
    throw std::invalid_argument("invalid error event range");
  std::size_t n = 0;
  for (const auto &e : entries_)
    if (matches(e, f))
      ++n;
  return n;
}
void Index::write_csv(std::ostream &out, Filter f) const {
  if (f.begin_event_count > f.end_event_count)
    throw std::invalid_argument("invalid error event range");
  out << "event_index,stream_id,timestamp,byte_offset,code,code_name,message,"
         "file,line\r\n";
  for (const auto &e : entries_)
    if (matches(e, f)) {
      out << e.event_index << ',' << e.stream_id << ',' << e.timestamp << ','
          << e.byte_offset << ',' << e.code << ',';
      quoted(out, code_name(e.code));
      out << ',';
      quoted(out, e.message);
      out << ',';
      quoted(out, e.file);
      out << ',' << e.line << "\r\n";
    }
  if (!out)
    throw std::runtime_error("unable to write error CSV");
}
} // namespace pvd::errors
