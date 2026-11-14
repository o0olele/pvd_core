#include "export_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include <algorithm>
#include <limits>

namespace pvd::viewer {
void draw_export_progress(ExportJob &job, bool compact) {
  try {
    const auto status = job.status();
    if (status.stage == ExportStage::idle)
      return;
    ImGui::TextUnformatted(export_stage_name(status.stage));
    if (job.running()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%.1f s", status.elapsed_seconds);
      if (!status.cancel_requested) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel export"))
          job.cancel();
      } else {
        ImGui::SameLine();
        ImGui::TextDisabled("Cancellation requested");
      }
      if (!compact) {
        ImGui::ProgressBar(
            -static_cast<float>(ImGui::GetTime()) * .5f, {0, 0},
            status.cancel_requested
                ? "Stopping after the current preparation step..."
                : status.message.c_str());
      }
    }
    if (!compact) {
      if (!status.destination.empty())
        ImGui::TextWrapped("%s", status.destination.c_str());
      if (status.stage == ExportStage::completed && status.has_result)
        ImGui::TextWrapped(
            "Saved %zu events in %zu groups (%llu bytes).",
            status.result.event_count, status.result.group_count,
            static_cast<unsigned long long>(status.result.bytes_written));
      if (status.has_result && status.result.discarded_tail_events)
        ImGui::TextColored(
            {1, .7f, .35f, 1},
            "Excluded %zu events from an unfinished property transaction.",
            status.result.discarded_tail_events);
      if (status.stage == ExportStage::cancelled)
        ImGui::TextWrapped("Export cancelled. No new output was published.");
      if (!status.error.empty())
        ImGui::TextWrapped("Export error: %s", status.error.c_str());
      if (!status.warnings.empty()) {
        ImGui::TextColored({1, .7f, .35f, 1}, "%zu export warnings",
                           status.warnings.size());
        if (ImGui::CollapsingHeader("Export warnings")) {
          for (std::size_t i = 0;
               i < std::min<std::size_t>(status.warnings.size(), 30); ++i)
            ImGui::TextWrapped("%s", status.warnings[i].c_str());
          if (status.warnings.size() > 30)
            ImGui::TextDisabled(
                "Additional warnings are preserved in the saved recording.");
        }
      }
    } else if (!status.error.empty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted("Open Save recording for details.");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", status.error.c_str());
    }
  } catch (const std::exception &e) {
    ImGui::TextWrapped("Export status unavailable: %s", e.what());
  }
}
ExportPanelResult draw_export_panel(Document &doc, ExportJob &job,
                                    ExportPanelState &state,
                                    const ChooseExportPath &choose) {
  ExportPanelResult result;
  const auto *source = doc.session();
  const auto scene = doc.scene_id();
  std::size_t frame_count = 0;
  std::string frame_error;
  try {
    if (source && scene)
      for (const auto &frame : source->frames())
        if (frame.scene_id == *scene)
          ++frame_count;
  } catch (const std::exception &e) {
    frame_error = e.what();
  }
  if (state.source != source || state.scene_id != scene) {
    state.source = source;
    state.scene_id = scene;
    state.first_native_frame = 1;
    state.last_native_frame = static_cast<int>(
        std::min<std::size_t>(frame_count, std::numeric_limits<int>::max()));
    state.allow_partial_memory = false;
    state.error.clear();
  }
  if (!frame_error.empty())
    state.error = frame_error;
  const bool available = doc.loaded() && source != nullptr;
  const bool busy = job.running();
  ImGui::TextUnformatted("SAVE RECORDING");
  if (!available)
    ImGui::TextWrapped(
        "Open a recording before saving or choosing a frame range.");
  if (doc.live())
    ImGui::TextWrapped(
        "Save the currently imported prefix. Reception and raw recording continue; "
        "view updates resume when the background export finishes.");
  ImGui::BeginDisabled(!available || busy);
  int mode = state.mode == ExportMode::whole ? 0 : 1;
  ImGui::RadioButton("Whole recording", &mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Native frame range", &mode, 1);
  state.mode = mode == 0 ? ExportMode::whole : ExportMode::native_frame_range;
  if (state.mode == ExportMode::whole) {
    ImGui::TextWrapped(
        "Save the complete safe event prefix as a new raw .pxd2 recording. An "
        "unfinished property transaction is excluded and reported.");
  } else {
    if (scene)
      ImGui::TextDisabled("Current scene %llu  |  %zu native SDK frames",
                          static_cast<unsigned long long>(*scene), frame_count);
    else
      ImGui::TextDisabled("Select a scene with native SDK frames.");
    ImGui::SetNextItemWidth(ui_pixels(110));
    ImGui::InputInt("First frame", &state.first_native_frame);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_pixels(110));
    ImGui::InputInt("Last frame", &state.last_native_frame);
    const int maximum = static_cast<int>(
        std::min<std::size_t>(frame_count, std::numeric_limits<int>::max()));
    if (maximum) {
      state.first_native_frame =
          std::clamp(state.first_native_frame, 1, maximum);
      state.last_native_frame = std::clamp(state.last_native_frame, 1, maximum);
    }
    ImGui::TextWrapped("Frame numbers start at 1. Other scenes and streams "
                       "inside this interval are retained.");
    ImGui::TextWrapped(
        "Memory totals and allocation generations restart at the cropped "
        "live-allocation baseline. Earlier memory history is excluded.");
    ImGui::Checkbox("Allow incomplete memory baseline",
                    &state.allow_partial_memory);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Strict by default. Partial mode accepts only frees "
                       "with no observed allocation.");
    ImGui::PopStyleColor();
    if (state.allow_partial_memory)
      ImGui::TextWrapped(
          "Partial memory warnings will be kept in the saved recording. "
          "Malformed or unsupported state still prevents cropping.");
  }
  const bool range_valid =
      state.mode == ExportMode::whole ||
      (scene && frame_count &&
       state.first_native_frame <= state.last_native_frame);
  ImGui::BeginDisabled(!range_valid || !choose);
  const bool start = ImGui::Button("Choose output and save...");
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (start && source) {
    try {
      const auto destination =
          choose(state.mode == ExportMode::whole ? "capture.pxd2"
                                                 : "capture-crop.pxd2");
      if (destination) {
        ExportRequest request;
        request.destination = destination->path;
        request.overwrite = destination->overwrite;
        request.mode = state.mode;
        request.scene_id = scene;
        if (state.mode == ExportMode::native_frame_range) {
          request.first_native_frame =
              static_cast<std::size_t>(state.first_native_frame - 1);
          request.last_native_frame =
              static_cast<std::size_t>(state.last_native_frame - 1);
          request.allow_partial_memory = state.allow_partial_memory;
        }
        job.start(*source, std::move(request));
        state.error.clear();
        result.started = true;
      }
    } catch (const std::exception &e) {
      state.error = e.what();
    }
  }
  if (!state.error.empty())
    ImGui::TextWrapped("Export setup error: %s", state.error.c_str());
  ImGui::Separator();
  draw_export_progress(job);
  if (ImGui::Button(busy ? "Hide" : "Close"))
    result.close_requested = true;
  return result;
}
} // namespace pvd::viewer
