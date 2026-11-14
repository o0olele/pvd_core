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
std::string vec(Vec3 v) {
  return real(v.x) + ", " + real(v.y) + ", " + real(v.z);
}
std::string quat(Quat q) {
  return real(q.x) + ", " + real(q.y) + ", " + real(q.z) + ", " + real(q.w);
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
                    const PropertyValue &value, std::size_t item) {
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
  const auto summary = propertyValueText(value, item);
  text(summary);
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
      const auto page_size =
          std::clamp<std::size_t>(state.array_page_size, 1, 128);
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
      for (std::uint64_t offset = 0; offset < count; ++offset) {
        const auto item = start + offset;
        IdScope row(std::to_string(item));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        muted("  [" + std::to_string(item) + "]");
        ImGui::TableSetColumnIndex(1);
        if (item > std::numeric_limits<std::size_t>::max())
          muted("Index unavailable on this host");
        else
          property_value(doc, state, value, static_cast<std::size_t>(item));
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
} // namespace

std::string propertyValueText(const PropertyValue &value, std::size_t item) {
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
        return real(*n);
      if (auto n = value.as_vec3(item))
        return vec(*n);
      if (auto n = value.as_quat(item))
        return quat(*n);
      if (auto n = value.as_transform(item))
        return "P: " + vec(n->position) + "\nQ: " + quat(n->rotation);
      if (name == "PxVec2" || name == "PxVec4") {
        const auto components = name == "PxVec2" ? 2u : 4u;
        const auto stride = components * 4;
        if (item > std::numeric_limits<std::size_t>::max() / stride)
          return "<unavailable>";
        std::string s;
        for (unsigned j = 0; j < components; ++j) {
          if (j)
            s += ", ";
          s += real(raw_float(value, item * stride + j * 4));
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
                    raw_float(value, base + 8)}) +
               "\nMax: " +
               vec({raw_float(value, base + 12), raw_float(value, base + 16),
                    raw_float(value, base + 20)});
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
            out += real(raw_float(value, item * stride + (col * n + row) * 4));
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
  try {
    if (!doc.loaded()) {
      drawEmptyState("No capture loaded",
                     "Open a PVD recording to browse its scenes and objects.");
      return;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##hierarchy-search", "Search this scene by name, class or ID...",
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
          if (ImGui::IsItemClicked())
            command(state, [&] {
              if (!doc.select(key) && !doc.inspect(shape->key.id))
                throw std::runtime_error(
                    "This shape is not present at the current frame.");
              state.reveal_object = shape->key;
            });
          tooltip(shape->class_name + " | " + id_text(shape->key.id) +
                  (shape->present ? "" : " | Not present at this frame"));
        }
      }
      if (!any)
        muted(query.empty() ? "No objects at this frame."
                            : "No matching objects.");
    }
    state.reveal_object.reset();
    panel_error(state);
  } catch (const std::exception &e) {
    state.error = e.what();
    panel_error(state);
  }
}

void drawInspector(Document &doc, PanelState &state) {
  try {
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
      if (width > 900) {
        ImGui::SameLine();
        float fps = static_cast<float>(doc.playback().frames_per_second);
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragFloat("fps", &fps, .25f, 1, 240, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp))
          command(state, [&] {
            auto p = doc.playback();
            p.frames_per_second = fps;
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
        ImGui::SameLine();
        float fps = static_cast<float>(doc.playback().frames_per_second);
        ImGui::SetNextItemWidth(65);
        if (ImGui::DragFloat("fps", &fps, .25f, 1, 240, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp))
          command(state, [&] {
            auto p = doc.playback();
            p.frames_per_second = fps;
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
