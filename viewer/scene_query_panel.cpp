#include "scene_query_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include "renderer.hpp"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace pvd::viewer {
namespace {
using namespace render;
const char *kind(SceneQueryKind k) {
  switch (k) {
  case SceneQueryKind::raycast:
    return "Raycast";
  case SceneQueryKind::sweep:
    return "Sweep";
  case SceneQueryKind::overlap:
    return "Overlap";
  }
  return "Unknown";
}
const char *role(SceneQueryHitRole r) {
  switch (r) {
  case SceneQueryHitRole::blocking:
    return "blocking";
  case SceneQueryHitRole::touching:
    return "touching";
  case SceneQueryHitRole::unreported:
    return "unreported role";
  }
  return "unknown role";
}
std::string hex(std::uint64_t v) {
  std::ostringstream out;
  out << "0x" << std::hex << std::uppercase << std::setfill('0')
      << std::setw(16) << v;
  return out.str();
}
std::string lower(std::string v) {
  for (auto &c : v)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v;
}
bool same(const SceneQuerySource &a, const SceneQuerySource &b) {
  return same_query_source(a, b);
}
bool same(const std::optional<SceneQuerySource> &a,
          const std::optional<SceneQuerySource> &b) {
  return a.has_value() == b.has_value() && (!a || same(*a, *b));
}
template <class F> void action(SceneQueryPanelState &s, F &&fn) {
  try {
    fn();
    s.error.clear();
  } catch (const std::exception &e) {
    s.error = e.what();
  }
}
std::string searchable(const SceneQuery &q) {
  std::ostringstream out;
  out << sceneQuerySourceText(q.source) << ' '
      << (q.source.batched ? "batch" : "native") << ' ' << q.type << ' '
      << hex(q.filter_flags);
  for (const auto &range :
       {q.hits_range, q.geometry_range, q.pose_range, q.filter_data_range})
    out << ' ' << range.array_name << ' ' << range.base_index << ' '
        << range.count;
  for (const auto &hit : q.hits)
    out << ' ' << hex(hit.actor_id) << ' ' << hit.actor_id << ' '
        << hex(hit.shape_id) << ' ' << hit.shape_id;
  return lower(out.str());
}
void value(const char *label, const std::string &text) {
  ImGui::TextWrapped("%s: %s", label, text.c_str());
}
void vec(const char *label, Vec3 v) {
  ImGui::TextWrapped("%s: (%.9g, %.9g, %.9g)", label, v.x, v.y, v.z);
}
void pose(const char *label, const Transform &v) {
  vec(label, v.position);
  ImGui::TextWrapped("  quaternion: (%.9g, %.9g, %.9g, %.9g)", v.rotation.x,
                     v.rotation.y, v.rotation.z, v.rotation.w);
}
void range(const char *label, const SceneQueryArrayRange &r) {
  ImGui::TextWrapped("%s: %s / base %u / count %u", label,
                     r.array_name.empty() ? "not transmitted"
                                          : r.array_name.c_str(),
                     r.base_index, r.count);
}
void collections(const SceneQueries &queries) {
  if (ImGui::BeginTable("Query collection availability", 2,
                        ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Collection", ImGuiTableColumnFlags_WidthStretch,
                            .44f);
    ImGui::TableSetupColumn("Transport", ImGuiTableColumnFlags_WidthStretch,
                            .56f);
    ImGui::TableHeadersRow();
    for (const auto &c : queries.collections) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s%s", c.batched ? "Batch " : "Native ",
                         kind(c.kind));
      ImGui::TableNextColumn();
      const char *state =
          c.state == SceneQueryCollectionState::recorded    ? "recorded"
          : c.state == SceneQueryCollectionState::malformed ? "malformed"
                                                            : "uncollected";
      ImGui::TextWrapped("%s / %zu queries", state, c.query_count);
      if (c.publication_event_count) {
        ImGui::TextDisabled("publication %zu", c.publication_event_count);
      }
    }
    ImGui::EndTable();
  }
}
void inspect_button(Document &doc, SceneQueryPanelState &state,
                    const char *label, std::uint64_t raw,
                    const std::optional<ObjectKey> &key,
                    const Snapshot &snapshot) {
  const auto *instance = raw ? snapshot.instance(raw) : nullptr;
  const bool valid = key && instance && key->id == raw &&
                     instance->generation == key->generation;
  ImGui::BeginDisabled(!valid);
  if (ImGui::Button(label))
    action(state, [&] {
      const auto current = doc.frame();
      const auto *object = current ? current->snapshot.instance(raw) : nullptr;
      if (!valid || !object || object->generation != key->generation ||
          !doc.inspect(raw))
        throw std::runtime_error(
            "Hit object is absent or its generation changed");
    });
  ImGui::EndDisabled();
  if (!valid) {
    ImGui::SameLine();
    ImGui::TextDisabled("unresolved / unavailable");
  }
}
void hit_details(Document &doc, SceneQueryPanelState &s, const SceneQueryHit &h,
                 const Snapshot &snapshot) {
  value("Actor source ID", hex(h.actor_id));
  if (h.actor)
    ImGui::TextDisabled("Actor generation %llu",
                        static_cast<unsigned long long>(h.actor->generation));
  inspect_button(doc, s, "Inspect hit actor", h.actor_id, h.actor, snapshot);
  value("Shape source ID", hex(h.shape_id));
  if (h.shape)
    ImGui::TextDisabled("Shape generation %llu",
                        static_cast<unsigned long long>(h.shape->generation));
  inspect_button(doc, s, "Inspect hit shape", h.shape_id, h.shape, snapshot);
  value("Hit role", role(h.role));
  value("Reported hit flags", hex(h.flags));
  if (h.position_available)
    vec("Position", h.position);
  else
    ImGui::TextUnformatted("Position: not reported");
  if (h.normal_available)
    vec("Normal", h.normal);
  else
    ImGui::TextUnformatted("Normal: not reported");
  if (h.distance_available)
    ImGui::Text("Distance: %.9g", h.distance);
  else
    ImGui::TextUnformatted("Distance: not reported");
  if (h.uv_available)
    ImGui::Text("UV: %.9g, %.9g", h.u, h.v);
  else
    ImGui::TextUnformatted("UV: not reported");
  if (h.face_index_available)
    ImGui::Text("Face index: %u", h.face_index);
  else
    ImGui::TextUnformatted("Face index: not reported");
}
void auxiliary(const SceneQuery &q) {
  if (!q.filter_data.empty() && ImGui::TreeNode("Recorded filter data")) {
    ImGui::BeginChild("filter data", {0, ui_pixels(120)}, ImGuiChildFlags_Borders);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(q.filter_data.size()));
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const auto &f = q.filter_data[static_cast<std::size_t>(i)];
        ImGui::TextWrapped("%d: %08X %08X %08X %08X", i + 1, f[0], f[1], f[2],
                           f[3]);
      }
    ImGui::EndChild();
    ImGui::TreePop();
  }
  if (!q.poses.empty() && ImGui::TreeNode("Recorded poses")) {
    ImGui::BeginChild("poses", {0, ui_pixels(150)}, ImGuiChildFlags_Borders);
    // Fixed two-line height keeps the auxiliary list virtualized.
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(q.poses.size()),
               ImGui::GetTextLineHeightWithSpacing() * 2);
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const auto &p = q.poses[static_cast<std::size_t>(i)];
        ImGui::Text("%d: %.5g %.5g %.5g", i + 1, p.position.x, p.position.y,
                    p.position.z);
        ImGui::Text("q %.5g %.5g %.5g %.5g", p.rotation.x, p.rotation.y,
                    p.rotation.z, p.rotation.w);
      }
    ImGui::EndChild();
    ImGui::TreePop();
  }
  if (!q.geometries.empty() && ImGui::TreeNode("Recorded query geometry")) {
    ImGui::TextWrapped(
        "Geometry belongs to this publication, not an invented actor.");
    ImGui::BeginChild("geometries", {0, ui_pixels(180)}, ImGuiChildFlags_Borders);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(q.geometries.size()),
               ImGui::GetTextLineHeightWithSpacing() * 3);
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const auto &g = q.geometries[static_cast<std::size_t>(i)];
        ImGui::Text("%d: %s", i + 1, hex(g.key.id).c_str());
        std::visit(
            [](const auto &v) {
              using T = std::decay_t<decltype(v)>;
              if constexpr (std::is_same_v<T, Box>) {
                vec("Box half extents", v.half_extents);
                ImGui::TextUnformatted("PhysX native axes");
              } else if constexpr (std::is_same_v<T, Sphere>) {
                ImGui::Text("Sphere radius %.9g", v.radius);
                ImGui::TextUnformatted(" ");
              } else if constexpr (std::is_same_v<T, Capsule>) {
                ImGui::Text("Capsule radius %.9g", v.radius);
                ImGui::Text("Half height %.9g (X axis)", v.half_height);
              } else {
                ImGui::Text("Convex resource %s", hex(v.resource_id).c_str());
                ImGui::Text("Scale %.5g %.5g %.5g", v.scale.scale.x,
                            v.scale.scale.y, v.scale.scale.z);
              }
            },
            g.value);
      }
    ImGui::EndChild();
    ImGui::TreePop();
  }
}
} // namespace

void SceneQueryPanelState::reset() { *this = SceneQueryPanelState{}; }
std::string sceneQuerySourceText(const render::SceneQuerySource &s) {
  std::ostringstream out;
  out << (s.batched ? "Batch " : "Native ") << kind(s.kind) << " #"
      << s.sequence + 1 << " / scene " << hex(s.scene.id) << " generation "
      << s.scene.generation << " / snapshot prefix " << s.snapshot_event_count
      << " / publication " << s.publication_event_count;
  return out.str();
}

void drawSceneQueries(Document &doc, OrbitCamera &camera,
                      RenderOptions &options, SceneQueryPanelState &state) {
  state.visible_rows.clear();
  state.visible_row_count = 0;
  state.hit_row_count = state.visible_hit_row_count = 0;
  if (!doc.loaded()) {
    state.row_count = 0;
    state.matching_queries.clear();
    state.cached_frame.reset();
    ImGui::TextWrapped(
        "Open a capture or connect live to inspect recorded scene queries.");
    return;
  }
  if (state.cached_source != doc.session()) {
    const auto aspect = state.viewport_aspect;
    state.reset();
    state.viewport_aspect = aspect;
    state.cached_source = doc.session();
  }
  if (doc.sample_point() != SamplePoint::native_frame) {
    state.row_count = 0;
    state.matching_queries.clear();
    state.cached_frame.reset();
    state.selected_hit.reset();
    ImGui::TextWrapped(
        "After-fetch markers are narrow sections, not native scene-query "
        "publication windows. Cached arrays are not fresh queries here.");
    if (ImGui::Button("Use Frame end"))
      action(state, [&] {
        if (!doc.set_sample_point(SamplePoint::native_frame))
          throw std::runtime_error(
              "This scene has no native Frame end samples");
      });
    if (!state.error.empty())
      ImGui::TextWrapped("%s", state.error.c_str());
    return;
  }
  auto frame = doc.frame();
  const auto &queries = frame->scene_queries;
  ImGui::TextWrapped(
      "Native frame queries. Sequence numbers are local to each collection.");
  ImGui::Checkbox("Query paths", &options.show_scene_queries);
  ImGui::SameLine();
  ImGui::Checkbox("Impact normals", &options.show_query_normals);
  ImGui::Checkbox("Query volumes", &options.show_query_volumes);
  std::size_t recorded = 0, uncollected = 0, malformed = 0;
  for (const auto &collection : queries.collections) {
    if (collection.state == SceneQueryCollectionState::recorded) ++recorded;
    else if (collection.state == SceneQueryCollectionState::uncollected) ++uncollected;
    else ++malformed;
  }
  ImGui::TextWrapped("%zu recorded | %zu uncollected | %zu malformed collections",
                     recorded, uncollected, malformed);
  ImGui::TextWrapped("Empty hits do not prove a miss; result status is unreported.");
  if (!queries.complete)
    ImGui::TextColored({1, .7f, .3f, 1},
                       "Incomplete query transport / diagnostics");
  int type = state.kind_filter ? static_cast<int>(*state.kind_filter) + 1 : 0;
  int batch = state.batched_filter ? (*state.batched_filter ? 2 : 1) : 0;
  ImGui::SetNextItemWidth(
      std::min(ui_pixels(160.f), ImGui::GetContentRegionAvail().x - ui_pixels(55)));
  if (ImGui::Combo("Kind", &type, "All kinds\0Raycast\0Sweep\0Overlap\0"))
    state.kind_filter = type ? std::optional<SceneQueryKind>(
                                   static_cast<SceneQueryKind>(type - 1))
                             : std::nullopt;
  ImGui::SetNextItemWidth(
      std::min(ui_pixels(160.f), ImGui::GetContentRegionAvail().x - ui_pixels(55)));
  if (ImGui::Combo("Mode", &batch, "Native + Batch\0Native\0Batch\0"))
    state.batched_filter =
        batch ? std::optional<bool>(batch == 2) : std::nullopt;
  ImGui::SetNextItemWidth(std::max(ui_pixels(1.f), ImGui::GetContentRegionAvail().x));
  ImGui::InputTextWithHint("##query-search",
                           "Filter type, raw ID, array or flags",
                           state.text_filter.data(), state.text_filter.size());
  const auto search = lower(state.text_filter.data());
  if (state.cached_frame.lock() != frame || state.cached_text != search ||
      state.cached_kind != state.kind_filter ||
      state.cached_batched != state.batched_filter) {
    state.matching_queries.clear();
    for (std::size_t i = 0; i < queries.queries.size(); ++i) {
      const auto &q = queries.queries[i];
      if (state.kind_filter && *state.kind_filter != q.source.kind)
        continue;
      if (state.batched_filter && *state.batched_filter != q.source.batched)
        continue;
      if (!search.empty() && searchable(q).find(search) == std::string::npos)
        continue;
      state.matching_queries.push_back(i);
    }
    state.cached_frame = frame;
    state.cached_text = search;
    state.cached_kind = state.kind_filter;
    state.cached_batched = state.batched_filter;
  }
  state.row_count = state.matching_queries.size();
  ImGui::Text("%zu / %zu queries", state.row_count, queries.queries.size());
  auto selection = doc.selected_query();
  if (!same(selection, state.cached_selection)) {
    state.selected_hit.reset();
    state.cached_selection = selection;
  }
  ImGui::BeginChild("Scene query rows", {0, ui_pixels(190)}, ImGuiChildFlags_Borders);
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(state.matching_queries.size()));
  while (clip.Step())
    for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
      const auto index = state.matching_queries[static_cast<std::size_t>(row)];
      const auto &q = queries.queries[index];
      ImGui::PushID(static_cast<int>(index));
      std::ostringstream label;
      label << (q.source.batched ? "Batch " : "Native ") << kind(q.source.kind)
            << " #" << q.source.sequence + 1 << " / " << q.hits.size()
            << " hit records" << (q.complete ? "" : " / incomplete");
      const bool selected = selection && same(*selection, q.source);
      if (ImGui::Selectable(label.str().c_str(), selected))
        action(state, [&] {
          if (!doc.select_query(q.source))
            throw std::runtime_error(
                "Query source is no longer present in this sample");
          selection = q.source;
          state.cached_selection = selection;
          state.selected_hit.reset();
        });
      const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      if (ImGui::IsItemVisible()) {
        state.visible_rows.push_back(
            {q.source, a.x, a.y, b.x - a.x, b.y - a.y});
        ++state.visible_row_count;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", sceneQuerySourceText(q.source).c_str());
      ImGui::PopID();
    }
  ImGui::EndChild();
  if (ImGui::CollapsingHeader("Collection transport and field limits")) {
    collections(queries);
    ImGui::TextWrapped(
        "SDK 3.4 does not transmit hasBlock, batch status, requested hit flags "
        "or sweep inflation. Ordinary hit roles are unreported; eNO_BLOCK "
        "can establish touching-only hits.");
  }
  const SceneQuery *selected = nullptr;
  if (selection)
    for (const auto &q : queries.queries)
      if (same(*selection, q.source)) {
        selected = &q;
        break;
      }
  if (selected) {
    const auto &q = *selected;
    ImGui::Separator();
    ImGui::TextWrapped("%s", sceneQuerySourceText(q.source).c_str());
    if (ImGui::Button("Fit query"))
      action(state, [&] {
        if (!doc.fit_query(camera, state.viewport_aspect))
          throw std::runtime_error(
              "Selected query has no finite visualization bounds");
      });
    value("Stream ID", hex(q.source.stream_id));
    ImGui::TextWrapped(
        "Outer timestamp %llu / byte offset %llu",
        static_cast<unsigned long long>(q.source.outer_timestamp),
        static_cast<unsigned long long>(q.source.byte_offset));
    ImGui::Text("Raw query type: %u", q.type);
    if (q.coordinates_ambiguous)
      ImGui::TextWrapped("OriginShift overlaps collection: values below are raw "
                         "recorded coordinates with unknown per-query epochs. "
                         "Spatial preview and Fit are unavailable.");
    value("Filter flags", hex(q.filter_flags));
    if (q.origin)
      vec("Origin", *q.origin);
    if (q.unit_direction)
      vec("Unit direction", *q.unit_direction);
    if (q.max_distance)
      ImGui::Text("Max distance: %.9g", *q.max_distance);
    if (q.overlap_pose)
      pose("Overlap position", *q.overlap_pose);
    value("Requested hit flags", q.requested_hit_flags
                                     ? hex(*q.requested_hit_flags)
                                     : "not transmitted");
    if (q.sweep_inflation)
      ImGui::Text("Sweep inflation: %.9g", *q.sweep_inflation);
    else
      ImGui::TextUnformatted("Sweep inflation: not transmitted");
    value("hasBlock", q.has_block
                          ? (*q.has_block ? "true (derived)"
                                          : "false (derived from eNO_BLOCK)")
                          : "not transmitted");
    ImGui::TextUnformatted("Batch query status: not transmitted");
    range("Hits", q.hits_range);
    range("Geometry", q.geometry_range);
    range("Poses", q.pose_range);
    range("Filter data", q.filter_data_range);
    auxiliary(q);
    state.hit_row_count = q.hits.size();
    if (q.hits.empty())
      ImGui::TextWrapped("No hit records in this query's transmitted range. "
                         "Outcome is unreported; this is not a proven miss.");
    else {
      ImGui::Text("%zu hit records (role preserved)", q.hits.size());
      if (ImGui::BeginTable("Query hit rows", 3,
                            ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingStretchProp,
                            {0, ui_pixels(170)})) {
        ImGui::TableSetupColumn("Hit", ImGuiTableColumnFlags_WidthFixed, ui_pixels(35));
        ImGui::TableSetupColumn("Actor / shape source IDs",
                                ImGuiTableColumnFlags_WidthStretch, .65f);
        ImGui::TableSetupColumn("Role / distance",
                                ImGuiTableColumnFlags_WidthStretch, .35f);
        ImGui::TableHeadersRow();
        ImGuiListClipper hits;
        hits.Begin(static_cast<int>(q.hits.size()));
        while (hits.Step())
          for (int i = hits.DisplayStart; i < hits.DisplayEnd; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const auto &hit = q.hits[static_cast<std::size_t>(i)];
            const auto label = std::to_string(i + 1);
            if (ImGui::Selectable(label.c_str(),
                                  state.selected_hit ==
                                      static_cast<std::size_t>(i),
                                  ImGuiSelectableFlags_SpanAllColumns))
              state.selected_hit = static_cast<std::size_t>(i);
            if (ImGui::IsItemVisible())
              ++state.visible_hit_row_count;
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", hex(hit.actor_id).c_str());
            ImGui::TextWrapped("%s", hex(hit.shape_id).c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", role(hit.role));
            if (hit.distance_available)
              ImGui::Text("%.7g", hit.distance);
            else
              ImGui::TextUnformatted("unreported");
            ImGui::PopID();
          }
        ImGui::EndTable();
      }
      if (state.selected_hit && *state.selected_hit < q.hits.size())
        hit_details(doc, state, q.hits[*state.selected_hit], frame->snapshot);
      else
        ImGui::TextDisabled(
            "Select a hit to inspect raw IDs and available fields.");
    }
  } else
    ImGui::TextWrapped("Select a query record to inspect its source, ranges "
                       "and hit availability.");
  if (!queries.diagnostics.empty() &&
      ImGui::CollapsingHeader("Query transport diagnostics"))
    for (const auto &d : queries.diagnostics)
      ImGui::TextWrapped("%s [payload %zu]: %s", d.property.c_str(),
                         d.payload_offset, d.message.c_str());
  const auto volume_error = [](const std::string &message) {
    return message.rfind("Scene query volume:", 0) == 0;
  };
  if (std::any_of(frame->errors.begin(), frame->errors.end(), volume_error) &&
      ImGui::CollapsingHeader("Query visualization diagnostics"))
    for (const auto &message : frame->errors)
      if (volume_error(message))
        ImGui::TextWrapped("%s", message.c_str());
  if (!state.error.empty())
    ImGui::TextWrapped("%s", state.error.c_str());
}
} // namespace pvd::viewer
