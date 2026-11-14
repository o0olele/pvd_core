#include "workspace_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace pvd::viewer {
namespace {
std::string fit(const std::string &text, float width) {
  std::size_t end = std::min<std::size_t>(text.size(), 96);
  while (end < text.size() && end &&
         ((static_cast<unsigned char>(text[end]) & 0xc0) == 0x80))
    --end;
  std::string s = text.substr(0, end);
  while (!s.empty() && ImGui::CalcTextSize(
                           (s + (s.size() < text.size() ? "..." : "")).c_str())
                               .x > width) {
    auto pos = s.size() - 1;
    while (pos && ((static_cast<unsigned char>(s[pos]) & 0xc0) == 0x80))
      --pos;
    s.resize(pos);
  }
  if (s.size() < text.size())
    s += "...";
  return s;
}
bool literal_selectable(const char *id, const std::string &label,
                        bool selected = false) {
  const auto position = ImGui::GetCursorScreenPos();
  const bool result = ImGui::Selectable(id, selected);
  ImGui::GetWindowDrawList()->AddText(
      position, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
  return result;
}
void activate(Workspace &workspace, const WorkspaceActions &actions,
              SlotId id) {
  if (actions.activate)
    actions.activate(id);
  else
    workspace.activate(id);
}
void close(Workspace &workspace, const WorkspaceActions &actions, SlotId id) {
  if (actions.close)
    actions.close(id);
  else
    workspace.close(id);
}
const char *sample_name(SamplePoint sample) {
  return sample == SamplePoint::native_frame ? "Native end" : "After fetch";
}
} // namespace
void draw_workspace_tabs(Workspace &workspace,
                         const WorkspaceActions &actions) {
  std::optional<SlotId> selected, closed;
  const auto active = workspace.active_id().value_or(0);
  auto *storage = ImGui::GetStateStorage();
  const auto low_key = ImGui::GetID("workspace-active-low"),
             high_key = ImGui::GetID("workspace-active-high");
  const auto previous =
      std::uint64_t(std::uint32_t(storage->GetInt(low_key))) |
      (std::uint64_t(std::uint32_t(storage->GetInt(high_key))) << 32);
  const bool request_selection = previous != active;
  if (ImGui::BeginTabBar("workspace-document-tabs",
                         ImGuiTabBarFlags_FittingPolicyResizeDown |
                             ImGuiTabBarFlags_TabListPopupButton)) {
    for (const auto *slot : workspace.slots()) {
      bool open = true;
      const auto label = fit(slot->title(), ui_pixels(180)) + "###workspace-slot-" +
                         std::to_string(slot->id());
      const auto flags =
          request_selection && workspace.active_id() == slot->id()
              ? ImGuiTabItemFlags_SetSelected
              : ImGuiTabItemFlags_None;
      const bool shown = ImGui::BeginTabItem(
          label.c_str(), slot->lock_reason() ? nullptr : &open, flags);
      if (ImGui::IsItemClicked())
        selected = slot->id();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s%s", slot->title().c_str(),
                          slot->source_path().c_str(),
                          slot->lock_reason()
                              ? "\nLocked while loading, receiving or exporting"
                              : "");
      if (shown)
        ImGui::EndTabItem();
      if (!open)
        closed = slot->id();
    }
    ImGui::EndTabBar();
  }
  storage->SetInt(low_key,
                  static_cast<int>(static_cast<std::uint32_t>(active)));
  storage->SetInt(high_key,
                  static_cast<int>(static_cast<std::uint32_t>(active >> 32)));
  try {
    if (selected)
      activate(workspace, actions, *selected);
    if (closed)
      close(workspace, actions, *closed);
  } catch (const std::exception &e) {
    ImGui::TextWrapped("Workspace action failed: %s", e.what());
  }
}
void draw_workspace_panel(Workspace &workspace, WorkspacePanelState &state,
                          const WorkspaceActions &actions) {
  if (state.active_slot != workspace.active_id()) {
    state.active_slot = workspace.active_id();
    state.error.clear();
  }
  ImGui::TextUnformatted("WORKSPACE");
  const bool new_slot = ImGui::Button("New tab");
  ImGui::SameLine();
  ImGui::BeginDisabled(!actions.open);
  const bool open_file = ImGui::Button("Open in new tab...");
  ImGui::EndDisabled();
  std::optional<SlotId> selected, closed, load;
  ImGui::BeginChild("workspace-documents", {0, ui_pixels(160)}, ImGuiChildFlags_Borders);
  for (const auto *slot : workspace.slots()) {
    ImGui::PushID(std::to_string(slot->id()).c_str());
    const auto label = fit(
        slot->title(), std::max(ui_pixels(60.f), ImGui::GetContentRegionAvail().x - ui_pixels(85)));
    if (literal_selectable("##document", label,
                           workspace.active_id() == slot->id()))
      selected = slot->id();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s\n%s", slot->title().c_str(),
                        slot->source_path().empty()
                            ? "Unsaved session"
                            : slot->source_path().c_str());
    if (slot->lock_reason())
      ImGui::TextDisabled("%s", slot_lock_name(*slot->lock_reason()));
    else {
      if (!slot->document().loaded() && !slot->source_path().empty()) {
        ImGui::BeginDisabled(!actions.open);
        if (ImGui::SmallButton("Load source"))
          load = slot->id();
        ImGui::EndDisabled();
        ImGui::SameLine();
      }
      if (ImGui::SmallButton("Close"))
        closed = slot->id();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  try {
    if (selected)
      activate(workspace, actions, *selected);
    if (closed)
      close(workspace, actions, *closed);
    if (load && actions.open)
      if (const auto *slot = workspace.find(*load))
        actions.open(*load, slot->source_path());
    if (new_slot || open_file) {
      auto id = workspace.create_empty("Untitled");
      activate(workspace, actions, id);
      if (open_file)
        actions.open(id, {});
    }
  } catch (const std::exception &e) {
    state.error = e.what();
  }
  if (ImGui::CollapsingHeader("Recent files")) {
    std::optional<std::string> recent;
    const auto paths = workspace.recent_files();
    ImGui::BeginDisabled(!actions.open);
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto &path = paths[i];
      ImGui::PushID(static_cast<int>(i));
      if (literal_selectable("##recent",
                             fit(path, ImGui::GetContentRegionAvail().x - ui_pixels(10))))
        recent = path;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", path.c_str());
      ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (recent)
      try {
        auto id = workspace.create_empty();
        activate(workspace, actions, id);
        actions.open(id, *recent);
      } catch (const std::exception &e) {
        state.error = e.what();
      }
    if (paths.empty())
      ImGui::TextDisabled("No recent capture files.");
  }
  auto *active = workspace.active();
  if (active) {
    ImGui::Separator();
    ImGui::TextUnformatted("CAPTURE BOOKMARKS");
    if (!active->warning().empty())
      ImGui::TextWrapped("%s", active->warning().c_str());
    if (active->source_path().empty())
      ImGui::TextWrapped("Unsaved session: bookmarks stay in memory until a "
                         "completed recording is associated with this tab.");
    const bool can_bookmark =
        active->document().loaded() && !active->document().timeline().empty();
    ImGui::BeginDisabled(!can_bookmark);
    ImGui::SetNextItemWidth(std::min(ui_pixels(260.f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##bookmark-name", "Bookmark name",
                             state.bookmark_name.data(),
                             state.bookmark_name.size());
    ImGui::Checkbox("Include camera", &state.bookmark_camera);
    const bool add = ImGui::Button("Add current frame");
    ImGui::EndDisabled();
    if (!can_bookmark)
      ImGui::TextDisabled(
          "Load a scene with completed frames to add bookmarks.");
    if (add)
      try {
        auto name = std::string(state.bookmark_name.data());
        if (name.empty())
          name = "Bookmark " + std::to_string(active->bookmarks().size() + 1);
        workspace.add_bookmark(active->id(), name, state.bookmark_camera);
        state.bookmark_name[0] = 0;
        state.error.clear();
      } catch (const std::exception &e) {
        state.error = e.what();
      }
    std::optional<BookmarkId> jump, remove;
    ImGui::BeginChild(
        "workspace-bookmarks",
        {0, std::max(ui_pixels(120.f), ImGui::GetContentRegionAvail().y - ui_pixels(45))},
        ImGuiChildFlags_Borders);
    for (const auto &book : active->bookmarks()) {
      ImGui::PushID(std::to_string(book.id).c_str());
      const bool verified = active->bookmark_verified(book.id);
      ImGui::BeginDisabled(!verified);
      if (literal_selectable(
              "##bookmark",
              fit(book.name, ImGui::GetContentRegionAvail().x - ui_pixels(12))))
        jump = book.id;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s%s", book.name.c_str(),
                          verified ? ""
                                   : "\nUnverified: capture content differs or "
                                     "has not been loaded.");
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (book.frame_index == UINT64_MAX)
        ImGui::TextWrapped("Scene %llu | %s | Frame out of range",
                           static_cast<unsigned long long>(book.scene_id),
                           sample_name(book.sample_point));
      else
        ImGui::TextWrapped(
            "Scene %llu | %s | Frame %llu",
            static_cast<unsigned long long>(book.scene_id),
            sample_name(book.sample_point),
            static_cast<unsigned long long>(book.frame_index + 1));
      ImGui::PopStyleColor();
      if (!verified)
        ImGui::TextColored({1, .7f, .35f, 1}, "Unverified bookmark");
      if (ImGui::SmallButton("Remove"))
        remove = book.id;
      ImGui::PopID();
    }
    if (active->bookmarks().empty())
      ImGui::TextDisabled("No bookmarks for this capture.");
    ImGui::EndChild();
    try {
      if (jump) {
        if (actions.jump_bookmark)
          actions.jump_bookmark(active->id(), *jump);
        else if (!workspace.jump_to_bookmark(active->id(), *jump))
          throw WorkspaceError(
              "Bookmark scene/frame is unavailable in this capture.");
        state.error.clear();
      }
      if (remove)
        workspace.remove_bookmark(active->id(), *remove);
    } catch (const std::exception &e) {
      state.error = e.what();
    }
  }
  if (!state.error.empty())
    ImGui::TextWrapped("Workspace error: %s", state.error.c_str());
}
} // namespace pvd::viewer
