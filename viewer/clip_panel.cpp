#include "clip_panel.hpp"
#include "imgui.h"
#include "ui_scale.hpp"
#include <algorithm>
#include <cstdio>

namespace pvd::viewer {
namespace {
template <class F> void action(ClipPanelState &state, F &&fn) {
  try {
    fn();
    state.error.clear();
  } catch (const std::exception &error) {
    state.error = error.what();
  }
}
std::optional<clips::SceneKey> current_scene(const Document &doc) {
  const auto frame = doc.frame();
  const auto id = doc.scene_id();
  const auto *scene = frame && id ? frame->snapshot.instance(*id) : nullptr;
  return scene ? std::optional<clips::SceneKey>{{scene->id, scene->generation}}
               : std::nullopt;
}
clips::Catalog *catalog(ClipPanelState &state) {
  if (!state.active_catalog)
    return nullptr;
  const auto at = state.catalogs.find(*state.active_catalog);
  return at == state.catalogs.end() ? nullptr : &at->second;
}
void load_selected(ClipPanelState &state, const clips::Catalog &catalog) {
  if (const auto *clip = catalog.selected()) {
    std::snprintf(state.name.data(), state.name.size(), "%s",
                  clip->name.c_str());
    state.first_frame = clip->frames.first + 1;
    state.last_frame = clip->frames.last + 1;
    state.edited_clip = clip->id;
  } else {
    state.edited_clip.reset();
    state.name[0] = 0;
  }
}
std::string scene_label(clips::SceneKey scene) {
  return "Scene " + std::to_string(scene.id) + " / generation " +
         std::to_string(scene.generation);
}
void next_button(const char *label) {
  const auto right =
      ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  const auto width =
      ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
  if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <=
      right)
    ImGui::SameLine();
}
std::size_t playhead(const Document &doc, const clips::Catalog &catalog) {
  if (doc.sample_point() != SamplePoint::native_frame ||
      current_scene(doc) !=
          std::optional<clips::SceneKey>(catalog.state().binding.scene))
    throw clips::Error("Choose this scene lifetime and Frame end before "
                       "editing at the playhead");
  return doc.frame_index();
}
std::string fit_name(const std::string &name, float available) {
  auto count = name.size();
  while (
      count &&
      ImGui::CalcTextSize(
          (name.substr(0, count) + (count < name.size() ? "..." : "")).c_str())
              .x > available) {
    --count;
    while (count && (static_cast<unsigned char>(name[count]) & 0xc0) == 0x80)
      --count;
  }
  return name.substr(0, count) + (count < name.size() ? "..." : "");
}
} // namespace
void drawClipRanges(Document &doc, std::uint64_t source_slot,
                    ClipPanelState &state, const ClipPanelActions &actions) {
  state.visible_rows = 0;
  ImGui::TextUnformatted("NAMED CLIPS");
  if (!source_slot || (state.source_slot && state.source_slot != source_slot)) {
    ImGui::TextWrapped("This clip panel belongs to another source tab.");
    return;
  }
  state.source_slot = source_slot;
  if (!doc.loaded() || !doc.session()) {
    ImGui::TextWrapped(
        "Open a recording or receive closed native frames to create clips.");
    return;
  }
  const auto *session = doc.session();
  const auto changed = [&](ClipPanelChangeKind kind =
                               ClipPanelChangeKind::edited) {
    if (actions.changed)
      actions.changed({source_slot, kind});
  };
  if (state.observed_source && state.observed_source != session &&
      !state.catalogs.empty())
    state.needs_verification = true;
  state.observed_source = session;
  if (state.needs_verification) {
    ImGui::TextWrapped("The source was replaced. Existing clips are disabled "
                       "until their sealed prefixes are verified.");
    ImGui::BeginDisabled(actions.busy);
    if (ImGui::Button("Verify saved clips"))
      action(state, [&] {
        std::vector<clips::State> values;
        for (const auto &entry : state.catalogs)
          values.push_back(entry.second.state());
        auto restored = clips::restore_catalogs(*session, values);
        std::map<clips::SceneKey, clips::Catalog> verified;
        for (std::size_t i = 0; i < restored.size(); ++i) {
          if (!restored[i].catalog)
            throw clips::Error(restored[i].error);
          verified.emplace(values[i].binding.scene,
                           std::move(*restored[i].catalog));
        }
        state.catalogs.swap(verified);
        state.needs_verification = false;
        changed(ClipPanelChangeKind::verified);
      });
    next_button("Discard saved clips");
    if (ImGui::Button("Discard saved clips"))
      action(state, [&] {
        state.catalogs.clear();
        state.active_catalog.reset();
        state.edited_clip.reset();
        state.needs_verification = false;
        state.error.clear();
        changed(ClipPanelChangeKind::discarded);
      });
    ImGui::EndDisabled();
  }
  ImGui::BeginDisabled(actions.busy || state.needs_verification);
  const auto scene = current_scene(doc);
  ImGui::BeginDisabled(!scene);
  if (ImGui::Button("Use current scene"))
    action(state, [&] {
      auto at = state.catalogs.find(*scene);
      if (at == state.catalogs.end()) {
        auto created = clips::Catalog::create(*session, *scene);
        at = state.catalogs.emplace(*scene, std::move(created)).first;
      } else
        at->second.validate(*session);
      state.active_catalog = *scene;
      load_selected(state, at->second);
      changed();
    });
  ImGui::EndDisabled();
  if (!state.catalogs.empty()) {
    ImGui::SetNextItemWidth(
        std::min(ui_pixels(430), ImGui::GetContentRegionAvail().x));
    const auto label = state.active_catalog ? scene_label(*state.active_catalog)
                                            : "Choose a clip scene/lifetime";
    if (ImGui::BeginCombo("##clip-catalog", label.c_str())) {
      for (auto &entry : state.catalogs) {
        const auto text = scene_label(entry.first);
        if (ImGui::Selectable(text.c_str(),
                              state.active_catalog == entry.first))
          action(state, [&] {
            entry.second.validate(*session);
            state.active_catalog = entry.first;
            load_selected(state, entry.second);
            changed();
          });
      }
      ImGui::EndCombo();
    }
  }
  auto *ranges = catalog(state);
  if (ranges) {
    const auto &binding = ranges->state().binding;
    ImGui::TextWrapped(
        "%s | closed native frames %zu-%zu | sealed prefix %llu",
        scene_label(binding.scene).c_str(), binding.available.first + 1,
        binding.available.last + 1,
        static_cast<unsigned long long>(binding.source.event_count));
    ImGui::TextWrapped(
        "Ranges use original native frame numbers. Split keeps the playhead in "
        "the left clip. Trims keep the playhead. Live append preserves "
        "existing sealed clips; Refresh admits newly closed frames.");
    if (ImGui::Button("Refresh closed frames"))
      action(state, [&] {
        ranges->refresh(*session);
        state.first_frame = ranges->state().binding.available.first + 1;
        state.last_frame = ranges->state().binding.available.last + 1;
        changed();
      });
    if (ranges->state().selected != state.edited_clip)
      load_selected(state, *ranges);
    std::optional<clips::Id> choose;
    if (ImGui::BeginTable("##named-clips", 4,
                          ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit,
                          {0, ui_pixels(150)})) {
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed,
                              ui_pixels(65));
      ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed,
                              ui_pixels(65));
      ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed,
                              ui_pixels(65));
      ImGui::TableHeadersRow();
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(ranges->state().clips.size()));
      while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
          const auto &clip =
              ranges->state().clips[static_cast<std::size_t>(row)];
          ImGui::PushID(std::to_string(clip.id).c_str());
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          const auto start = ImGui::GetCursorScreenPos();
          const auto name =
              fit_name(clip.name, ImGui::GetContentRegionAvail().x);
          if (ImGui::Selectable("##clip", ranges->state().selected == clip.id,
                                ImGuiSelectableFlags_SpanAllColumns))
            choose = clip.id;
          ImGui::GetWindowDrawList()->AddText(
              start, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
          if (ImGui::IsItemVisible())
            ++state.visible_rows;
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nID %llu | source prefix %llu",
                              clip.name.c_str(),
                              static_cast<unsigned long long>(clip.id),
                              static_cast<unsigned long long>(
                                  clip.binding.source.event_count));
          ImGui::TableNextColumn();
          ImGui::Text("%zu", clip.frames.first + 1);
          ImGui::TableNextColumn();
          ImGui::Text("%zu", clip.frames.last + 1);
          ImGui::TableNextColumn();
          ImGui::Text("%zu", clip.frames.count());
          ImGui::PopID();
        }
      ImGui::EndTable();
    }
    if (choose)
      action(state, [&] {
        ranges->select(*session, *choose);
        load_selected(state, *ranges);
        changed();
      });
    const bool native_here =
        doc.sample_point() == SamplePoint::native_frame &&
        current_scene(doc) == std::optional<clips::SceneKey>(binding.scene);
    if (!native_here)
      ImGui::TextWrapped("Switch to this scene's Frame end timeline to split "
                         "or trim at its native playhead.");
    const bool selected = ranges->selected() != nullptr;
    ImGui::BeginDisabled(!selected || !native_here);
    if (ImGui::Button("Split at playhead"))
      action(state, [&] {
        ranges->split(*session, playhead(doc, *ranges));
        load_selected(state, *ranges);
        changed();
      });
    next_button("Trim left");
    if (ImGui::Button("Trim left"))
      action(state, [&] {
        ranges->trim_left(*session, playhead(doc, *ranges));
        load_selected(state, *ranges);
        changed();
      });
    next_button("Trim right");
    if (ImGui::Button("Trim right"))
      action(state, [&] {
        ranges->trim_right(*session, playhead(doc, *ranges));
        load_selected(state, *ranges);
        changed();
      });
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##clip-name", "Clip name (UTF-8)",
                             state.name.data(), state.name.size());
    ImGui::SetNextItemWidth(ui_pixels(105));
    ImGui::InputScalar("Start frame", ImGuiDataType_U64, &state.first_frame);
    ImGui::SetNextItemWidth(ui_pixels(105));
    ImGui::InputScalar("End frame", ImGuiDataType_U64, &state.last_frame);
    if (ImGui::Button("Add named range"))
      action(state, [&] {
        if (!state.first_frame || !state.last_frame ||
            state.first_frame > std::numeric_limits<std::size_t>::max() ||
            state.last_frame > std::numeric_limits<std::size_t>::max())
          throw clips::Error(
              "Frame numbers must be representable and start at one");
        ranges->add(*session, state.name.data(),
                    {static_cast<std::size_t>(state.first_frame - 1),
                     static_cast<std::size_t>(state.last_frame - 1)});
        load_selected(state, *ranges);
        changed();
      });
    next_button("Rename selected");
    ImGui::BeginDisabled(!selected);
    if (ImGui::Button("Rename selected"))
      action(state, [&] {
        ranges->rename(*session, ranges->selected()->id, state.name.data());
        changed();
      });
    if (ImGui::Button("Select range"))
      action(state, [&] {
        const auto clip = *ranges->selected();
        clips::validate_clip(*session, clip);
        if (!doc.set_scene(clip.binding.scene.id) ||
            !doc.set_sample_point(SamplePoint::native_frame) ||
            !doc.set_frame(clip.frames.first))
          throw clips::Error(
              "The selected native range is no longer available");
      });
    next_button("Delete selected");
    if (ImGui::Button("Delete selected"))
      action(state, [&] {
        ranges->erase(*session, ranges->selected()->id);
        load_selected(state, *ranges);
        changed();
      });
    next_button("Save As...");
    ImGui::BeginDisabled(!actions.save_as);
    if (ImGui::Button("Save As..."))
      action(state, [&] {
        const auto clip = *ranges->selected();
        clips::validate_clip(*session, clip);
        actions.save_as({source_slot, clip});
      });
    ImGui::EndDisabled();
    ImGui::EndDisabled();
  } else
    ImGui::TextWrapped(
        "Use the current scene to create its first named range. Other scenes "
        "and scene generations keep independent clip lists.");
  ImGui::EndDisabled();
  if (!state.error.empty())
    ImGui::TextWrapped("Clip operation: %s", state.error.c_str());
}
} // namespace pvd::viewer
