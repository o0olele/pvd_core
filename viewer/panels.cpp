#include "panels.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <limits>
#include <sstream>

namespace pvd::viewer {
namespace {
std::string id_text(std::uint64_t id) {
  char out[24];
  std::snprintf(out, sizeof(out), "0x%llX",
                static_cast<unsigned long long>(id));
  return out;
}
std::string real(double n) {
  char out[48];
  std::snprintf(out, sizeof(out), "%.6g", n);
  return out;
}
std::string numeric(double n, bool wide,
                    const InspectorPreferences &preferences) {
  if (!valid_inspector_preferences(preferences))
    return "<invalid display preferences>";
  const char *format =
      preferences.number_format == NumberFormat::scientific ? "%.*e"
      : preferences.number_format == NumberFormat::fixed    ? "%.*f"
                                                            : "%.*g";
  char out[512];
  const auto precision =
      wide ? preferences.double_precision : preferences.float_precision;
  const auto written =
      std::snprintf(out, sizeof(out), format, static_cast<int>(precision), n);
  return written < 0 || static_cast<std::size_t>(written) >= sizeof(out)
             ? "<unavailable>"
             : std::string(out);
}
std::string vec(Vec3 v, const InspectorPreferences &p) {
  return numeric(v.x, false, p) + ", " + numeric(v.y, false, p) + ", " +
         numeric(v.z, false, p);
}
std::string quat(Quat q, const InspectorPreferences &p) {
  return numeric(q.x, false, p) + ", " + numeric(q.y, false, p) + ", " +
         numeric(q.z, false, p) + ", " + numeric(q.w, false, p);
}
std::string lower(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return value;
}
bool matches(const std::string &haystack, const std::string &needle) {
  return needle.empty() || lower(haystack).find(needle) != std::string::npos;
}
std::string clipped(const std::string &s, std::size_t count = 512) {
  if (s.size() <= count)
    return s;
  auto end = count;
  while (end && ((static_cast<unsigned char>(s[end]) & 0xc0) == 0x80))
    --end;
  return s.substr(0, end) + "...";
}
void text(const std::string &value) {
  auto short_value = clipped(value, 4096);
  ImGui::TextUnformatted(short_value.c_str());
}
void muted(const std::string &value) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  text(value);
  ImGui::PopStyleColor();
}
void tooltip(const std::string &value) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    text(value);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}
struct IdScope {
  explicit IdScope(const std::string &id) { ImGui::PushID(id.c_str()); }
  ~IdScope() { ImGui::PopID(); }
};
struct TableScope {
  bool open;
  TableScope(const char *id, int cols, ImGuiTableFlags flags)
      : open(ImGui::BeginTable(id, cols, flags)) {}
  ~TableScope() {
    if (open)
      ImGui::EndTable();
  }
};
struct DisabledScope {
  explicit DisabledScope(bool disabled) { ImGui::BeginDisabled(disabled); }
  ~DisabledScope() { ImGui::EndDisabled(); }
};
template <class F> void command(PanelState &state, F &&action) {
  try {
    action();
    state.error.clear();
  } catch (const std::exception &e) {
    state.error = e.what();
  } catch (...) {
    state.error = "This action could not be completed.";
  }
}
void panel_error(const PanelState &state) {
  if (!state.error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.98f, .52f, .45f, 1));
    ImGui::TextWrapped("%s", clipped(state.error).c_str());
    ImGui::PopStyleColor();
  }
}
bool same(const ObjectKey &a, const ObjectKey &b) {
  return a.id == b.id && a.generation == b.generation;
}
std::string object_name(const InstanceState *object) {
  if (!object)
    return {};
  for (const char *name : {"Name", "name"})
    if (auto p = object->property(name)) {
      auto s = p->as_string();
      if (s && !s->empty())
        return *s;
    }
  return object->class_name.name;
}
void inspect(Document &doc, PanelState &state, std::uint64_t id) {
  command(state, [&] {
    if (!doc.inspect(id))
      throw std::runtime_error(
          "This object is not present at the current frame.");
    state.reveal_object = doc.inspected_object();
  });
}
std::uint64_t raw_uint(const PropertyValue &v, std::size_t offset,
                       std::size_t width) {
  const auto &b = v.raw_bytes();
  if (width > 8 || offset > b.size() || width > b.size() - offset)
    throw std::out_of_range("incomplete property");
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    auto j = v.byte_order == ByteOrder::little ? i : width - 1 - i;
    value |= std::uint64_t(b[offset + j]) << (i * 8);
  }
  return value;
}
float raw_float(const PropertyValue &v, std::size_t offset) {
  auto bits = static_cast<std::uint32_t>(raw_uint(v, offset, 4));
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
void property_value(Document &doc, PanelState &state,
                    const PropertyValue &value, std::size_t item,
                    bool compact = false) {
  if (value.reference_ids) {
    if (item >= value.reference_ids->size()) {
      muted("Unavailable reference");
      return;
    }
    const auto id = (*value.reference_ids)[item];
    if (!id) {
      muted("None");
      return;
    }
    auto frame = doc.frame();
    const auto *target = frame ? frame->snapshot.instance(id) : nullptr;
    const auto label = id_text(id);
    if (target) {
      if (ImGui::SmallButton(label.c_str()))
        inspect(doc, state, id);
      tooltip(object_name(target) + " | " + target->class_name.str());
    } else {
      muted(label + "  (not present)");
    }
    return;
  }
  const auto summary =
      propertyValueText(value, item, state.inspector_preferences);
  auto display = summary;
  if (compact) {
    std::replace(display.begin(), display.end(), '\n', ' ');
    std::replace(display.begin(), display.end(), '\r', ' ');
  }
  text(display);
  tooltip(clipped(summary, 1024) + "\n" +
          std::to_string(value.raw_bytes().size()) + " captured bytes");
}
void properties(Document &doc, PanelState &state, const InstanceState &object) {
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##property-search", "Filter properties...",
                           state.property_search.data(),
                           state.property_search.size());
  ImGui::Spacing();
  TableScope table("##properties", 3,
                   ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                       ImGuiTableFlags_BordersInnerV);
  if (!table.open)
    return;
  ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, .9f);
  ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
  ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, .7f);
  ImGui::TableHeadersRow();
  const auto query = lower(state.property_search.data());
  for (const auto &entry : object.properties) {
    const auto &name = entry.first;
    const auto &value = entry.second;
    if (!matches(name, query) && !matches(value.incoming_type.str(), query))
      continue;
    IdScope id(name);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const auto *metadata = doc.metadata();
    const auto *definition =
        metadata ? metadata->find_property(object.class_name, name) : nullptr;
    const bool array =
        definition ? definition->property_type == 2 : value.item_count != 1;
    bool expanded = false;
    if (array)
      expanded = ImGui::TreeNodeEx(clipped(name).c_str(),
                                   ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth);
    else
      text(name);
    tooltip(name);
    ImGui::TableSetColumnIndex(1);
    if (array)
      muted(std::to_string(value.item_count) + " items");
    else
      property_value(doc, state, value, 0);
    ImGui::TableSetColumnIndex(2);
    muted(value.incoming_type.name);
    tooltip(value.incoming_type.str());
    if (expanded && value.item_count) {
      const auto page_size = state.inspector_preferences.array_page_size;
      auto &page =
          state.array_pages[std::to_string(object.id) + ":" +
                            std::to_string(object.generation) + ":" + name];
      const auto last = (value.item_count - 1) / page_size;
      page = std::min(page, last);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(1);
      {
        DisabledScope disabled(page == 0);
        if (ImGui::SmallButton("<"))
          --page;
      }
      ImGui::SameLine();
      ImGui::Text("%llu-%llu / %llu",
                  static_cast<unsigned long long>(page * page_size + 1),
                  static_cast<unsigned long long>(std::min<std::uint64_t>(
                      value.item_count,
                      page * page_size +
                          std::min<std::uint64_t>(
                              page_size, value.item_count - page * page_size))),
                  static_cast<unsigned long long>(value.item_count));
      ImGui::SameLine();
      {
        DisabledScope disabled(page == last);
        if (ImGui::SmallButton(">"))
          ++page;
      }
      const auto start = page * page_size;
      const auto count =
          std::min<std::uint64_t>(page_size, value.item_count - start);
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(count));
      while (clipper.Step())
        for (int row_offset = clipper.DisplayStart;
             row_offset < clipper.DisplayEnd; ++row_offset) {
          const auto offset = static_cast<std::uint64_t>(row_offset);
          ++state.array_visible_rows;
          const auto item = start + offset;
          IdScope row(std::to_string(item));
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          muted("  [" + std::to_string(item) + "]");
          ImGui::TableSetColumnIndex(1);
          if (item > std::numeric_limits<std::size_t>::max())
            muted("Index unavailable on this host");
          else
            property_value(doc, state, value, static_cast<std::size_t>(item),
                           true);
        }
    }
  }
}
void role_button(Document &doc, PanelState &state, const char *label,
                 ObjectKey key) {
  auto current = doc.inspected_object();
  const bool active = current && same(*current, key);
  if (active)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_Header));
  if (ImGui::SmallButton(label))
    inspect(doc, state, key.id);
  if (active)
    ImGui::PopStyleColor();
}
std::optional<double> interval_ms(const Document &doc,
                                  const TimelineEntry &entry) {
  const auto *session = doc.session();
  if (!session)
    return {};
  std::uint64_t begin = 0, end = 0;
  bool found = false;
  if (entry.sample_point == SamplePoint::native_frame) {
    for (const auto &marker : session->frames())
      if (marker.stream_id == entry.stream_id &&
          marker.event_count == entry.event_count) {
        begin = marker.begin_timestamp;
        end = marker.end_timestamp;
        found = true;
        break;
      }
  } else
    for (const auto &marker : session->sections())
      if (marker.stream_id == entry.stream_id &&
          marker.event_count == entry.event_count) {
        begin = marker.begin_timestamp;
        end = marker.end_timestamp;
        found = true;
        break;
      }
  const auto &h = session->header();
  if (!found || end < begin || !h.timestamp_denominator)
    return {};
  const auto ms = static_cast<long double>(end - begin) *
                  h.timestamp_numerator / h.timestamp_denominator / 1000000.0L;
  if (!std::isfinite(ms) || ms > std::numeric_limits<double>::max())
    return {};
  return static_cast<double>(ms);
}
std::string scene_label(const SceneInfo &scene) {
  return scene.name.empty() ? "Scene " + id_text(scene.key.id) : scene.name;
}

void shape_menu(Document &doc, PanelState &state, const PickingKey &key) {
  if (!ImGui::BeginPopupContextItem("##shape-actions"))
    return;
  if (ImGui::MenuItem("Hide attachment"))
    command(state, [&] { doc.set_visible(key, false); });
  if (ImGui::MenuItem("Show attachment"))
    command(state, [&] { doc.set_visible(key, true); });
  if (ImGui::MenuItem("Isolate attachment"))
    command(state, [&] { doc.isolate(key); });
  ImGui::Separator();
  if (ImGui::MenuItem("Restore all"))
    command(state, [&] { doc.show_all(); });
  ImGui::TextDisabled(
      "Show removes manual hide; category filters still apply.");
  ImGui::EndPopup();
}
void actor_menu(Document &doc, PanelState &state, ObjectKey scene,
                ObjectKey actor) {
  if (!ImGui::BeginPopupContextItem("##actor-actions"))
    return;
  if (ImGui::MenuItem("Hide actor"))
    command(state, [&] { doc.set_actor_visible(scene, actor, false); });
  if (ImGui::MenuItem("Show actor"))
    command(state, [&] { doc.set_actor_visible(scene, actor, true); });
  if (ImGui::MenuItem("Isolate actor"))
    command(state, [&] { doc.isolate_actor(scene, actor); });
  ImGui::Separator();
  if (ImGui::MenuItem("Restore all"))
    command(state, [&] { doc.show_all(); });
  ImGui::EndPopup();
}
void object_categories(Document &doc, PanelState &state,
                       const std::string &query) {
  if (state.reveal_object || !query.empty())
    ImGui::SetNextItemOpen(true);
  if (!ImGui::CollapsingHeader("All objects by class (all scenes)"))
    return;
  const auto prefix = doc.frame()->snapshot.event_count;
  bool refresh =
      state.category_source != doc.session() || state.category_prefix != prefix;
  if (refresh) {
    state.categories = doc.object_groups();
    state.category_source = doc.session();
    state.category_prefix = prefix;
    state.category_page = 0;
    state.category_object_pages.clear();
  }
  if (refresh || state.category_query != query) {
    state.category_query = query;
    state.matching_categories.clear();
    state.category_matches.clear();
    state.category_matches.resize(state.categories.size());
    state.object_count = 0;
    for (std::size_t gi = 0; gi < state.categories.size(); ++gi) {
      const auto &group = state.categories[gi];
      auto &list = state.category_matches[gi];
      const bool class_match = matches(group.class_name.str(), query);
      for (std::size_t oi = 0; oi < group.objects.size(); ++oi) {
        const auto &object = group.objects[oi];
        if (class_match || matches(object.name + " " + id_text(object.key.id) +
                                       " " + std::to_string(object.key.id),
                                   query))
          list.push_back(oi);
      }
      if (!list.empty()) {
        state.matching_categories.push_back(gi);
        state.object_count += list.size();
      }
    }
  }
  constexpr std::size_t group_page_size = 32, object_page_size = 128;
  if (state.reveal_object)
    for (std::size_t g = 0; g < state.matching_categories.size(); ++g) {
      const auto gi = state.matching_categories[g];
      const auto &objects = state.categories[gi].objects;
      const auto &list = state.category_matches[gi];
      for (std::size_t i = 0; i < list.size(); ++i)
        if (same(objects[list[i]].key, *state.reveal_object)) {
          state.category_page = g / group_page_size;
          state.category_object_pages[state.categories[gi].class_name.str()] =
              i / object_page_size;
        }
    }
  const auto last =
      state.matching_categories.empty()
          ? 0
          : (state.matching_categories.size() - 1) / group_page_size;
  state.category_page = std::min(state.category_page, last);
  muted(std::to_string(state.object_count) + " matching live objects | " +
        std::to_string(state.matching_categories.size()) + " classes");
  if (last) {
    DisabledScope prev(!state.category_page);
    if (ImGui::SmallButton("Previous classes"))
      --state.category_page;
  }
  if (last) {
    ImGui::SameLine();
    DisabledScope next(state.category_page == last);
    if (ImGui::SmallButton("Next classes"))
      ++state.category_page;
  }
  state.object_visible_rows = 0;
  const auto begin = state.category_page * group_page_size,
             end = std::min(begin + group_page_size,
                            state.matching_categories.size());
  for (auto g = begin; g < end; ++g) {
    const auto gi = state.matching_categories[g];
    const auto &group = state.categories[gi];
    const auto &list = state.category_matches[gi];
    const auto cls = group.class_name.str();
    IdScope group_id("class:" + cls);
    bool reveal = false;
    if (state.reveal_object)
      for (auto oi : list)
        reveal |= same(group.objects[oi].key, *state.reveal_object);
    if (reveal || !query.empty())
      ImGui::SetNextItemOpen(true);
    if (!ImGui::TreeNodeEx(
            (clipped(cls, 160) + " (" + std::to_string(list.size()) + ")")
                .c_str(),
            ImGuiTreeNodeFlags_SpanAvailWidth))
      continue;
    auto &page = state.category_object_pages[cls];
    const auto last_object = (list.size() - 1) / object_page_size;
    page = std::min(page, last_object);
    if (last_object) {
      DisabledScope previous(!page);
      if (ImGui::SmallButton("Previous objects"))
        --page;
    }
    if (last_object) {
      ImGui::SameLine();
      DisabledScope next(page == last_object);
      if (ImGui::SmallButton("Next objects"))
        ++page;
    }
    const auto first = page * object_page_size,
               count = std::min(object_page_size, list.size() - first);
    if (ImGui::BeginTable(
            "##class-objects", 2,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp,
            {0, std::min(165.f, ImGui::GetFrameHeightWithSpacing() *
                                    static_cast<float>(count + 1))})) {
      ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("ID / generation",
                              ImGuiTableColumnFlags_WidthStretch);
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(count));
      while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
          const auto &object =
              group.objects[list[first + static_cast<std::size_t>(row)]];
          IdScope object_id("object:" + std::to_string(object.key.id));
          ++state.object_visible_rows;
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          const auto inspected = doc.inspected_object();
          if (ImGui::Selectable(clipped(object.name, 180).c_str(),
                                inspected && same(*inspected, object.key)))
            inspect(doc, state, object.key.id);
          tooltip(object.class_name + " | " + id_text(object.key.id));
          ImGui::TableNextColumn();
          text(id_text(object.key.id) + " / " +
               std::to_string(object.key.generation));
        }
      ImGui::EndTable();
    }
    ImGui::TreePop();
  }
}
} // namespace

std::string propertyValueText(const PropertyValue &value, std::size_t item,
                              const InspectorPreferences &preferences) {
  try {
    if (item >= value.item_count)
      return "<unavailable>";
    if (value.reference_ids) {
      if (item >= value.reference_ids->size())
        return "<unavailable>";
      return (*value.reference_ids)[item]
                 ? id_text((*value.reference_ids)[item])
                 : "None";
    }
    if (auto s = value.as_string(); s && item == 0)
      return clipped(*s, 4096);
    if (value.incoming_type.namespace_name == "physx3") {
      const auto &name = value.incoming_type.name;
      if (name == "PvdBool") {
        auto n = value.as_uint(item);
        return n ? (*n ? "True" : "False") : "<unavailable>";
      }
      if (auto n = value.as_uint(item))
        return name == "VoidPtr" ? id_text(*n) : std::to_string(*n);
      if (auto n = value.as_int(item))
        return std::to_string(*n);
      if (auto n = value.as_double(item))
        return numeric(*n, name == "PvdF64", preferences);
      if (auto n = value.as_vec3(item))
        return vec(*n, preferences);
      if (auto n = value.as_quat(item))
        return quat(*n, preferences);
      if (auto n = value.as_transform(item))
        return "P: " + vec(n->position, preferences) +
               "\nQ: " + quat(n->rotation, preferences);
      if (name == "PxVec2" || name == "PxVec4") {
        const auto components = name == "PxVec2" ? 2u : 4u;
        const auto stride = components * 4;
        if (item > std::numeric_limits<std::size_t>::max() / stride)
          return "<unavailable>";
        std::string s;
        for (unsigned j = 0; j < components; ++j) {
          if (j)
            s += ", ";
          s += numeric(raw_float(value, item * stride + j * 4), false,
                       preferences);
        }
        return s;
      }
      if (name == "ObjectRef")
        return "Unresolved internal reference";
      if (name == "PxBounds3") {
        if (item > std::numeric_limits<std::size_t>::max() / 24)
          return "<unavailable>";
        const auto base = item * 24;
        return "Min: " +
               vec({raw_float(value, base), raw_float(value, base + 4),
                    raw_float(value, base + 8)},
                   preferences) +
               "\nMax: " +
               vec({raw_float(value, base + 12), raw_float(value, base + 16),
                    raw_float(value, base + 20)},
                   preferences);
      }
      if (name == "PxMat33" || name == "PxMat44") {
        const std::size_t n = name == "PxMat33" ? 3 : 4, stride = n * n * 4;
        if (item > std::numeric_limits<std::size_t>::max() / stride)
          return "<unavailable>";
        std::string out;
        for (std::size_t row = 0; row < n; ++row) {
          if (row)
            out += '\n';
          for (std::size_t col = 0; col < n; ++col) {
            if (col)
              out += ", ";
            out +=
                numeric(raw_float(value, item * stride + (col * n + row) * 4),
                        false, preferences);
          }
        }
        return out;
      }
      if (name == "PvdColor" || name == "U32Array4") {
        const auto width = name == "PvdColor" ? 1u : 4u;
        const auto stride = width * 4;
        if (item > std::numeric_limits<std::size_t>::max() / stride)
          return "<unavailable>";
        std::string s;
        for (unsigned j = 0; j < 4; ++j) {
          if (j)
            s += ", ";
          s +=
              std::to_string(raw_uint(value, item * stride + j * width, width));
        }
        return s;
      }
    }
    const auto &bytes = value.raw_bytes();
    if (!value.item_count || bytes.size() % value.item_count)
      return "Opaque payload (" + std::to_string(bytes.size()) + " bytes)";
    const auto stride = bytes.size() / value.item_count;
    if (item > std::numeric_limits<std::size_t>::max() /
                   std::max<std::size_t>(stride, 1))
      return "<unavailable>";
    const auto start = item * stride;
    std::string out;
    char hex[4];
    for (std::size_t i = 0; i < std::min<std::size_t>(stride, 16); ++i) {
      std::snprintf(hex, sizeof(hex), "%02X", bytes[start + i]);
      if (i)
        out += ' ';
      out += hex;
    }
    if (stride > 16)
      out += " ...";
    return out.empty() ? "No captured data" : out;
  } catch (const std::exception &) {
    return "<incomplete or unavailable>";
  } catch (...) {
    return "<unavailable>";
  }
}

void drawEmptyState(const char *title, const char *description) {
  auto avail = ImGui::GetContentRegionAvail();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                       std::max(0.0f, (avail.y - 60.0f) * .35f));
  ImGui::TextUnformatted(title);
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", description);
  ImGui::PopStyleColor();
}

void drawHierarchy(Document &doc, PanelState &state) {
  state.object_visible_rows = 0;
  try {
    if (!doc.loaded()) {
      drawEmptyState("No capture loaded",
                     "Open a PVD recording to browse its scenes and objects.");
      return;
    }
    if (ImGui::SmallButton("Restore all visibility"))
      command(state, [&] { doc.show_all(); });
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##hierarchy-search", "Search objects by name, class or ID...",
        state.hierarchy_search.data(), state.hierarchy_search.size());
    ImGui::Spacing();
    const auto query = lower(state.hierarchy_search.data());
    const auto selection = doc.selected_key();
    const bool changed_selection =
        selection.has_value() != state.last_selection.has_value() ||
        (selection && state.last_selection &&
         !(*selection == *state.last_selection));
    if (changed_selection && selection)
      state.reveal_object = selection->shape;
    state.last_selection = selection;
    const auto scenes = doc.scenes();
    for (const auto &scene : scenes) {
      IdScope scene_id("scene:" + std::to_string(scene.key.id));
      const bool current = doc.scene_id() && *doc.scene_id() == scene.key.id;
      if (current && state.reveal_object)
        ImGui::SetNextItemOpen(true);
      else
        ImGui::SetNextItemOpen(current, ImGuiCond_Once);
      const auto label = clipped(scene_label(scene));
      const bool opened = ImGui::TreeNodeEx(
          label.c_str(), ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_SpanAvailWidth |
                             (current ? ImGuiTreeNodeFlags_Selected : 0));
      const bool clicked = ImGui::IsItemClicked();
      const bool toggled = ImGui::IsItemToggledOpen();
      if ((clicked && !toggled) || (opened && !current && toggled))
        command(state, [&] {
          if (!doc.set_scene(scene.key.id))
            throw std::runtime_error("This scene has no recorded frames.");
          doc.inspect(scene.key.id);
        });
      tooltip(scene_label(scene) + " | " + id_text(scene.key.id) + " | " +
              std::to_string(scene.native_frames) + " frames");
      if (!opened)
        continue;
      struct Pop {
        ~Pop() { ImGui::TreePop(); }
      } pop_scene;
      if (!doc.scene_id() || *doc.scene_id() != scene.key.id) {
        muted("Select this scene to browse objects.");
        continue;
      }
      auto scene_key = scene.key;
      if (const auto current_frame = doc.frame())
        if (const auto *current_scene =
                current_frame->snapshot.instance(scene.key.id))
          scene_key.generation = current_scene->generation;
      auto actors = doc.actor_tree();
      const auto selected = doc.selected_key();
      const auto inspected = doc.inspected_object();
      bool any = false;
      for (const auto &actor : actors) {
        bool actor_match = matches(actor.name + " " + actor.class_name + " " +
                                       id_text(actor.key.id) + " " +
                                       std::to_string(actor.key.id),
                                   query);
        std::vector<const ShapeNode *> shapes;
        for (const auto &shape : actor.shapes)
          if (actor_match || matches(shape.name + " " + shape.class_name + " " +
                                         id_text(shape.key.id) + " " +
                                         std::to_string(shape.key.id),
                                     query))
            shapes.push_back(&shape);
        if (!actor_match && shapes.empty())
          continue;
        any = true;
        IdScope actor_id("actor:" + std::to_string(actor.key.id));
        const bool active = inspected && same(*inspected, actor.key);
        bool reveal = !query.empty();
        if (state.reveal_object) {
          reveal = reveal || same(*state.reveal_object, actor.key);
          for (const auto &shape : actor.shapes)
            reveal = reveal || same(*state.reveal_object, shape.key) ||
                     (shape.geometry_id &&
                      *shape.geometry_id == state.reveal_object->id);
        }
        if (reveal)
          ImGui::SetNextItemOpen(true);
        const auto actor_label = clipped(
            actor.name.empty() ? actor.class_name + " " + id_text(actor.key.id)
                               : actor.name);
        const bool actor_open = ImGui::TreeNodeEx(
            actor_label.c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                (active ? ImGuiTreeNodeFlags_Selected : 0) |
                (shapes.empty() ? ImGuiTreeNodeFlags_Leaf : 0));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
          inspect(doc, state, actor.key.id);
        actor_menu(doc, state, scene_key, actor.key);
        tooltip(actor.class_name + " | " + id_text(actor.key.id));
        if (!actor_open)
          continue;
        struct PopActor {
          ~PopActor() { ImGui::TreePop(); }
        } pop_actor;
        for (const auto *shape : shapes) {
          IdScope shape_id("shape:" + std::to_string(shape->key.id));
          const PickingKey key{scene_key, actor.key, shape->key};
          const bool shape_active = (selected && *selected == key) ||
                                    (inspected && same(*inspected, shape->key));
          const auto shape_label =
              clipped(shape->name.empty()
                          ? shape->class_name + " " + id_text(shape->key.id)
                          : shape->name);
          ImGui::TreeNodeEx(
              shape_label.c_str(),
              ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                  ImGuiTreeNodeFlags_SpanAvailWidth |
                  (shape_active ? ImGuiTreeNodeFlags_Selected : 0));
          const bool shape_clicked = ImGui::IsItemClicked();
          shape_menu(doc, state, key);
          if (shape_clicked)
            command(state, [&] {
              if (!doc.select(key) && !doc.inspect(shape->key.id))
                throw std::runtime_error(
                    "This shape is not present at the current frame.");
              state.reveal_object = shape->key;
            });
          tooltip(shape->class_name + " | " + id_text(shape->key.id) +
                  (shape->present ? "" : " | Not present at this frame") +
                  (doc.visible(key) ? " | Visible"
                                    : " | Hidden, filtered or no geometry"));
        }
      }
      if (!any)
        muted(query.empty() ? "No objects at this frame."
                            : "No matching objects.");
    }
    object_categories(doc, state, query);
    state.reveal_object.reset();
    panel_error(state);
  } catch (const std::exception &e) {
    state.error = e.what();
    panel_error(state);
  }
}

void drawInspector(Document &doc, PanelState &state) {
  try {
    state.array_visible_rows = 0;
    if (ImGui::SmallButton("Inspector settings"))
      ImGui::OpenPopup("Inspector preferences");
    ImGui::SetNextWindowSizeConstraints(
        {280, 0},
        {std::max(280.f, std::min(440.f, ImGui::GetIO().DisplaySize.x - 16)),
         std::numeric_limits<float>::max()});
    if (ImGui::BeginPopup("Inspector preferences")) {
      auto &preferences = state.inspector_preferences;
      bool changed = false;
      int format = static_cast<int>(preferences.number_format);
      ImGui::SetNextItemWidth(180);
      if (ImGui::Combo("Number format", &format,
                       "General\0Scientific\0Fixed\0")) {
        preferences.number_format = static_cast<NumberFormat>(format);
        changed = true;
      }
      int fp = static_cast<int>(preferences.float_precision),
          dp = static_cast<int>(preferences.double_precision);
      ImGui::SetNextItemWidth(180);
      if (ImGui::SliderInt("Float precision", &fp, 1, 17)) {
        preferences.float_precision = static_cast<unsigned>(fp);
        changed = true;
      }
      ImGui::SetNextItemWidth(180);
      if (ImGui::SliderInt("Double precision", &dp, 1, 17)) {
        preferences.double_precision = static_cast<unsigned>(dp);
        changed = true;
      }
      ImGui::TextWrapped(
          "General uses significant digits; scientific/fixed use decimal "
          "places. Defaults (float 9, double 17) retain round-trip precision "
          "in General. Integer values and IDs are always exact.");
      const std::size_t groups[] = {32, 128, 512, 2048};
      int group = 0;
      for (int i = 0; i < 4; ++i)
        if (preferences.array_page_size == groups[i])
          group = i;
      ImGui::SetNextItemWidth(180);
      if (ImGui::Combo("Array group size", &group,
                       "32\0"
                       "128\0"
                       "512\0"
                       "2048\0")) {
        preferences.array_page_size = groups[group];
        changed = true;
      }
      ImGui::TextWrapped("Only one array group is displayed at a time; visible "
                         "rows are clipped even for 2048-item groups.");
      if (ImGui::Button("Reset defaults")) {
        preferences = {};
        changed = true;
      }
      if (changed)
        state.array_pages.clear();
      ImGui::EndPopup();
    }
    if (!valid_inspector_preferences(state.inspector_preferences))
      throw std::invalid_argument(
          "Inspector display preferences are outside supported limits");
    if (!doc.loaded()) {
      drawEmptyState("Inspector",
                     "Object properties appear after a capture is opened.");
      return;
    }
    if (auto key = doc.selected_key()) {
      role_button(doc, state, "Shape", key->shape);
      ImGui::SameLine();
      role_button(doc, state, "Actor", key->actor);
      ImGui::SameLine();
      role_button(doc, state, "Scene", key->scene);
      ImGui::Spacing();
    }
    auto frame = doc.frame();
    const auto *object = doc.inspected_instance();
    if (!object) {
      drawEmptyState("Nothing selected", "Choose an object in the hierarchy or "
                                         "click a shape in the viewport.");
      panel_error(state);
      return;
    }
    const auto name = object_name(object);
    text(name);
    muted(object->class_name.str());
    tooltip(object->class_name.str());
    muted(id_text(object->id));
    ImGui::Spacing();
    if (ImGui::SmallButton("Copy ID"))
      ImGui::SetClipboardText(id_text(object->id).c_str());
    tooltip("Copy the full captured object ID");
    ImGui::SameLine();
    muted(std::to_string(object->properties.size()) + " properties");
    ImGui::Separator();
    ImGui::Spacing();
    properties(doc, state, *object);
    panel_error(state);
  } catch (const std::exception &e) {
    state.error = e.what();
    panel_error(state);
  }
}

void drawTimeline(Document &doc, PanelState &state) {
  try {
    if (!doc.loaded()) {
      muted("Open a capture to inspect its recorded frames.");
      return;
    }
    std::string current_scene = "Select scene";
    for (const auto &scene : doc.scenes())
      if (doc.scene_id() && scene.key.id == *doc.scene_id())
        current_scene = scene_label(scene);
    const auto width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width > 850 ? 180.0f : 150.0f);
    if (ImGui::BeginCombo("##scene", clipped(current_scene).c_str())) {
      for (const auto &scene : doc.scenes()) {
        IdScope id(std::to_string(scene.key.id));
        if (ImGui::Selectable(clipped(scene_label(scene)).c_str(),
                              doc.scene_id() &&
                                  *doc.scene_id() == scene.key.id))
          command(state, [&] { doc.set_scene(scene.key.id); });
      }
      ImGui::EndCombo();
    }
    tooltip("Recorded scene");
    ImGui::SameLine();
    const bool after = doc.sample_point() == SamplePoint::after_fetch;
    ImGui::SetNextItemWidth(width > 850 ? 150.0f : 135.0f);
    if (ImGui::BeginCombo("##sample-point",
                          after ? "After simulation" : "Frame end")) {
      if (ImGui::Selectable("Frame end", !after))
        command(state,
                [&] { doc.set_sample_point(SamplePoint::native_frame); });
      if (ImGui::Selectable("After simulation", after))
        command(state, [&] {
          if (!doc.set_sample_point(SamplePoint::after_fetch))
            throw std::runtime_error(
                "No after-simulation markers are available for this scene.");
        });
      ImGui::EndCombo();
    }
    tooltip("Frame end uses the simulation's recorded frame boundary. After "
            "simulation uses an explicit post-step marker when available.");
    const auto total = doc.timeline().size();
    // Keep the fixed-height host panel to three rows at the supported window
    // sizes.
    if (width > 850)
      ImGui::SameLine(0, 16);
    {
      DisabledScope disabled(total == 0);
      if (ImGui::Button("First"))
        command(state, [&] { doc.set_frame(0); });
      ImGui::SameLine();
      if (ImGui::Button("<"))
        command(state, [&] { doc.step(-1); });
      ImGui::SameLine();
      if (ImGui::Button(doc.playback().playing ? "Pause" : "Play"))
        command(state, [&] { doc.set_playing(!doc.playback().playing); });
      ImGui::SameLine();
      if (ImGui::Button(">"))
        command(state, [&] { doc.step(1); });
      ImGui::SameLine();
      if (ImGui::Button("Last"))
        command(state, [&] { doc.set_frame(total - 1); });
      ImGui::SameLine();
      bool loop = doc.playback().loop;
      if (ImGui::Checkbox("Loop", &loop))
        command(state, [&] {
          auto p = doc.playback();
          p.loop = loop;
          doc.set_playback(p);
        });
      if (width > 500)
        ImGui::SameLine();
      bool reverse = doc.playback().frames_per_second < 0;
      if (ImGui::Checkbox("Reverse", &reverse))
        command(state, [&] {
          auto p = doc.playback();
          p.frames_per_second =
              std::abs(p.frames_per_second) * (reverse ? -1 : 1);
          doc.set_playback(p);
        });
      if (width > 900) {
        ImGui::SameLine();
        float fps =
            static_cast<float>(std::abs(doc.playback().frames_per_second));
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragFloat("fps", &fps, .25f, 1, 240, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp))
          command(state, [&] {
            auto p = doc.playback();
            p.frames_per_second = fps * (p.frames_per_second < 0 ? -1 : 1);
            doc.set_playback(p);
          });
      }
    }
    if (total) {
      auto index = static_cast<std::uint64_t>(doc.frame_index() + 1);
      const std::uint64_t low = 1, high = total;
      ImGui::SetNextItemWidth(-1);
      if (ImGui::SliderScalar("##frame", ImGuiDataType_U64, &index, &low, &high,
                              "Frame %llu", ImGuiSliderFlags_AlwaysClamp))
        command(state,
                [&] { doc.set_frame(static_cast<std::size_t>(index - 1)); });
      ImGui::Text("Frame %llu / %llu",
                  static_cast<unsigned long long>(doc.frame_index() + 1),
                  static_cast<unsigned long long>(total));
      ImGui::SameLine();
      muted(after ? "After simulation" : "Frame end");
      auto frame = doc.frame();
      if (frame && frame->marker)
        if (auto ms = interval_ms(doc, *frame->marker)) {
          ImGui::SameLine();
          muted("Interval " + real(*ms) + " ms");
          tooltip(
              "Captured marker interval, not an inferred simulation timestep.");
        }
      if (width <= 900) {
        if (width > 650)
          ImGui::SameLine();
        float fps =
            static_cast<float>(std::abs(doc.playback().frames_per_second));
        ImGui::SetNextItemWidth(65);
        if (ImGui::DragFloat("fps", &fps, .25f, 1, 240, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp))
          command(state, [&] {
            auto p = doc.playback();
            p.frames_per_second = fps * (p.frames_per_second < 0 ? -1 : 1);
            doc.set_playback(p);
          });
      }
      if (!state.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(.98f, .52f, .45f, 1), "Action unavailable");
        tooltip(state.error);
      }
    } else
      muted("No frames are available at this sample point.");
  } catch (const std::exception &e) {
    state.error = e.what();
    panel_error(state);
  }
}

void drawDiagnostics(Document &doc, PanelState &state) {
  try {
    panel_error(state);
    auto frame = doc.frame();
    if (!frame) {
      muted("No capture diagnostics.");
      return;
    }
    const auto total = frame->errors.size() + frame->render.diagnostics.size() +
                       frame->debug.diagnostics.size();
    if (!total) {
      muted("No issues at this frame.");
      return;
    }
    ImGui::Text("%llu messages", static_cast<unsigned long long>(total));
    ImGui::Separator();
    for (const auto &error : frame->errors) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.98f, .52f, .45f, 1));
      ImGui::TextWrapped("%s", clipped(error, 2048).c_str());
      ImGui::PopStyleColor();
      ImGui::Spacing();
    }
    for (const auto &d : frame->render.diagnostics) {
      IdScope id("r:" + std::to_string(&d - frame->render.diagnostics.data()));
      ImGui::PushStyleColor(ImGuiCol_Text, d.severity == render::Severity::error
                                               ? ImVec4(.98f, .52f, .45f, 1)
                                               : ImVec4(.88f, .75f, .44f, 1));
      ImGui::TextWrapped("%s", clipped(d.message, 2048).c_str());
      ImGui::PopStyleColor();
      if (d.object_id) {
        if (ImGui::SmallButton(id_text(d.object_id).c_str()))
          inspect(doc, state, d.object_id);
        ImGui::SameLine();
        muted(d.property);
      }
      ImGui::Spacing();
    }
    for (const auto &d : frame->debug.diagnostics) {
      IdScope id("d:" + std::to_string(&d - frame->debug.diagnostics.data()));
      ImGui::TextWrapped("%s", clipped(d.message, 2048).c_str());
      if (d.channel_instance_id) {
        if (ImGui::SmallButton(id_text(d.channel_instance_id).c_str()))
          inspect(doc, state, d.channel_instance_id);
      }
      ImGui::Spacing();
    }
  } catch (const std::exception &e) {
    state.error = e.what();
    panel_error(state);
  }
}
} // namespace pvd::viewer
