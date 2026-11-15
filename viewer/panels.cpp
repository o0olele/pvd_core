#include "panels.hpp"
#include "ui_scale.hpp"
#include "ui_widgets.hpp"
#include "pvd/coordinates.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

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
std::string integer_text(std::uint64_t n, const InspectorPreferences &p) {
  return p.integer_hex ? id_text(n) : std::to_string(n);
}
std::string integer_text(std::int64_t n, const InspectorPreferences &p) {
  if (!p.integer_hex)
    return std::to_string(n);
  return n < 0 ? "-" + id_text(std::uint64_t(0) - static_cast<std::uint64_t>(n))
               : id_text(static_cast<std::uint64_t>(n));
}
std::string named_integer(std::uint64_t n, const InspectorPreferences &p,
                          const PropertyDefinition *definition) {
  const auto raw = integer_text(n, p);
  if (!definition || definition->named_values.empty())
    return raw;
  const bool flags = definition->semantic == "Bitflag";
  if (!flags && definition->semantic != "Enumeration Value")
    return raw;
  std::map<std::uint32_t, std::string> names;
  for (const auto &entry : definition->named_values) {
    auto &name = names[entry.second];
    if (!name.empty())
      name += "/";
    name += entry.first;
  }
  if (n <= std::numeric_limits<std::uint32_t>::max())
    if (auto exact = names.find(static_cast<std::uint32_t>(n));
        exact != names.end())
      return exact->second + " (" + raw + ")";
  if (!flags)
    return "Unknown (" + raw + ")";
  std::uint64_t remaining = n;
  std::string symbolic;
  const auto consume = [&](const auto &entry) {
    const auto bits = static_cast<std::uint64_t>(entry.first);
    if (!bits || (remaining & bits) != bits)
      return;
    if (!symbolic.empty())
      symbolic += " | ";
    symbolic += entry.second;
    remaining &= ~bits;
  };
  // Prefer individual bits; composite aliases are used only for residual bits.
  for (const auto &entry : names)
    if (entry.first && !(entry.first & (entry.first - 1)))
      consume(entry);
  for (const auto &entry : names)
    if (entry.first & (entry.first - 1))
      consume(entry);
  if (remaining) {
    if (!symbolic.empty())
      symbolic += " | ";
    symbolic += "Unknown bits " + id_text(remaining);
  }
  if (symbolic.empty())
    symbolic = "None";
  return symbolic + " (" + raw + ")";
}
void copy_text(PanelState &state, const std::string &value) {
  if (state.copy_text)
    state.copy_text(value);
  else
    ImGui::SetClipboardText(value.c_str());
}
void property_copy_menu(PanelState &state, const PropertyValue &value,
                        std::size_t item,
                        const PropertyDefinition *definition) {
  if (!ui::begin_popup_context_item("##property-copy"))
    return;
  if (ImGui::MenuItem("Copy native value"))
    command(state, [&] {
      copy_text(state, propertyValueCopyText(value, item,
                  PropertyCopy::native_value, state.inspector_preferences,
                  definition));
    });
  if (ImGui::MenuItem("Copy last wire value"))
    command(state, [&] {
      copy_text(state, propertyValueCopyText(value, item,
                  PropertyCopy::last_wire_value, state.inspector_preferences,
                  definition));
    });
  if (ImGui::MenuItem("Copy wire bytes"))
    command(state, [&] {
      copy_text(state, propertyValueCopyText(value, item,
                  PropertyCopy::wire_bytes, state.inspector_preferences,
                  definition));
    });
  ImGui::Separator();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui_pixels(340));
  ImGui::TextWrapped("Copy uses the full selected element, not its row preview. "
      "Native coordinates include OriginShift; last wire values and bytes "
      "retain their incoming coordinate epoch. Decoded floats use float 9 / "
      "double 17 General precision.");
  ImGui::PopTextWrapPos();
  ui::end_popup();
}
void property_value(Document &doc, PanelState &state,
                    const PropertyValue &value, std::size_t item,
                    bool compact = false,
                    const PropertyDefinition *definition = nullptr) {
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
      propertyValueText(value, item, state.inspector_preferences, false,
                        definition);
  auto display = summary;
  if (compact) {
    std::replace(display.begin(), display.end(), '\n', ' ');
    std::replace(display.begin(), display.end(), '\r', ' ');
  }
  text(display + (value.native_coordinate_bytes ? "  [native]" : ""));
  auto detail = clipped(summary, 1024) + "\n" +
                std::to_string(value.raw_bytes().size()) + " captured bytes";
  if (value.native_coordinate_bytes)
    detail += "\nNative coordinates include recorded OriginShift.\nLast wire value: " +
              propertyValueText(value, item, state.inspector_preferences, true,
                                definition);
  tooltip(detail);
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
    else {
      property_value(doc, state, value, 0, false, definition);
      property_copy_menu(state, value, 0, definition);
    }
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
          else {
            property_value(doc, state, value, static_cast<std::size_t>(item),
                           true, definition);
            property_copy_menu(state, value, static_cast<std::size_t>(item),
                               definition);
          }
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
  if (!ui::begin_popup_context_item("##shape-actions"))
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
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui_pixels(300));
  muted("Show removes manual hide; category filters still apply.");
  ImGui::PopTextWrapPos();
  ui::end_popup();
}
void actor_menu(Document &doc, PanelState &state, ObjectKey scene,
                ObjectKey actor) {
  if (!ui::begin_popup_context_item("##actor-actions"))
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
  ui::end_popup();
}
const std::string *category_name(const InstanceState &object) {
  for (const auto *key : {"Name", "name"})
    if (const auto *value = object.property(key);
        value && value->text && !value->text->empty())
      return &*value->text;
  return nullptr;
}
struct CategoryChange {
  std::uint64_t id;
  const InstanceState *previous, *current;
};
std::vector<CategoryChange> category_changes(const SnapshotView &before,
                                            const SnapshotView &after) {
  std::vector<CategoryChange> result;
  auto old = before.instances.begin(), now = after.instances.begin();
  while (old != before.instances.end() || now != after.instances.end()) {
    if (now == after.instances.end() ||
        (old != before.instances.end() && old->first < now->first)) {
      result.push_back({old->first, &old->second, nullptr}); ++old;
    } else if (old == before.instances.end() || now->first < old->first) {
      result.push_back({now->first, nullptr, &now->second}); ++now;
    } else {
      if (&old->second != &now->second) {
        const auto *a = category_name(old->second), *b = category_name(now->second);
        if (old->second.generation != now->second.generation ||
            old->second.class_name != now->second.class_name ||
            bool(a) != bool(b) || (a && *a != *b))
          result.push_back({now->first, &old->second, &now->second});
      }
      ++old; ++now;
    }
  }
  return result;
}
std::set<QualifiedName> update_category_groups(PanelState &state,
                                              const std::vector<CategoryChange> &changes) {
  std::map<QualifiedName, std::vector<ObjectNode>> replacements;
  for (const auto &change : changes) {
    if (change.previous) replacements.try_emplace(change.previous->class_name);
    if (change.current) replacements.try_emplace(change.current->class_name);
  }
  const auto locate = [&](const QualifiedName &name) {
    return std::lower_bound(state.categories.begin(), state.categories.end(), name,
        [](const auto &group, const auto &key) { return group.class_name < key; });
  };
  for (auto &[cls, objects] : replacements) {
    const auto found = locate(cls);
    if (found != state.categories.end() && found->class_name == cls) {
      objects.reserve(found->objects.size());
      for (const auto &object : found->objects) {
        const auto changed = std::lower_bound(changes.begin(), changes.end(), object.key.id,
            [](const auto &change, auto id) { return change.id < id; });
        if (changed == changes.end() || changed->id != object.key.id)
          objects.push_back(object);
      }
    }
    for (const auto &change : changes)
      if (change.current && change.current->class_name == cls) {
        const auto &object = *change.current;
        const auto *label = category_name(object);
        objects.push_back({{object.id, object.generation},
            label ? *label : object.class_name.name + " " + std::to_string(object.id),
            object.class_name.str()});
      }
    std::sort(objects.begin(), objects.end(),
        [](const auto &a, const auto &b) { return a.key.id < b.key.id; });
  }
  state.categories.reserve(state.categories.size() + replacements.size());
  state.category_matches.reserve(state.categories.size() + replacements.size());
  std::set<QualifiedName> dirty;
  for (auto &[cls, objects] : replacements) {
    dirty.insert(cls);
    auto found = locate(cls);
    const auto index = static_cast<std::size_t>(found - state.categories.begin());
    const bool exists = found != state.categories.end() && found->class_name == cls;
    if (objects.empty()) {
      if (exists) {
        state.categories.erase(found);
        state.category_matches.erase(state.category_matches.begin() + index);
      }
    } else if (exists) {
      found->objects = std::move(objects);
      state.category_matches[index].clear();
    } else {
      state.categories.insert(found, ObjectGroup{cls, std::move(objects)});
      state.category_matches.insert(state.category_matches.begin() + index, std::vector<std::size_t>{});
    }
    state.category_object_pages.erase(cls.str());
  }
  return dirty;
}
void object_categories(Document &doc, PanelState &state,
                       const std::string &query) {
  if (state.reveal_object || !query.empty())
    ImGui::SetNextItemOpen(true);
  if (!ImGui::CollapsingHeader("All objects by class (all scenes)"))
    return;
  const auto frame = doc.frame();
  const auto prefix = frame->snapshot.event_count;
  const bool refresh = state.category_source != doc.session() || !state.category_snapshot;
  std::set<QualifiedName> dirty;
  try {
  if (!refresh && (state.category_prefix != prefix || state.category_frame.lock() != frame))
    dirty = update_category_groups(state, category_changes(*state.category_snapshot, frame->snapshot));
  if (refresh) {
    state.categories = doc.object_groups();
    state.category_source = doc.session();
    state.category_page = 0;
    state.category_object_pages.clear();
  }
  state.category_prefix = prefix;
  state.category_snapshot = frame->snapshot;
  state.category_frame = frame;
  const bool rematch_all = refresh || state.category_query != query;
  if (rematch_all || !dirty.empty()) {
    state.category_query = query;
    state.matching_categories.clear();
    if (rematch_all) state.category_matches.clear();
    state.category_matches.resize(state.categories.size());
    state.object_count = 0;
    for (std::size_t gi = 0; gi < state.categories.size(); ++gi) {
      const auto &group = state.categories[gi];
      auto &list = state.category_matches[gi];
      if (rematch_all || dirty.count(group.class_name)) {
      list.clear();
      const bool class_match = matches(group.class_name.str(), query);
      for (std::size_t oi = 0; oi < group.objects.size(); ++oi) {
        const auto &object = group.objects[oi];
        if (class_match || matches(object.name + " " + id_text(object.key.id) +
                                       " " + std::to_string(object.key.id),
                                   query))
          list.push_back(oi);
      }
      }
      if (!list.empty()) {
        state.matching_categories.push_back(gi);
        state.object_count += list.size();
      }
    }
  }
  } catch (...) {
    state.category_source = nullptr;
    state.category_snapshot.reset();
    throw;
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
            {0, std::min(ui_pixels(165), ImGui::GetFrameHeightWithSpacing() *
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

namespace {
std::string format_property(const PropertyValue &value, std::size_t item,
                            const InspectorPreferences &preferences,
                            bool last_wire_value,
                            const PropertyDefinition *definition,
                            bool complete) {
  if (!valid_inspector_preferences(preferences))
    throw std::invalid_argument("invalid Inspector preferences");
  if (!last_wire_value && value.native_coordinate_bytes)
    return format_property(coordinates::native_value(value), item, preferences,
                           true, definition, complete);
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
    if (auto s = value.as_string(); s && item == 0) {
      if (!complete)
        return clipped(*s, 4096);
      // ImGui's UTF-8 clipboard is NUL-terminated. Keep embedded NUL visible
      // instead of silently losing the rest of the captured string.
      std::string out;
      for (char c : *s)
        if (c == '\0')
          out += "\\u0000";
        else
          out += c;
      return out;
    }
    if (value.incoming_type.namespace_name == "physx3") {
      const auto &name = value.incoming_type.name;
      if (name == "PvdBool") {
        auto n = value.as_uint(item);
        return n ? (*n ? "True" : "False") : "<unavailable>";
      }
      if (auto n = value.as_uint(item))
        return name == "VoidPtr" ? id_text(*n)
                                 : named_integer(*n, preferences, definition);
      if (auto n = value.as_int(item))
        return integer_text(*n, preferences);
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
          s += integer_text(raw_uint(value, item * stride + j * width, width),
                            preferences);
        }
        return s;
      }
    }
    const auto &bytes = value.raw_bytes();
    if (!value.item_count || bytes.size() % value.item_count) {
      if (complete)
        throw std::invalid_argument("opaque element byte boundaries unavailable");
      return "Opaque payload (" + std::to_string(bytes.size()) + " bytes)";
    }
    const auto stride = bytes.size() / value.item_count;
    if (item > std::numeric_limits<std::size_t>::max() /
                   std::max<std::size_t>(stride, 1))
      return "<unavailable>";
    const auto start = item * stride;
    std::string out;
    char hex[4];
    for (std::size_t i = 0; i < (complete ? stride : std::min<std::size_t>(stride, 16)); ++i) {
      std::snprintf(hex, sizeof(hex), "%02X", bytes[start + i]);
      if (i)
        out += ' ';
      out += hex;
    }
    if (!complete && stride > 16)
      out += " ...";
    return out.empty() ? "No captured data" : out;
  } catch (const std::exception &) {
    if (complete)
      throw;
    return "<incomplete or unavailable>";
  } catch (...) {
    if (complete)
      throw;
    return "<unavailable>";
  }
}
} // namespace

std::string propertyValueText(const PropertyValue &value, std::size_t item,
                              const InspectorPreferences &preferences,
                              bool last_wire_value,
                              const PropertyDefinition *definition) {
  if (!valid_inspector_preferences(preferences))
    return "<invalid display preferences>";
  try {
    return format_property(value, item, preferences, last_wire_value, definition,
                           false);
  } catch (...) {
    return "<unavailable>";
  }
}

std::string propertyValueCopyText(const PropertyValue &value, std::size_t item,
                                  PropertyCopy representation,
                                  const InspectorPreferences &preferences,
                                  const PropertyDefinition *definition) {
  if (item >= value.item_count)
    throw std::out_of_range("selected property element is unavailable");
  if (representation == PropertyCopy::wire_bytes) {
    const auto &bytes = value.raw_bytes();
    if (!value.item_count || bytes.size() % value.item_count)
      throw std::invalid_argument("wire element byte boundaries unavailable");
    const auto stride = bytes.size() / value.item_count;
    if (item > std::numeric_limits<std::size_t>::max() /
                   std::max<std::size_t>(stride, 1))
      throw std::out_of_range("wire element offset exceeds host size");
    const auto start = item * stride;
    std::string result;
    if (stride > result.max_size() / 3)
      throw std::length_error("wire element is too large to copy");
    char hex[4];
    for (std::size_t i = 0; i < stride; ++i) {
      if (i)
        result += ' ';
      std::snprintf(hex, sizeof(hex), "%02X", bytes[start + i]);
      result += hex;
    }
    return result;
  }
  if (representation != PropertyCopy::native_value &&
      representation != PropertyCopy::last_wire_value)
    throw std::invalid_argument("unsupported property copy representation");
  InspectorPreferences exact;
  exact.integer_hex = preferences.integer_hex;
  auto result = format_property(value, item, exact,
                    representation == PropertyCopy::last_wire_value,
                    definition, true);
  if (!value.text && (result == "<unavailable>" ||
      result == "Unresolved internal reference"))
    throw std::invalid_argument("selected property element is unavailable");
  return result;
}

void drawEmptyState(const char *title, const char *description) {
  auto avail = ImGui::GetContentRegionAvail();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                       std::max(0.0f, (avail.y - ui_pixels(60)) * .35f));
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
    auto frame = doc.frame();
    const auto *object = doc.loaded() ? doc.inspected_instance() : nullptr;
    const float heading_x = ImGui::GetCursorPosX();
    const float heading_width = ImGui::GetContentRegionAvail().x;
    const float settings_width = ui_pixels(24);
    const float title_width = std::max(
        1.f, heading_width - settings_width - ImGui::GetStyle().ItemSpacing.x);
    if (object) {
      const auto name = object_name(object);
      auto title = name;
      if (ImGui::CalcTextSize(title.c_str()).x > title_width) {
        const char *end = title.c_str();
        ImGui::GetFont()->CalcTextSizeA(
            ImGui::GetFontSize(),
            std::max(1.f, title_width - ImGui::CalcTextSize("...").x), 0,
            title.c_str(), nullptr, &end);
        title = std::string(title.c_str(), end) + "...";
      }
      text(title);
      tooltip(name);
    } else
      ImGui::Dummy({title_width, ImGui::GetTextLineHeight()});
    ImGui::SameLine(heading_x + heading_width - settings_width);
    if (ui::icon_button("Settings##Inspector settings", ui::Icon::settings,
                         "Number format and precision", false, 24)) {
      ui::anchor_popup_to_item();
      ImGui::OpenPopup("Inspector preferences");
    }
    tooltip("Inspector settings: number format, precision and array groups");
    ImGui::SetNextWindowSizeConstraints(
        {ui_pixels(280), 0},
        {std::max(ui_pixels(280), std::min(ui_pixels(440), ImGui::GetIO().DisplaySize.x - ui_pixels(16))),
         std::numeric_limits<float>::max()});
    if (ui::begin_popup("Inspector preferences")) {
      ImGui::TextUnformatted("Inspector settings");
      ImGui::Separator();
      auto &preferences = state.inspector_preferences;
      bool changed = false;
      ImGui::TextDisabled("Numeric display");
      int format = static_cast<int>(preferences.number_format);
      ImGui::SetNextItemWidth(ui_pixels(180));
      if (ImGui::Combo("Number format", &format,
                       "General\0Scientific\0Fixed\0")) {
        preferences.number_format = static_cast<NumberFormat>(format);
        changed = true;
      }
      if (ImGui::Checkbox("Integer hexadecimal", &preferences.integer_hex))
        changed = true;
      ImGui::Spacing();
      ImGui::TextDisabled("Precision");
      int fp = static_cast<int>(preferences.float_precision),
          dp = static_cast<int>(preferences.double_precision);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                          {ui_pixels(10), ui_pixels(2)});
      ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, ui_pixels(5));
      ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, ui_pixels(3));
      ImGui::PushStyleColor(ImGuiCol_SliderGrab, {.31f, .43f, .48f, 1});
      ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {.36f, .64f, .58f, 1});
      ImGui::SetNextItemWidth(ui_pixels(180));
      if (ImGui::SliderInt("Float precision", &fp, 1, 17)) {
        preferences.float_precision = static_cast<unsigned>(fp);
        changed = true;
      }
      ImGui::SetNextItemWidth(ui_pixels(180));
      if (ImGui::SliderInt("Double precision", &dp, 1, 17)) {
        preferences.double_precision = static_cast<unsigned>(dp);
        changed = true;
      }
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(3);
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s",
          "General uses significant digits; scientific/fixed use decimal "
          "places. Defaults (float 9, double 17) retain round-trip precision "
          "in General. Integer values and IDs are always exact.");
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Arrays");
      const std::size_t groups[] = {32, 128, 512, 2048};
      int group = 0;
      for (int i = 0; i < 4; ++i)
        if (preferences.array_page_size == groups[i])
          group = i;
      ImGui::SetNextItemWidth(ui_pixels(180));
      if (ImGui::Combo("Array group size", &group,
                       "32\0"
                       "128\0"
                       "512\0"
                       "2048\0")) {
        preferences.array_page_size = groups[group];
        changed = true;
      }
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", "One group is displayed at a time. Rows stay "
                          "clipped even for 2048-item groups.");
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      ImGui::Separator();
      if (ImGui::Button("Reset defaults")) {
        preferences = {};
        changed = true;
      }
      if (changed)
        state.array_pages.clear();
      ui::end_popup();
    }
    if (!valid_inspector_preferences(state.inspector_preferences))
      throw std::invalid_argument(
          "Inspector display preferences are outside supported limits");
    if (!doc.loaded()) {
      drawEmptyState("Inspector",
                     "Object properties appear after a capture is opened.");
      return;
    }
    if (!object) {
      drawEmptyState("Nothing selected", "Choose an object in the hierarchy or "
                                         "click a shape in the viewport.");
      panel_error(state);
      return;
    }
    const auto object_id = id_text(object->id);
    {
      TableScope identity("##inspector-identity", 2,
                           ImGuiTableFlags_SizingStretchProp);
      if (identity.open) {
        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "ID", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize(object_id.c_str()).x +
                ImGui::GetStyle().FramePadding.x * 2);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        muted(object->class_name.str());
        tooltip(object->class_name.str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton((object_id + "##copy-object-id").c_str()))
          command(state, [&] { copy_text(state, object_id); });
        tooltip("Copy the full captured object ID");
      }
    }
    if (auto key = doc.selected_key()) {
      role_button(doc, state, "Actor", key->actor);
      ImGui::SameLine();
      role_button(doc, state, "Shape", key->shape);
      ImGui::SameLine();
      role_button(doc, state, "Scene", key->scene);
    }
    ImGui::Spacing();
    ImGui::Separator();

    // These are views of captured properties, not synthesized rigid-body
    // defaults. Keep the same formatting, native-coordinate handling and exact
    // copy commands as the raw table; unknown classes always retain that table.
    struct SummaryField {
      const char *name;
      const char *alias;
      const char *label;
    };
    const auto *metadata = doc.metadata();
    const auto summary_value = [&](const SummaryField &field) {
      const auto *value = object->property(field.name);
      const char *property_name = field.name;
      if (!value && field.alias) {
        value = object->property(field.alias);
        property_name = field.alias;
      }
      const auto *definition = metadata
          ? metadata->find_property(object->class_name, property_name)
          : nullptr;
      if (value && (value->item_count != 1 ||
                    (definition && definition->property_type == 2)))
        value = nullptr;
      return std::make_pair(property_name, value);
    };
    const auto summary_group = [&](const char *heading,
                                   std::initializer_list<SummaryField> fields) {
      const bool available = std::any_of(
          fields.begin(), fields.end(),
          [&](const SummaryField &field) { return summary_value(field).second; });
      if (!available)
        return false;
      ImGui::Spacing();
      ImGui::TextUnformatted(heading);
      const std::string table_id = std::string("##inspector-summary-") + heading;
      TableScope overview(table_id.c_str(), 2,
                           ImGuiTableFlags_SizingStretchProp);
      if (!overview.open)
        return true;
      ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch,
                              .85f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch,
                              1.3f);
      for (const auto &field : fields) {
        const auto entry = summary_value(field);
        if (!entry.second)
          continue;
        const auto *definition = metadata
            ? metadata->find_property(object->class_name, entry.first)
            : nullptr;
        IdScope field_id(entry.first);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        muted(field.label);
        tooltip(entry.first);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0);
        property_value(doc, state, *entry.second, 0, false, definition);
        ImGui::PopTextWrapPos();
        property_copy_menu(state, *entry.second, 0, definition);
      }
      return true;
    };
    bool has_summary = false;
    has_summary |= summary_group("Transform", {
        {"GlobalPose", "globalPose", "Global pose"},
        {"LocalPose", "localPose", "Local pose"},
        {"CMassLocalPose", "cMassLocalPose", "Mass frame"}});
    has_summary |= summary_group("Dynamics", {
        {"Mass", "mass", "Mass"},
        {"LinearVelocity", "linearVelocity", "Linear velocity"},
        {"AngularVelocity", "angularVelocity", "Angular velocity"},
        {"IsSleeping", "isSleeping", "Sleeping"}});
    has_summary |= summary_group("Collision", {
        {"ContactOffset", "contactOffset", "Contact offset"},
        {"RestOffset", "restOffset", "Rest offset"},
        {"Flags", "flags", "Flags"}});
    has_summary |= summary_group("Scene", {
        {"Gravity", "gravity", "Gravity"},
        {"BounceThresholdVelocity", "bounceThresholdVelocity", "Bounce threshold"}});
    ImGui::Spacing();
    if (!has_summary || state.property_search[0])
      ImGui::SetNextItemOpen(true);
    if (ImGui::CollapsingHeader("All properties")) {
      properties(doc, state, *object);
    }
    panel_error(state);
  } catch (const std::exception &e) {
    state.error = e.what();
    panel_error(state);
  }
}

namespace {
// The input path follows Dear ImGui's native SliderScalar: the value remains
// U64 throughout dragging, keyboard navigation and temporary exact text input.
// Only the track and thumb presentation are replaced here.
bool timeline_scrubber(std::uint64_t &value, std::uint64_t high) {
  auto *window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;
  auto &g = *ImGui::GetCurrentContext();
  const auto id = window->GetID("##frame");
  const std::uint64_t low = 1;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImRect frame(origin, {origin.x + ImGui::CalcItemWidth(),
                              origin.y + ui_pixels(18)});
  ImGui::ItemSize(frame);
  if (!ImGui::ItemAdd(frame, id, &frame, ImGuiItemFlags_Inputable))
    return false;
  const bool hovered = ImGui::ItemHoverable(frame, id, g.LastItemData.ItemFlags);
  bool editing = ImGui::TempInputIsActive(id);
  if (!editing) {
    const bool clicked = hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left, ImGuiInputFlags_None, id);
    const bool activate = clicked || g.NavActivateId == id;
    if (clicked)
      ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
    if (activate && ((clicked && g.IO.KeyCtrl) ||
        (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput))))
      editing = true;
    if (activate)
      std::memcpy(&g.ActiveIdValueOnActivation, &value, sizeof(value));
    if (activate && !editing) {
      ImGui::SetActiveID(id, window);
      ImGui::SetFocusID(id, window);
      ImGui::FocusWindow(window);
      g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
    }
  }
  if (editing) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ui_pixels(9), ui_pixels(2)});
    const bool changed = ImGui::TempInputScalar(frame, id, "##frame", ImGuiDataType_U64,
                                               &value, "%llu", &low, &high);
    ImGui::PopStyleVar();
    return changed;
  }

  const float radius = ui_pixels(6.5f);
  const float track_min = frame.Min.x + radius;
  const float track_max = std::max(track_min, frame.Max.x - radius);
  const float usable = track_max - track_min;
  // Integer sliders normally widen the grab to represent one whole item. Match
  // that native behavior rectangle to the small circular thumb's visible range,
  // so even a two-frame recording can seek all the way to either endpoint.
  const float grab = high > 1
      ? std::max(g.Style.GrabMinSize,
          static_cast<float>(static_cast<long double>(usable) /
                             static_cast<long double>(high - 1)))
      : g.Style.GrabMinSize;
  const ImRect behavior({track_min - grab * .5f - 2, frame.Min.y},
                         {track_max + grab * .5f + 2, frame.Max.y});
  ImRect grab_bounds;
  const bool changed = ImGui::SliderBehavior(behavior, id, ImGuiDataType_U64,
      &value, &low, &high, "%llu", ImGuiSliderFlags_AlwaysClamp, &grab_bounds);
  if (changed)
    ImGui::MarkItemEdited(id);
  const float thumb_x = high > 1 ? (grab_bounds.Min.x + grab_bounds.Max.x) * .5f
                                  : track_min;
  const float cy = (frame.Min.y + frame.Max.y) * .5f;
  const float half_track = ui_pixels(2);
  auto *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled({track_min, cy - half_track}, {track_max, cy + half_track},
                       ImGui::GetColorU32(ImGuiCol_Border), half_track);
  if (thumb_x > track_min)
    draw->AddRectFilled({track_min, cy - half_track}, {thumb_x, cy + half_track},
                         ImGui::GetColorU32(ImGuiCol_CheckMark), half_track);
  draw->AddCircleFilled({thumb_x, cy}, radius,
                         ImGui::GetColorU32(ImGuiCol_CheckMark), 24);
  ImGui::RenderNavCursor(frame, id);
  return changed;
}
} // namespace

void drawTimeline(Document &doc, PanelState &state) {
  try {
    if (!doc.loaded()) {
      muted("Open a capture to inspect its recorded frames.");
      return;
    }
    struct TimelineSpacing {
      TimelineSpacing() {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ui_pixels(4), ui_pixels(3)});
      }
      ~TimelineSpacing() { ImGui::PopStyleVar(); }
    } spacing;
    const auto width = ImGui::GetContentRegionAvail().x;
    const bool compact = width < ui_pixels(760);
    const auto &style = ImGui::GetStyle();
    const float row_right = ImGui::GetCursorPosX() + width;
    auto sample_point = [&] {
      const bool after = doc.sample_point() == SamplePoint::after_fetch;
      const float combo_width = ui_pixels(172);
      const auto origin = ImGui::GetCursorScreenPos();
      auto *draw = ImGui::GetWindowDrawList();
      ImGui::SetNextItemWidth(combo_width);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0, 0, 0, 0});
      const bool open = ui::begin_combo("##sample-point",
                            after ? "After simulation" : "Frame end", ImGuiComboFlags_NoArrowButton);
      ui::icon(ui::Icon::chevron_down,
               {origin.x + combo_width - ui_pixels(22),
                origin.y + (ImGui::GetFrameHeight() - ui_pixels(16)) * .5f},
               ui_pixels(16), ImGui::GetColorU32(ImGuiCol_TextDisabled), draw);
      // Draw into the originating window: BeginCombo may have entered a popup.
      if (open) {
        if (ImGui::Selectable("Frame end", !after))
          command(state,
                  [&] { doc.set_sample_point(SamplePoint::native_frame); });
        if (ImGui::Selectable("After simulation", after))
          command(state, [&] {
            if (!doc.set_sample_point(SamplePoint::after_fetch))
              throw std::runtime_error(
                  "No after-simulation markers are available for this scene.");
          });
        ui::end_combo();
      }
      ImGui::PopStyleColor();
      tooltip("Frame end uses the simulation's recorded frame boundary. After "
              "simulation uses an explicit post-step marker when available.");
    };
    auto total = doc.timeline().size();
    {
      DisabledScope disabled(total == 0);
      if (ui::icon_button("First", ui::Icon::first, "First frame / Home"))
        command(state, [&] { doc.set_frame(0); });
      ImGui::SameLine();
      if (ui::icon_button("<", ui::Icon::chevron_left, "Previous frame / Left"))
        command(state, [&] { doc.step(-1); });
      ImGui::SameLine();
      if (ui::icon_button(doc.playback().playing ? "Pause" : "Play",
                           doc.playback().playing ? ui::Icon::pause : ui::Icon::play,
                           "Play / pause / Space", true))
        command(state, [&] { doc.set_playing(!doc.playback().playing); });
      ImGui::SameLine();
      if (ui::icon_button(">", ui::Icon::chevron_right, "Next frame / Right"))
        command(state, [&] { doc.step(1); });
      ImGui::SameLine();
      if (ui::icon_button("Last", ui::Icon::last, "Last frame / End"))
        command(state, [&] { doc.set_frame(total - 1); });
    }

    const float fps_width = ui_pixels(compact ? 60.f : 75.f);
    const float options_width = ui_pixels(30);
    const float controls_width =
        fps_width + ImGui::CalcTextSize("fps").x + style.ItemInnerSpacing.x +
        options_width + style.ItemSpacing.x +
        (compact ? 0 : ui_pixels(172) + style.ItemSpacing.x);
    const float next_x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                         style.ItemSpacing.x;
    const float controls_x = std::max(next_x, row_right - controls_width);
    {
      const auto full =
          std::to_string(total ? doc.frame_index() + 1 : 0) + " / " +
          std::to_string(total) + " frames" + (compact ?
              (doc.sample_point() == SamplePoint::after_fetch ? " · After simulation" : " · Frame end") : "");
      auto preview = full;
      const float available =
          std::max(0.f, controls_x - next_x - style.ItemSpacing.x);
      while (!preview.empty() && ImGui::CalcTextSize(
                 (preview + (preview == full ? "" : "...")).c_str()).x > available) {
        auto end = preview.size() - 1;
        while (end && ((static_cast<unsigned char>(preview[end]) & 0xc0) == 0x80))
          --end;
        preview.resize(end);
      }
      if (!preview.empty() && preview != full)
        preview += "...";
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(preview.c_str());
      tooltip(full);
    }
    ImGui::SameLine(controls_x);
    if (!compact) {
      sample_point();
      ImGui::SameLine();
    }
    {
      DisabledScope disabled(doc.timeline().empty());
      float fps =
          static_cast<float>(std::abs(doc.playback().frames_per_second));
      ImGui::SetNextItemWidth(fps_width);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0, 0, 0, 0});
      if (ImGui::DragFloat("fps", &fps, .25f, 1, 240, "%.1f",
                           ImGuiSliderFlags_AlwaysClamp))
        command(state, [&] {
          auto p = doc.playback();
          p.frames_per_second = fps * (p.frames_per_second < 0 ? -1 : 1);
          doc.set_playback(p);
        });
      ImGui::PopStyleColor();
    }
    tooltip("Captured frames per second; this does not infer simulation time.");
    ImGui::SameLine();
    if (!state.error.empty())
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.98f, .52f, .45f, 1));
    const bool playback_options = ui::icon_button("...##playback-options", ui::Icon::more,
                                                   "Playback options");
    if (!state.error.empty())
      ImGui::PopStyleColor();
    tooltip(state.error.empty() ? "Playback options" : state.error);
    if (playback_options) {
      ui::anchor_popup_to_item();
      ImGui::OpenPopup("Playback options");
    }
    if (ui::begin_popup("Playback options")) {
      ImGui::TextUnformatted("Playback");
      ImGui::Separator();
      if (compact) {
        sample_point();
        ImGui::Separator();
      }
      {
        DisabledScope disabled(doc.timeline().empty());
        bool loop = doc.playback().loop;
        if (ImGui::Checkbox("Loop", &loop))
          command(state, [&] {
            auto p = doc.playback();
            p.loop = loop;
            doc.set_playback(p);
          });
        bool reverse = doc.playback().frames_per_second < 0;
        if (ImGui::Checkbox("Reverse", &reverse))
          command(state, [&] {
            auto p = doc.playback();
            p.frames_per_second =
                std::abs(p.frames_per_second) * (reverse ? -1 : 1);
            doc.set_playback(p);
          });
      }
      if (const auto frame = doc.frame(); frame && frame->marker)
        if (auto ms = interval_ms(doc, *frame->marker)) {
          ImGui::Separator();
          muted("Recorded interval: " + real(*ms) + " ms");
          tooltip("Captured marker interval, not an inferred simulation timestep.");
        }
      panel_error(state);
      ui::end_popup();
    }

    // Two rows: transport and sampling controls, then an exact 64-bit scrubber.
    // Re-query after a sampling-mode change: its timeline can have a different
    // length (including zero) from the one used to draw the transport controls.
    total = doc.timeline().size();
    if (total) {
      auto index = static_cast<std::uint64_t>(doc.frame_index() + 1);
      ImGui::SetNextItemWidth(-1);
      if (timeline_scrubber(index, static_cast<std::uint64_t>(total)))
        command(state,
                [&] { doc.set_frame(static_cast<std::size_t>(index - 1)); });
      tooltip(state.error.empty()
                  ? "Drag to scrub. Ctrl-click to enter an exact frame number."
                  : state.error);
      const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
      const auto tick_origin = ImGui::GetCursorScreenPos();
      const float tick_font = ImGui::GetFontSize() * .86f;
      const float tick_left = min.x + ui_pixels(6.5f),
                  tick_right = max.x - ui_pixels(6.5f);
      auto *draw = ImGui::GetWindowDrawList();
      const auto last_label = std::to_string(total);
      const float label_width = ImGui::GetFont()->CalcTextSizeA(
          tick_font, std::numeric_limits<float>::max(), 0, last_label.c_str()).x;
      unsigned divisions = 4;
      if ((tick_right - tick_left) / 4 < label_width + ui_pixels(14)) divisions = 2;
      if ((tick_right - tick_left) / 2 < label_width + ui_pixels(14)) divisions = 1;
      if (total == 1) divisions = 0;
      for (unsigned tick = 0; tick <= divisions; ++tick) {
        // Divide first: no overflow for long recordings, and labels retain
        // exact captured integer frame numbers rather than rounded floats.
        const std::uint64_t span = static_cast<std::uint64_t>(total) - 1;
        const std::uint64_t number = divisions ?
            1 + (span / divisions) * tick + (span % divisions) * tick / divisions : 1;
        const auto label = std::to_string(number);
        const float text_width = ImGui::GetFont()->CalcTextSizeA(
            tick_font, std::numeric_limits<float>::max(), 0, label.c_str()).x;
        const float ratio = divisions ?
            static_cast<float>(tick) / static_cast<float>(divisions) : 0;
        float x = tick_left + (tick_right - tick_left) * ratio - text_width * .5f;
        x = std::clamp(x, min.x, std::max(min.x, max.x - text_width));
        draw->AddText(ImGui::GetFont(), tick_font, {x, tick_origin.y},
                       ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
      }
      ImGui::Dummy({0, tick_font});
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
