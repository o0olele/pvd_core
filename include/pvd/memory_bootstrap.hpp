#pragma once
#include "pvd/memory.hpp"

namespace pvd::memory {
enum class UnknownFreePolicy { reject, allow_partial };
struct BootstrapOptions {
  Options decode_limits;
  std::size_t max_input_bytes = 256u * 1024u * 1024u;
  std::size_t max_total_bytes = 64u * 1024u * 1024u;
  std::size_t max_sources = 4096;
  std::size_t max_dictionary_entries = 1024u * 1024u;
  std::size_t max_active_allocations = 2u * 1024u * 1024u;
  UnknownFreePolicy unknown_frees = UnknownFreePolicy::reject;
};
struct SourceBootstrap {
  Source source;
  std::uint64_t buffer_generation = 0;
  Bytes bytes;
  std::size_t dictionary_entries = 0, active_allocations = 0;
  std::string property_name = "events";
};
struct Bootstrap {
  std::size_t event_count = 0, total_bytes = 0;
  std::vector<SourceBootstrap> sources;
  // Only live-source diagnostics are relevant to this continued baseline.
  // Explicit allow_partial may return unknown-free diagnostics. Every other
  // live-source diagnostic rejects construction; corrupt state is not guessed.
  std::vector<Diagnostic> diagnostics;
  bool partial = false;
};
class BootstrapError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
// Canonical memory-event bytes for sources whose buffer is live at this prefix.
// Emits each prior dictionary entry once, followed only by active allocations.
// No future declarations, frees, timestamps or historical peaks are invented.
// Source byte order and original handles/addresses are retained. Allocation
// generations rebase to zero in the cropped recording; cumulative/peak usage
// starts with the synthetic live-allocation baseline.
PVD_ANALYSIS_API Bootstrap make_bootstrap(const Session &,
                                          std::size_t event_count,
                                          const BootstrapOptions & = {});
} // namespace pvd::memory
