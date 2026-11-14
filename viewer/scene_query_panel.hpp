#pragma once
#include "model.hpp"
#include "pvd/scene_queries.hpp"
#include <array>
#include <memory>

namespace pvd::viewer {
struct RenderOptions;
struct SceneQueryPanelRow {
  render::SceneQuerySource source;
  // Last visible row's screen coordinates; independent of ImGui types.
  float left = 0, top = 0, width = 0, height = 0;
};
struct SceneQueryPanelState {
  double viewport_aspect = 1.0;
  std::optional<render::SceneQueryKind> kind_filter;
  std::optional<bool> batched_filter;
  std::array<char, 192> text_filter{};
  std::string error;
  std::size_t row_count = 0, visible_row_count = 0, hit_row_count = 0,
              visible_hit_row_count = 0;
  std::optional<std::size_t> selected_hit;
  std::vector<SceneQueryPanelRow> visible_rows;
  void reset();

  // Bounded value-only UI cache. No query/instance pointers survive a draw.
  std::weak_ptr<const FrameState> cached_frame;
  const Session *cached_source = nullptr;
  std::vector<std::size_t> matching_queries;
  std::string cached_text;
  std::optional<render::SceneQueryKind> cached_kind;
  std::optional<bool> cached_batched;
  std::optional<render::SceneQuerySource> cached_selection;
};
// Content only. The host owns Begin/End, playback ticks and viewport rendering.
// Filters change the list only; they never turn transport absence into a miss.
void drawSceneQueries(Document &, OrbitCamera &, RenderOptions &,
                      SceneQueryPanelState &);
std::string sceneQuerySourceText(const render::SceneQuerySource &);
} // namespace pvd::viewer
