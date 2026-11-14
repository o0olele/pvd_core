#pragma once
#include "pvd/profile.hpp"

namespace pvd::profile {
struct BootstrapOptions {
  Options decode_limits;
  std::size_t max_input_bytes = 256u * 1024u * 1024u;
  std::size_t max_total_bytes = 64u * 1024u * 1024u;
  std::size_t max_sources = 4096;
  std::size_t max_open_scopes = 128u * 1024u;
  std::size_t max_counters = 1024u * 1024u;
};
struct BootstrapRecord {
  std::size_t payload_offset = 0;
  Kind kind = Kind::start;
  Source original;
};
struct SourceBootstrap {
  std::uint64_t stream_id = 0;
  ZoneKey zone;
  std::uint64_t buffer_generation = 0;
  std::string property_name;
  Bytes bytes;
  std::size_t open_scopes = 0, counters = 0;
  std::vector<BootstrapRecord> records;
};
struct Bootstrap {
  std::size_t event_count = 0, total_bytes = 0;
  std::vector<SourceBootstrap> sources;
  // Supported counter/clock limitations remain explicit and are reproduced by
  // the canonical bytes/header. Malformed/GPU/ambiguous sources are rejected.
  std::vector<Diagnostic> diagnostics;
};
class BootstrapError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
// Reconstruct CPU state for profile sources whose buffer is alive at prefix.
// Emits outstanding starts (absolute full context/ticks) in original order,
// then only the latest counter per event/thread/context. Compression never
// spans packets. No completed interval, stop, GPU payload or old motion is
// copied. records maps canonical offsets back to original source locations.
// Multiple properties/streams for one live zone are explicitly refused because
// grouping them could change cross-stream recursive start ordering.
PVD_ANALYSIS_API Bootstrap make_bootstrap(const Session &,
                                          std::size_t event_count,
                                          const BootstrapOptions & = {});
} // namespace pvd::profile
