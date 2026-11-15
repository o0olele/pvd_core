// Actual ImGui panels, scaled fonts/style and physical mouse coordinates.
// No OS window, renderer or SDK is required. Chooser callbacks cancel without
// publishing files; the actual content-only widgets and model commands run.
#include "analysis_panels.hpp"
#include "curve_panels.hpp"
#include "deformable_panel.hpp"
#include "display_panel.hpp"
#include "export_panel.hpp"
#include "imgui_internal.h"
#include "panels.hpp"
#include "profile_curve_panel.hpp"
#include "repx_panel.hpp"
#include "scene_query_fixture.hpp"
#include "scene_query_panel.hpp"
#include "scene_tools.hpp"
#include "ui_scale.hpp"
#include "workspace_panel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
namespace a = pvd::analysis;
void check(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture : scene_query_test::Fixture {
  static constexpr std::uint64_t cloth = 6000, fabric = 6001, memory = 8000,
                                 zone = 9000;
  using Writer = scene_query_test::BytesWriter;
  Bytes integer(std::uint64_t value, std::size_t width = 4) {
    Writer w(header.byte_order);
    w.u(value, width);
    return w.bytes;
  }
  void reference(std::uint64_t object, const char *property,
                 std::uint64_t value) {
    set(object, property, "VoidPtr", integer(value, 8), 1);
  }
  void profile_event(Writer &w, unsigned tag, std::uint64_t ticks) {
    w.u(tag, 1);
    w.u(15, 1);
    w.u(1, 2);
    w.u(7, 4);
    w.u(777, 8);
    w.u(0, 1);
    w.u(0, 1);
    w.u(ticks, 8);
  }
  void memory_string(Writer &w, const char *text, unsigned handle) {
    const std::string value(text);
    w.u(0x3ff1, 2);
    w.u(value.size() + 1, 4);
    w.text(value);
    w.u(handle, 4);
  }
  Fixture() {
    header.timestamp_numerator = 1000;
    header.timestamp_denominator = 1;
    for (const char *name :
         {"PxRigidDynamic", "PxCloth", "PxClothFabric", "PxClothParticle",
          "PxClothFabricPhase", "MemoryBuffer"})
      emit(CreateClass{type(name)});
    emit(DeriveClass{type("PxActor"), type("PxRigidDynamic")});
    property("PxRigidDynamic", "Shapes", "ObjectRef", 2);
    property("PxRigidDynamic", "GlobalPose", "PxTransform");
    property("PxRigidDynamic", "RigidBodyFlags", "PvdU8");
    property("PxRigidDynamic", "Counter", "PvdU64");
    property("PxShape", "Geometry", "ObjectRef");
    property("PxShape", "LocalPose", "PxTransform");
    property("PxShape", "Flags", "PvdU8");
    emit(DestroyInstance{actor});
    emit(CreateInstance{type("PxRigidDynamic"), actor});
    actor_scene(scene);
    set(actor, "RigidBodyFlags", "PvdU8", {0}, 1);
    reference(actor, "Shapes", shape);
    reference(shape, "Geometry", box);
    Writer local(header.byte_order);
    local.pose({0, 0, 0});
    set(shape, "LocalPose", "PxTransform", local.bytes, 1);
    set(shape, "Flags", "PvdU8", {3}, 1);

    property("PxCloth", "Scene", "ObjectRef");
    property("PxCloth", "Fabric", "ObjectRef");
    property("PxCloth", "GlobalPose", "PxTransform");
    property("PxCloth", "NbParticles", "PvdU32");
    property("PxCloth", "ParticleBuffer", "PxClothParticle", 2);
    property("PxClothParticle", "Pos", "PxVec3");
    property("PxClothParticle", "InvWeight", "PvdF32");
    property("PxClothFabric", "NbParticles", "PvdU32");
    property("PxClothFabric", "ParticleIndices", "PvdU32", 2);
    property("PxClothFabric", "Restvalues", "PvdF32", 2);
    property("PxClothFabric", "Sets", "PvdU32", 2);
    property("PxClothFabric", "Phases", "PxClothFabricPhase", 2);
    property("PxClothFabricPhase", "PhaseType", "PvdU32");
    property("PxClothFabricPhase", "SetIndex", "PvdU32");
    emit(CreateInstance{type("PxClothFabric"), fabric});
    emit(CreateInstance{type("PxCloth"), cloth});
    reference(cloth, "Scene", scene);
    reference(cloth, "Fabric", fabric);
    set(cloth, "GlobalPose", "PxTransform", local.bytes, 1);
    set(cloth, "NbParticles", "PvdU32", integer(2), 1);
    set(fabric, "NbParticles", "PvdU32", integer(2), 1);
    Writer particles(header.byte_order), indices(header.byte_order),
        rest(header.byte_order), phase(header.byte_order);
    particles.v({0, 0, 0});
    particles.f(0);
    particles.v({1, 0, 0});
    particles.f(1);
    indices.u(0, 4);
    indices.u(1, 4);
    rest.f(1);
    phase.u(1, 4);
    phase.u(0, 4);
    set(cloth, "ParticleBuffer", "PxClothParticle", particles.bytes, 2);
    set(fabric, "ParticleIndices", "PvdU32", indices.bytes, 2);
    set(fabric, "Restvalues", "PvdF32", rest.bytes, 1);
    set(fabric, "Sets", "PvdU32", integer(1), 1);
    set(fabric, "Phases", "PxClothFabricPhase", phase.bytes, 1);

    const WireName profile_class{str("_debugger_"), str("PxProfileZone")};
    emit(CreateClass{profile_class});
    const auto events_property = str("events");
    const auto profile_semantic = str("profile event stream");
    const auto byte_type = type("PvdU8");
    emit(CreateProperty{
        profile_class, events_property, profile_semantic, byte_type, 2, {}});
    emit(AddProfileZone{zone, "Scaled CPU zone"});
    emit(AddProfileZoneEvent{zone, "Scaled work", 1, true});
    emit(CreateInstance{profile_class, zone});
    emit(CreateProperty{type("MemoryBuffer"),
                        events_property,
                        str("memory event stream"),
                        byte_type,
                        2,
                        {}});
    emit(CreateInstance{type("MemoryBuffer"), memory});
    const auto frame = str("frame");
    for (unsigned i = 0; i < 4; ++i) {
      emit(BeginSection{scene, frame, i * 100});
      Writer pose(header.byte_order), profile(header.byte_order),
          allocation(header.byte_order);
      pose.pose({0, float(i), 0});
      set(actor, "GlobalPose", "PxTransform", pose.bytes, 1);
      set(actor, "Counter", "PvdU64", integer(9007199254740993ull + i, 8), 1);
      profile_event(profile, 1, i * 100 + 1);
      profile_event(profile, 2, i * 100 + 11 + i);
      set(zone, "events", "PvdU8", profile.bytes,
          static_cast<std::uint32_t>(profile.bytes.size()));
      if (!i) {
        memory_string(allocation, "ScaledType", 1);
        memory_string(allocation, "scaled.cpp", 2);
      }
      allocation.u(0x3ff2, 2);
      allocation.u(64 + i, 4);
      allocation.u(1, 4);
      allocation.u(2, 4);
      allocation.u(42, 4);
      allocation.u(0x10000 + i * 16, 8);
      set(memory, "events", "PvdU8", allocation.bytes,
          static_cast<std::uint32_t>(allocation.bytes.size()));
      publish();
      emit(ErrorMessage{2, "SCALED_CAPTURE_MESSAGE", "scaled.cpp", i + 1});
      emit(EndSection{scene, frame, i * 100 + 90});
    }
  }
  std::unique_ptr<Document> document() const {
    auto doc = std::make_unique<Document>();
    doc->adopt(std::make_unique<Session>(header, events),
               "Scaled panel fixture");
    check(doc->set_scene(scene) && doc->set_frame(0), "fixture timeline");
    check(doc->frame()->draws.size() == 1 &&
              doc->frame()->deformables.cloths.size() == 1 &&
              doc->frame()->scene_queries.queries.size() == 4,
          "real extraction supplies rigid/native/query panel data: " +
              std::to_string(doc->frame()->draws.size()) + "/" +
              std::to_string(doc->frame()->deformables.cloths.size()) + "/" +
              std::to_string(doc->frame()->scene_queries.queries.size()));
    return doc;
  }
};
struct Metrics {
  std::map<std::string, float> baseline;
  void same(const std::string &name, float physical, float scale) {
    const auto logical = physical / scale;
    check(std::isfinite(logical) && logical > 0, name + " is finite/visible");
    if (scale == 1)
      baseline[name] = logical;
    else {
      check(baseline.count(name), "1x baseline exists");
      const auto expected = baseline.at(name);
      check(std::abs(logical - expected) <= std::max(3.f, expected * .04f),
            name + " retains its logical size at scale " +
                std::to_string(scale));
    }
  }
};
struct Harness {
  std::string name, text;
  float scale, overflow = 0;
  std::function<void()> draw;
  ImGuiWindow *window = nullptr;
  Harness(std::string n, float s, std::function<void()> callback)
      : name(std::move(n)), scale(s), draw(std::move(callback)) {}
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
    UiPixelScope scope(scale);
    auto &io = ImGui::GetIO();
    io.DisplaySize = {1060 * scale, 700 * scale};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(name.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::LogToBuffer();
    draw();
    text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    window = ImGui::GetCurrentWindow();
    overflow =
        std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
    ImGui::End();
    ImGui::Render();
    const auto *data = ImGui::GetDrawData();
    check(data && data->Valid && data->TotalVtxCount > 0,
          name + " produces actual glyph/shape draw data");
    for (const auto *list : data->CmdLists)
      for (const auto &vertex : list->VtxBuffer)
        check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) &&
                  std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y),
              name + " has no nonfinite text or canvas vertices");
    check(text.find("Analysis error:") == std::string::npos &&
              text.find("Action unavailable") == std::string::npos,
          name + " does not surface a panel exception");
    if (overflow > 2 * scale)
      std::cerr << name << " at " << scale << ": " << text << '\n';
    check(overflow <= 2 * scale, name + " overflows logical 1060px width by " +
                                     std::to_string(overflow / scale));
  }
  void key(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frame();
    ImGui::GetIO().AddKeyEvent(key, false);
    frame();
  }
  ImVec2 focus(ImGuiWindow *target, const char *label) {
    frame();
    const auto id = target->GetID(label);
    for (unsigned i = 0; i < 80; ++i) {
      auto &g = *ImGui::GetCurrentContext();
      if (g.NavWindow == target && g.NavId == id) {
        return ImGui::WindowRectRelToAbs(
            target, target->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
      }
      key(ImGuiKey_Tab);
    }
    throw std::runtime_error(name + " could not focus " + label);
  }
  ImVec2 focus(const char *label) { return focus(window, label); }
  void click(ImVec2 point) {
    frame(point, false);
    frame(point, true);
    frame(point, false);
    frame(point, false);
  }
  void click(const char *label) { click(focus(label)); }
  void choose(const char *label, int row) {
    click(label);
    frame();
    const auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
    check(stack.Size && stack.back().Window, name + " opens actual combo");
    const auto *popup = stack.back().Window;
    const auto p = popup->DC.CursorStartPos;
    click({p.x + 15 * scale, p.y + ImGui::GetTextLineHeight() * .5f +
                                 row * ImGui::GetTextLineHeightWithSpacing()});
  }
  void stable() {
    frame();
    frame();
  }
};
void timeline_interactions(Harness &ui, Document &doc, Metrics &metrics) {
  check(doc.timeline().size() >= 4,
        "timeline interaction fixture has distinct exact seek targets");
  doc.set_playing(false);
  ui.focus("##frame");
  const auto frame_id = ui.window->GetID("##frame");
  const auto rect = ImGui::WindowRectRelToAbs(
      ui.window, ui.window->NavRectRel[ImGuiNavLayer_Main]);
  metrics.same("Timeline scrubber width", rect.GetWidth(), ui.scale);
  metrics.same("Timeline scrubber height", rect.GetHeight(), ui.scale);
  const auto center = rect.GetCenter();
  const ImVec2 first{rect.Min.x + ui.scale, center.y},
               last{rect.Max.x - ui.scale, center.y};
  // Native Inputable sliders enter exact text mode when reached by Tab.
  // Leave that mode before testing pointer dragging.
  if (ImGui::TempInputIsActive(frame_id))
    ui.key(ImGuiKey_Escape);

  // These are physical pointer drags through the submitted widget rectangle,
  // not calls to the model's seek command or a duplicate slider calculation.
  ui.frame(center, false);
  ui.frame(center, true);
  ui.frame(first, true);
  ui.frame(first, false);
  ui.frame(first, false);
  check(doc.frame_index() == 0,
        "scaled thin-track drag reaches the first captured frame: index=" +
            std::to_string(doc.frame_index()) + " editing=" +
            std::to_string(ImGui::TempInputIsActive(frame_id)));
  ui.frame(first, true);
  check(ImGui::GetCurrentContext()->ActiveId == frame_id,
        "thin-track accepts a new endpoint drag after mouse release");
  ui.frame(last, true);
  ui.frame(last, false);
  ui.frame(last, false);
  check(doc.frame_index() + 1 == doc.timeline().size(),
        "scaled thin-track drag reaches the last captured frame: index=" +
            std::to_string(doc.frame_index()) + " total=" +
            std::to_string(doc.timeline().size()) + " active=" +
            std::to_string(ImGui::GetCurrentContext()->ActiveId) + " mouse=" +
            std::to_string(ImGui::GetIO().MousePos.x) + "," +
            std::to_string(ImGui::GetIO().MousePos.y));

  doc.set_frame(1);
  ui.frame();
  // A pointer-selected NavId does not yet show ImGui's keyboard cursor. Use a
  // real Tab traversal before activating the focused slider for arrow tweaks.
  ui.key(ImGuiKey_Tab);
  ui.focus("##frame");
  if (ImGui::TempInputIsActive(frame_id))
    ui.key(ImGuiKey_Escape);
  ui.key(ImGuiKey_Space);
  check(ImGui::GetCurrentContext()->ActiveId == frame_id,
        "scaled scrubber accepts keyboard activation");
  ui.key(ImGuiKey_RightArrow);
  check(doc.frame_index() == 2,
        "scaled scrubber keyboard Right advances exactly one frame");
  ui.key(ImGuiKey_LeftArrow);
  check(doc.frame_index() == 1,
        "scaled scrubber keyboard Left returns exactly one frame");
  ui.key(ImGuiKey_Escape);

  const auto exact_seek = [&](const char *number) {
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    ui.frame(center, false);
    ui.frame(center, true);
    ui.frame(center, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    ui.frame(center, false);
    check(ImGui::TempInputIsActive(frame_id),
          "scaled Ctrl-click opens the scrubber's native exact input");
    io.AddInputCharactersUTF8(number);
    ui.frame();
    ui.key(ImGuiKey_Enter);
    check(!ImGui::TempInputIsActive(frame_id),
          "Enter commits and closes the exact frame input");
  };
  exact_seek("3");
  check(doc.frame_index() == 2,
        "scaled exact input maps displayed frame 3 to captured index 2");
  exact_seek("999");
  check(doc.frame_index() + 1 == doc.timeline().size(),
        "exact frame input clamps to the last real captured frame");
  exact_seek("0");
  check(doc.frame_index() == 0,
        "exact frame input clamps to the first real captured frame");
}
void run(float scale, Metrics &metrics) {
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  UiScale fonts(ImGui::GetStyle());
  fonts.reload_fonts(scale);
  Fixture fixture;
  auto source = fixture.document();
  auto &doc = *source;
  const auto captured_pose = doc.frame()
                                 ->snapshot.instance(fixture.actor)
                                 ->property("GlobalPose")
                                 ->raw_bytes();
  const auto captured_events = doc.session()->event_count();
  PanelState panel;
  OrbitCamera camera;
  RenderOptions options;
  RenderStats stats;
  DisplayPanelState display;
  Harness display_ui("Scaled display", scale, [&] {
    drawDisplaySettings(doc, camera, options, stats, display);
  });
  display_ui.stable();
  display_ui.focus("Antialiasing");
  metrics.same("AA control", display_ui.window->NavRectRel[0].GetWidth(),
               scale);
  display_ui.choose("Antialiasing", 1);
  check(options.antialiasing == Antialiasing::msaa4x,
        "scaled AA combo changes option");
  display_ui.choose("Up axis", 4);
  check(display_up(camera) == DisplayUp::positive_z,
        "scaled Up combo is operable");

  Harness inspector("Scaled Inspector", scale,
                    [&] { drawInspector(doc, panel); });
  check(doc.inspect(fixture.actor), "inspect synthetic source actor");
  inspector.stable();
  inspector.click("All properties");
  inspector.stable();
  check(inspector.text.find("9007199254740993") != std::string::npos,
        "scaled Inspector keeps exact 64bit source values");
  const float settings_width = 24.f * scale;
  const ImVec2 settings_button{
      inspector.window->WorkRect.Max.x - settings_width * .5f,
      inspector.window->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * .5f};
  inspector.frame(settings_button, false);
  check(ImGui::GetCurrentContext()->HoveredId ==
            inspector.window->GetID("Settings##Inspector settings"),
        "scaled settings button remains mouse-accessible in the header");
  inspector.click(settings_button);
  inspector.frame();
  auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
  check(popups.Size && popups.back().Window,
        "scaled Inspector preferences popup");
  metrics.same("Inspector popup", popups.back().Window->Size.x, scale);
  inspector.key(ImGuiKey_Escape);
  Harness timeline("Scaled timeline", scale, [&] { drawTimeline(doc, panel); });
  timeline.stable();
  timeline.click("...##playback-options");
  timeline.frame();
  auto &playback_popups = ImGui::GetCurrentContext()->OpenPopupStack;
  check(playback_popups.Size && playback_popups.back().Window,
        "scaled timeline playback options popup");
  timeline.click(timeline.focus(playback_popups.back().Window, "Reverse"));
  check(doc.playback().frames_per_second < 0,
        "scaled reverse checkbox changes direction");
  timeline.key(ImGuiKey_Escape);
  doc.set_frame(2);
  doc.set_playing(true);
  doc.advance(1. / 60);
  check(doc.frame_index() == 1,
        "scaled timeline causes actual reverse playback");
  doc.set_playing(false);
  timeline_interactions(timeline, doc, metrics);
  doc.set_frame(0);
  Harness hierarchy("Scaled hierarchy", scale,
                    [&] { drawHierarchy(doc, panel); });
  hierarchy.stable();
  check(hierarchy.text.find("PxCloth") != std::string::npos,
        "native class remains visible");

  SceneToolState scene_tools;
  Harness tools_ui("Scaled scene tools", scale,
                   [&] { drawSceneTools(doc, camera, scene_tools); });
  tools_ui.stable();
  tools_ui.choose("Color mode", 4);
  check(doc.view_options().color_mode == ColorMode::uniform,
        "scaled scene color combo");
  DeformablePanelState deformable;
  Harness native_ui("Scaled native", scale,
                    [&] { drawDeformables(doc, camera, options, deformable); });
  native_ui.stable();
  native_ui.click("Cloth");
  check(!options.show_cloth, "scaled cloth toggle controls viewport option");

  SceneQueryPanelState queries;
  Harness query_ui("Scaled queries", scale,
                   [&] { drawSceneQueries(doc, camera, options, queries); });
  query_ui.stable();
  const auto all_queries = queries.row_count;
  check(all_queries == 4 && queries.visible_rows.size() == 4,
        "query list is visible at each DPI");
  metrics.same("Query row", queries.visible_rows.front().height, scale);
  query_ui.choose("Kind", 1);
  check(queries.row_count == 2 &&
            queries.kind_filter == render::SceneQueryKind::raycast,
        "scaled query filter actually narrows source rows");
  const auto query_row = queries.visible_rows.front();
  query_ui.click(
      {query_row.left + 20 * scale, query_row.top + query_row.height * .5f});
  check(doc.selected_query().has_value(),
        "scaled physical click selects exact query");
  doc.clear_selection();

  ExportJob export_job;
  ExportPanelState exporting;
  unsigned choices = 0;
  const ChooseExportPath cancel_dialog = [&](const char *) {
    ++choices;
    return std::optional<ExportDestination>{};
  };
  Harness export_ui("Scaled save", scale, [&] {
    draw_export_panel(doc, export_job, exporting, cancel_dialog);
  });
  export_ui.stable();
  export_ui.click("Native frame range");
  check(exporting.mode == ExportMode::native_frame_range,
        "scaled crop range selector");
  export_ui.click("Choose output and save...");
  check(choices == 1 && !export_job.running(),
        "scaled save invokes cancellable host chooser");
  RepxJob repx_job;
  RepxPanelState repx;
  Harness repx_ui("Scaled RepX", scale,
                  [&] { draw_repx_panel(doc, repx_job, repx, cancel_dialog); });
  repx_ui.stable();
  repx_ui.choose("##RepX scope", 2);
  check(repx.scope == RepxScope::visible_shapes, "scaled RepX scope combo");
  repx_ui.click("Choose XML output and export...");
  check(choices == 2 && !repx_job.running(),
        "scaled RepX selection reaches host chooser");

  Workspace workspace;
  auto slot =
      workspace.add(fixture.document(), {}, "Scaled workspace document");
  workspace.activate(slot);
  WorkspacePanelState workspace_state;
  Harness workspace_ui("Scaled workspace", scale, [&] {
    draw_workspace_panel(workspace, workspace_state);
  });
  workspace_ui.stable();
  workspace_ui.click("Add current frame");
  check(workspace.active()->bookmarks().size() == 1,
        "scaled workspace bookmark button");

  AnalysisPanels analysis;
  AnalysisPage page = AnalysisPage::profile;
  const CsvExport no_csv = [](const char *,
                              const std::function<void(std::ostream &)> &) {};
  Harness analysis_ui("Scaled analysis", scale,
                      [&] { analysis.draw(doc, page, no_csv); });
  analysis_ui.stable();
  check(analysis.row_count(page) == 4 &&
            analysis_ui.text.find("4 completed intervals") != std::string::npos,
        "scaled profiler renders actual CPU interval timeline");
  analysis_ui.choose("##thread", 1);
  check(analysis.row_count(page) == 4,
        "scaled thread filter retains matching source intervals");
  page = AnalysisPage::memory;
  analysis_ui.stable();
  check(analysis.visible_row_count(page) == 1,
        "scaled memory shows selected-prefix allocation");
  analysis_ui.choose("Group by", 1);
  check(analysis_ui.text.find("scaled.cpp") != std::string::npos,
        "scaled memory grouping remains readable");
  page = AnalysisPage::errors;
  analysis_ui.stable();
  check(analysis.row_count(page) == 4,
        "scaled errors show actual source messages");
  analysis_ui.click("Up to current frame");
  check(analysis.visible_row_count(page) == 1,
        "scaled error filter checkbox changes prefix");

  CurvePanels curves;
  check(doc.inspect(fixture.actor), "inspect field curve source");
  curves.add_inspected(doc, "Counter");
  Harness curves_ui("Scaled curves", scale,
                    [&] { curves.draw(doc, CurvePage::curves, no_csv); });
  curves_ui.stable();
  const auto plot = curves.plot_view();
  check(plot.valid && curves.visible_row_count() == 4,
        "scaled field curve has actual numeric samples");
  metrics.same("Field plot width", plot.width, scale);
  metrics.same("Field plot height", plot.height, scale);
  curves_ui.click("Time axis");
  check(curves.plot_view().time_axis, "scaled field curve axis checkbox");
  std::ostringstream csv;
  a::write_curve_csv(csv, *curves.series(0));
  check(csv.str().find("9007199254740993") != std::string::npos,
        "DPI never changes exact source curve values");

  ProfileCurvePanel profile_curves;
  a::ProfileCurveRequest request;
  request.filter.zone = profile::ZoneKey{Fixture::zone, 0};
  request.coverage = a::ProfileCoverage::assume_continuous;
  profile_curves.add_series(doc, request);
  Harness profile_ui("Scaled CPU curves", scale,
                     [&] { profile_curves.draw(doc, no_csv); });
  profile_ui.stable();
  check(profile_curves.plot_view().valid &&
            profile_curves.visible_row_count() == 4 &&
            profile_curves.error().empty(),
        "scaled CPU curve contains actual duration samples");
  metrics.same("CPU plot width", profile_curves.plot_view().width, scale);
  metrics.same("CPU plot height", profile_curves.plot_view().height, scale);
  profile_ui.click("Time axis");
  check(profile_curves.plot_view().time_axis, "scaled CPU curve axis checkbox");
  check(doc.session()->event_count() == captured_events &&
            doc.frame()
                    ->snapshot.instance(fixture.actor)
                    ->property("GlobalPose")
                    ->raw_bytes() == captured_pose,
        "scaled controls never change source event count or world coordinates");
  std::cout << "Real ImGui panels at scale " << scale << " passed\n";
  ImGui::DestroyContext();
  check(current_ui_scale() == 1, "panels restore default headless pixel scope");
}
} // namespace
int main() {
  try {
    Metrics metrics;
    for (float scale : {1.f, 1.5f, 2.f})
      run(scale, metrics);
    std::cout << "Scaled panel workflow tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
