#include "repx_panel.hpp"
#include "imgui.h"
#include <algorithm>

namespace pvd::viewer {
namespace {
repx::ObjectKey key(ObjectKey k) { return {k.id, k.generation}; }
repx::AttachmentKey attachment(PickingKey k) {
  return {key(k.scene), key(k.actor), key(k.shape)};
}
}
RepxRequest make_repx_request(const Document &doc, RepxScope scope,
                             bool partial, ExportDestination destination) {
  const auto frame = doc.frame();
  if (!doc.loaded() || !doc.session() || !frame)
    throw std::invalid_argument("Open a recording and select a frame first.");
  RepxRequest request;
  request.destination = std::move(destination.path);
  request.overwrite = destination.overwrite;
  request.event_count = frame->snapshot.event_count;
  request.options.unsupported = partial ? repx::UnsupportedPolicy::allow_partial
                                       : repx::UnsupportedPolicy::reject;
  if (scope == RepxScope::current_scene) {
    const auto scene_id = doc.scene_id();
    const auto *scene = scene_id ? frame->snapshot.instance(*scene_id) : nullptr;
    if (!scene) throw std::invalid_argument("Select a live scene in this frame.");
    request.options.scene = repx::ObjectKey{scene->id, scene->generation};
  } else if (scope == RepxScope::visible_shapes) {
    request.options.selection = repx::Selection::attachments;
    for (const auto &draw : frame->draws)
      if (draw.visible) request.options.attachments.push_back(attachment(draw.key));
    if (request.options.attachments.empty())
      throw std::invalid_argument("The current viewport has no visible rigid shapes.");
  } else if (scope == RepxScope::selection) {
    const auto selected = doc.selected_key();
    const auto inspected = doc.inspected_object();
    if (selected && (!inspected || *inspected == selected->shape)) {
      request.options.selection = repx::Selection::attachments;
      request.options.attachments.push_back(attachment(*selected));
    } else if (inspected) {
      request.options.selection = repx::Selection::objects;
      request.options.objects.push_back(key(*inspected));
    } else throw std::invalid_argument("Select an actor or a shape first.");
  } else if (scope != RepxScope::all_scenes)
    throw std::invalid_argument("Unknown RepX scope.");
  return request;
}
void draw_repx_progress(RepxJob &job, bool compact) {
  const auto status = job.status();
  if (status.stage == RepxStage::idle) return;
  ImGui::TextUnformatted(repx_stage_name(status.stage));
  if (job.running()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f s", status.elapsed_seconds);
    if (!status.cancel_requested) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Cancel RepX")) job.cancel();
    } else ImGui::TextDisabled("Cancellation requested");
    if (!compact)
      ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()) * .5f, {0, 0},
          status.stage == RepxStage::writing ? "Publishing XML atomically..."
                                            : "Reconstructing recorded objects...");
  }
  if (!compact) {
    ImGui::TextWrapped("%s", status.destination.c_str());
    if (status.stage == RepxStage::completed) {
      ImGui::TextWrapped("%s: %zu actors, %zu shapes, %zu joints, %zu materials, %zu resources (%zu bytes).",
          status.complete ? "Complete rigid collection" : "Partial rigid collection",
          status.actors, status.shapes, status.joints, status.materials, status.resources, status.xml_bytes);
      ImGui::TextDisabled("Displayed prefix: %zu source events", status.event_count);
    }
    if (status.stage == RepxStage::cancelled)
      ImGui::TextWrapped("Cancelled. No new output was published.");
    if (!status.error.empty())
      ImGui::TextWrapped("RepX error: %s", status.error.c_str());
    if (!status.diagnostics.empty()) {
      ImGui::TextColored({1, .7f, .35f, 1}, "%zu diagnostic(s), preserved in XML",
                         status.diagnostics.size());
      if (ImGui::BeginChild("RepX diagnostics", {0, 95}, ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min<std::size_t>(status.diagnostics.size(), 10000)));
        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const auto &d = status.diagnostics[static_cast<std::size_t>(i)];
          ImGui::Text("%llu:%llu %s: %s", static_cast<unsigned long long>(d.object.id),
              static_cast<unsigned long long>(d.object.generation), d.property.c_str(), d.message.c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45.f);
            ImGui::TextWrapped("Object %llu, generation %llu\n%s: %s",
                static_cast<unsigned long long>(d.object.id),
                static_cast<unsigned long long>(d.object.generation),
                d.property.c_str(), d.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
          }
        }
      }
      ImGui::EndChild();
    }
  } else if (!status.error.empty() && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", status.error.c_str());
}
ExportPanelResult draw_repx_panel(Document &doc, RepxJob &job, RepxPanelState &state,
                                  const ChooseExportPath &choose) {
  ExportPanelResult result;
  if (state.source != doc.session()) {
    state = {};
    state.source = doc.session();
  }
  ImGui::TextUnformatted("EXPORT PHYSX 3.4 REPX");
  ImGui::TextWrapped("Export the displayed snapshot as an SDK-loadable rigid collection. "
                     "Profile, memory and simulation history remain in the raw recording.");
  if (doc.live()) ImGui::TextWrapped("Reception and raw recording continue while this imported prefix is frozen.");
  ImGui::BeginDisabled(job.running() || !doc.loaded());
  int scope = static_cast<int>(state.scope);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##RepX scope", &scope, "Current scene\0All scenes\0Visible rigid attachments\0Selected actor or shape\0"))
    state.scope = static_cast<RepxScope>(scope);
  ImGui::TextWrapped("Visible and selected shape scopes preserve the exact actor/shape attachment, including shared shapes.");
  ImGui::Checkbox("Allow partial export with persisted warnings", &state.allow_partial);
  ImGui::TextWrapped("Strict mode rejects unsupported content and native RepX losses. "
                     "Partial mode omits unsupported content and reports known losses; malformed geometry still fails.");
  if (doc.frame()) ImGui::TextDisabled("Displayed prefix: %zu events", doc.frame()->snapshot.event_count);
  ImGui::BeginDisabled(!choose);
  const bool start = ImGui::Button("Choose XML output and export...");
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (start) try {
    // Validate selection before opening the host dialog.
    auto request = make_repx_request(doc, state.scope, state.allow_partial, {});
    auto destination = choose("snapshot.repx");
    if (destination) {
      request.destination = std::move(destination->path);
      request.overwrite = destination->overwrite;
      job.start(*doc.session(), std::move(request));
      state.error.clear();
      result.started = true;
    }
  } catch (const std::exception &e) { state.error = e.what(); }
  if (!state.error.empty()) ImGui::TextWrapped("RepX setup error: %s", state.error.c_str());
  ImGui::Separator();
  draw_repx_progress(job);
  if (ImGui::Button(job.running() ? "Hide" : "Close")) result.close_requested = true;
  return result;
}
} // namespace pvd::viewer
