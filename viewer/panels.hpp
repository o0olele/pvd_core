#pragma once
#include "inspection_preferences.hpp"
#include "model.hpp"
#include <array>
#include <functional>
#include <limits>
#include <map>

namespace pvd::viewer {

struct PanelState {
  std::array<char, 256> hierarchy_search{};
  std::array<char, 128> property_search{};
  std::map<std::string, std::uint64_t> array_pages;
  std::optional<ObjectKey> reveal_object;
  std::optional<PickingKey> last_selection;
  std::string error;
  InspectorPreferences inspector_preferences;
  // Empty uses ImGui's clipboard. The callback receives the complete UTF-8 text
  // once per explicit Copy command, and must not replace this panel/document.
  std::function<void(const std::string &)> copy_text;
  std::size_t array_visible_rows = 0;
  // Value-only cache: safe across immutable FrameState replacement/selection.
  const Session *category_source = nullptr;
  std::size_t category_prefix = std::numeric_limits<std::size_t>::max(),
              category_page = 0;
  std::vector<ObjectGroup> categories;
  std::string category_query;
  std::vector<std::vector<std::size_t>> category_matches;
  std::vector<std::size_t> matching_categories;
  std::map<std::string, std::size_t> category_object_pages;
  std::size_t object_count = 0, object_visible_rows = 0;
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
std::string propertyValueText(const PropertyValue &, std::size_t item = 0,
                              const InspectorPreferences & = {},
                              bool last_wire_value = false,
                              const PropertyDefinition * = nullptr);

enum class PropertyCopy { native_value, last_wire_value, wire_bytes };
// Complete selected element, never the bounded row preview. Decoded values use
// General float9/double17 regardless of row precision; integer_hex is respected.
// wire_bytes is an exact hex dump in incoming byte order. Incomplete/ambiguous
// element boundaries throw instead of copying an invented value.
std::string propertyValueCopyText(const PropertyValue &, std::size_t item,
                                  PropertyCopy = PropertyCopy::native_value,
                                  const InspectorPreferences & = {},
                                  const PropertyDefinition * = nullptr);

} // namespace pvd::viewer
