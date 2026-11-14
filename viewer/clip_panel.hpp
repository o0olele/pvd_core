#pragma once
#include "clip_ranges.hpp"
#include "model.hpp"
#include <array>

namespace pvd::viewer {
struct ClipSaveRequest {
  std::uint64_t source_slot = 0;
  clips::Clip clip; // Complete immutable binding/range copied at the command.
};
enum class ClipPanelChangeKind { edited, verified, discarded };
struct ClipPanelChange {
  std::uint64_t source_slot = 0;
  ClipPanelChangeKind kind = ClipPanelChangeKind::edited;
};
struct ClipPanelActions {
  // The host resolves this stable source_slot, validates clip against that
  // Session, obtains a destination, locks/freezes that slot and starts its
  // existing ExportJob. Never resolve the active tab after choosing a path.
  std::function<void(ClipSaveRequest)> save_as;
  // Runs after successful catalog/selection/verification/discard commands.
  // The host reads this slot's panel states and persists via Workspace. Never
  // called merely because the panel was drawn or a live prefix grew.
  // Do not replace this PanelState/catalog map or close/replace its Document
  // inside the callback. Synchronize external revisions before the next draw.
  std::function<void(ClipPanelChange)> changed;
  bool busy = false; // Source replacement/export lock, NOT ordinary live input.
};
struct ClipPanelState {
  // Root may persist state() in its workspace archive. Catalog has no Session
  // pointer; restore() verifies the stored prefix before publishing it.
  std::map<clips::SceneKey, clips::Catalog> catalogs;
  std::optional<clips::SceneKey> active_catalog;
  std::uint64_t source_slot = 0;
  const Session *observed_source = nullptr; // Value-only pointer comparison.
  bool needs_verification = false;
  std::array<char, 257> name{};
  std::uint64_t first_frame = 1, last_frame = 1;
  std::optional<clips::Id> edited_clip;
  std::string error;
  std::size_t visible_rows = 0;
};
// Content-only; caller owns Begin/End and serializes Session append/queries.
// Binding/refresh/hash work is explicit on commands, never repeated per draw.
// Scene/lifetime catalogs are retained independently. "Use current scene"
// creates or activates that catalog; switching scenes never deletes ranges.
// Opening another source requires verification/restoration; stable source_slot
// guards accidental reuse between tabs and source changes disable commands.
void drawClipRanges(Document &, std::uint64_t source_slot, ClipPanelState &,
                    const ClipPanelActions & = {});
} // namespace pvd::viewer
