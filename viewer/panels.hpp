#pragma once
#include "model.hpp"
#include <array>
#include <map>

namespace pvd::viewer {

struct PanelState {
  std::array<char, 256> hierarchy_search{};
  std::array<char, 128> property_search{};
  std::map<std::string, std::uint64_t> array_pages;
  std::optional<ObjectKey> reveal_object;
  std::optional<PickingKey> last_selection;
  std::string error;
  std::size_t array_page_size = 32;
};

// Content-only panels: the host owns Begin/End and its layout. The host calls
// Document::advance(dt) exactly once per frame; these controls issue model
// commands.
void drawHierarchy(Document &, PanelState &);
void drawInspector(Document &, PanelState &);
void drawTimeline(Document &, PanelState &);
void drawDiagnostics(Document &, PanelState &);
void drawEmptyState(const char *title, const char *description);

// Bounded, endian-aware property summary shared by inspector rows and tooltips.
// Wrong/incomplete typed data is represented explicitly rather than throwing.
std::string propertyValueText(const PropertyValue &, std::size_t item = 0);

} // namespace pvd::viewer
