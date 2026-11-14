#pragma once
#include "workspace.hpp"
#include <array>
#include <functional>

namespace pvd::viewer {
struct WorkspacePanelState {
  std::array<char, 256> bookmark_name{};
  bool bookmark_camera = true;
  std::string error;
  std::optional<SlotId> active_slot;
};
struct WorkspaceActions {
  // Host owns file dialogs/futures. target is stable even if active slot
  // changes. Empty path means choose a file; a supplied path is a
  // recent/placeholder open.
  std::function<void(SlotId target, const std::string &path)> open;
  std::function<void(SlotId)> activate;
  std::function<void(SlotId)> close;
  std::function<void(SlotId, BookmarkId)> jump_bookmark;
};
// Content-only manager (documents, recent files, active-capture bookmarks).
// Locks are respected; missing host actions fall back to workspace commands
// except open, which never performs blocking file I/O in an ImGui frame.
void draw_workspace_panel(Workspace &, WorkspacePanelState &,
                          const WorkspaceActions & = {});
// Optional document tab strip; host owns placement/window and active callbacks.
void draw_workspace_tabs(Workspace &, const WorkspaceActions & = {});
} // namespace pvd::viewer
