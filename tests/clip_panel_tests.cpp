#include "clip_panel.hpp"
#include "clip_range_fixture.hpp"
#include "export_job.hpp"
#include "imgui_internal.h"
#include "ui_scale.hpp"
#include "workspace.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace pvd;
using namespace pvd::viewer;
namespace c = pvd::clips;
void check(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Harness {
  Document &doc;
  ClipPanelState state;
  ClipPanelActions actions;
  std::uint64_t slot = 41;
  float width, scale;
  ImGuiWindow *window = nullptr;
  std::string text;
  Harness(Document &source, float w, float s)
      : doc(source), width(w), scale(s) {}
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
    UiPixelScope pixels(scale);
    auto &io = ImGui::GetIO();
    io.DisplaySize = {width * scale, 1200 * scale};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width * scale, 1100 * scale});
    ImGui::Begin("Named clip workflow", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::LogToBuffer();
    drawClipRanges(doc, slot, state, actions);
    text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    window = ImGui::GetCurrentWindow();
    const auto overflow =
        std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
    ImGui::End();
    ImGui::Render();
    check(ImGui::GetDrawData()->TotalVtxCount > 0,
          "actual clip panel produces glyph/table draw data");
    check(overflow <= 2 * scale, "named clip panel overflows logical width: " +
                                     std::to_string(overflow / scale));
  }
  void key(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frame();
    ImGui::GetIO().AddKeyEvent(key, false);
    frame();
  }
  ImVec2 focus(const char *label) {
    frame();
    if (ImGui::GetCurrentContext()->NavWindow != window) {
      // A clicked scrollable table owns keyboard navigation. A real click on
      // the parent's noninteractive header returns focus to its controls.
      const auto padding = ImGui::GetStyle().WindowPadding;
      click({window->Pos.x + padding.x + 2 * scale,
             window->Pos.y + padding.y + 2 * scale});
    }
    const auto id = window->GetID(label);
    for (unsigned i = 0; i < 96; ++i) {
      const auto &g = *ImGui::GetCurrentContext();
      if (g.NavWindow == window && g.NavId == id) {
        const auto p = window->NavRectRel[ImGuiNavLayer_Main].GetCenter();
        return {p.x + window->Pos.x, p.y + window->Pos.y};
      }
      key(ImGuiKey_Tab);
    }
    std::cerr << "Panel focus state: " << text << '\n';
    const auto &debug = *ImGui::GetCurrentContext();
    std::cerr << "Nav window="
              << (debug.NavWindow ? debug.NavWindow->Name : "none")
              << " nav=" << debug.NavId << " target=" << id << '\n';
    throw std::runtime_error(std::string("Could not focus clip control: ") +
                             label);
  }
  void click(ImVec2 p) {
    frame(p, false);
    frame(p, true);
    frame(p, false);
    frame(p, false);
  }
  void click(const char *label) { click(focus(label)); }
  c::Catalog &catalog() { return state.catalogs.at(*state.active_catalog); }
  void select_last_row() {
    frame();
    frame();
    auto *table = ImGui::GetCurrentContext()->Tables.GetByKey(
        window->GetID("##named-clips"));
    check(table && state.visible_rows > 0,
          "actual named clip table is visible");
    click({table->Columns[0].WorkMinX + 12 * scale,
           (table->RowPosY1 + table->RowPosY2) * .5f});
  }
};
void context(float scale) {
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  UiScale fonts(ImGui::GetStyle());
  fonts.reload_fonts(scale);
}
ClipSaveRequest workflow(float width, float scale) {
  clip_test::Fixture fixture;
  Document doc;
  doc.adopt(std::make_unique<Session>(fixture.header, fixture.events));
  doc.set_scene(1);
  doc.set_frame(2);
  Harness h(doc, width, scale);
  std::vector<ClipSaveRequest> saves;
  std::vector<ClipPanelChange> changes;
  h.actions.changed = [&](ClipPanelChange change) {
    changes.push_back(change);
  };
  h.actions.save_as = [&](ClipSaveRequest request) {
    saves.push_back(std::move(request));
  };
  h.frame();
  h.frame();
  check(changes.empty(), "drawing a clip panel must not persist/hash catalogs");
  h.click("Use current scene");
  check(changes.size() == 1 && changes.back().source_slot == 41 &&
            changes.back().kind == ClipPanelChangeKind::edited,
        "create emits one source-bound change callback");
  check(h.state.catalogs.size() == 1 &&
            h.catalog().selected()->frames.count() == 6,
        "actual create command produces a full selected lifetime range");
  h.click("Split at playhead");
  check(h.catalog().state().clips.size() == 2 &&
            h.catalog().state().clips[0].frames.last == 2 &&
            h.catalog().state().clips[1].frames.first == 3,
        "actual playhead split creates and retains both named ranges");
  std::snprintf(h.state.name.data(), h.state.name.size(),
                "Failure ### literal");
  h.click("Rename selected");
  check(h.catalog().selected()->name == "Failure ### literal",
        "clip names remain literal text rather than ImGui ID syntax");
  doc.set_frame(1);
  h.click("Trim left");
  h.click("Trim right");
  check(h.catalog().selected()->frames.first == 1 &&
            h.catalog().selected()->frames.count() == 1,
        "actual trim commands preserve the native playhead frame");
  h.select_last_row();
  check(h.catalog().selected()->frames.first == 3,
        "actual table mouse selection uses stable clip ID");
  doc.set_sample_point(SamplePoint::after_fetch);
  h.click("Select range");
  check(doc.sample_point() == SamplePoint::native_frame &&
            doc.frame_index() == 3,
        "select range navigates its original native source frame");
  const auto original_id = h.catalog().selected()->id;
  const auto edits_before_save = changes.size();
  h.click("Save As...");
  check(saves.size() == 1 && saves[0].source_slot == 41 &&
            saves[0].clip.frames.first == 3 && saves[0].clip.frames.last == 5,
        "Save As copies the exact range and stable source slot");
  check(changes.size() == edits_before_save,
        "Save As must not mutate/persist catalogs after taking an export lock");
  const auto save_point = h.focus("Save As...");
  h.actions.busy = true;
  h.click(save_point);
  check(saves.size() == 1 && h.catalog().selected()->id == original_id,
        "export/replacement lock prevents another save or edit");
  h.actions.busy = false;
  std::snprintf(h.state.name.data(), h.state.name.size(), "Temporary range");
  h.state.first_frame = 4;
  h.state.last_frame = 6;
  h.click("Add named range");
  check(h.catalog().state().clips.size() == 3,
        "actual add command retains a third independent named range");
  h.click("Delete selected");
  check(h.catalog().state().clips.size() == 2 && h.catalog().find(original_id),
        "actual delete command removes only the chosen named range");
  doc.set_scene(2);
  doc.set_frame(0);
  h.click("Use current scene");
  check(h.state.catalogs.size() == 2 &&
            h.catalog().selected()->frames.count() == 3,
        "another scene retains an independent named range list");
  doc.set_scene(1);
  doc.set_frame(6);
  h.click("Use current scene");
  check(
      h.state.catalogs.size() == 3 && h.state.active_catalog->generation == 1 &&
          h.catalog().selected()->frames.first == 6,
      "reused scene generation does not inherit the old clip list or ordinals");
  check(h.state.catalogs.at({1, 0}).state().clips.size() == 2,
        "old scene/lifetime fragments remain archived and named");
  h.state.needs_verification = true;
  h.click("Verify saved clips");
  check(changes.back().kind == ClipPanelChangeKind::verified &&
            !h.state.needs_verification,
        "successful batch verification emits the explicit verified callback");

  const auto before = h.state.catalogs.size();
  h.slot = 99;
  h.click(save_point);
  check(h.state.catalogs.size() == before && saves.size() == 1 &&
            h.state.source_slot == 41,
        "wrong active source slot cannot retarget saved clips");
  h.slot = 41;
  auto altered = fixture.events;
  std::get<SetPropertyValue>(altered[fixture.first_value].payload).data[0] ^= 1;
  auto replacement = std::make_unique<Session>(fixture.header, altered);
  doc.adopt(std::move(replacement), "same-path-changed-content");
  h.frame();
  check(h.state.needs_verification,
        "source replacement disables archived ranges");
  const auto edits_before_failed_verify = changes.size();
  h.click("Verify saved clips");
  check(h.state.needs_verification && !h.state.error.empty() &&
            saves.size() == 1,
        "same-count source replacement fails the sealed prefix identity check");
  check(
      changes.size() == edits_before_failed_verify,
      "failed verification does not persist foreign source metadata as active");
  h.click("Discard saved clips");
  check(h.state.catalogs.empty() && !h.state.needs_verification,
        "explicitly discarding unverified clips permits a clean retry");
  check(changes.back().kind == ClipPanelChangeKind::discarded,
        "discard exposes explicit pending-removal intent to the host");
  h.click("Use current scene");
  check(h.state.error.empty() && h.state.catalogs.size() == 1,
        "new source can create its own clips after failed verification");
  const auto canonical = workspace_source_identity(*doc.session());
  const auto independent = c::source_identity(*doc.session());
  check(canonical.algorithm == independent.algorithm &&
            canonical.event_count == independent.event_count &&
            canonical.fingerprint == independent.fingerprint,
        "standalone clip identity matches installed workspace algorithm 1");
  std::cout << "Actual clip table/buttons width=" << width << " scale=" << scale
            << " passed\n";
  return saves.front();
}
void live() {
  clip_test::Fixture fixture;
  Document doc;
  doc.begin_live(fixture.header, "Live named ranges");
  doc.append_live(std::vector<Event>(
      fixture.events.begin(), fixture.events.begin() + fixture.open_frame));
  doc.set_scene(1);
  doc.set_sample_point(SamplePoint::native_frame);
  doc.set_frame(1);
  Harness h(doc, 1060, 1);
  std::vector<ClipSaveRequest> saves;
  h.actions.save_as = [&](ClipSaveRequest request) {
    saves.push_back(std::move(request));
  };
  h.click("Use current scene");
  const auto seal = h.catalog().selected()->binding.source;
  check(h.catalog().selected()->frames.count() == 3,
        "live panel excludes still-open native frame");
  h.click("Split at playhead");
  const auto id = h.catalog().selected()->id;
  doc.append_live(std::vector<Event>(
      fixture.events.begin() + fixture.open_frame, fixture.events.end()));
  h.click("Refresh closed frames");
  check(
      h.catalog().selected()->id == id &&
          h.catalog().selected()->binding.source == seal &&
          h.catalog().state().binding.available.last == 5,
      "live append/refresh preserve old values and admit newly closed frames");
  h.click("Save As...");
  check(saves.size() == 1 && saves[0].clip.binding.source == seal && doc.live(),
        "live sealed range Save As works while reception remains active");
  doc.finish_live();
  c::validate_clip(*doc.session(), saves.front().clip);
  std::cout << "Live named clip UI/sealed-prefix save dispatch passed\n";
}
void save_roundtrip(const ClipSaveRequest &selection,
                    const std::filesystem::path &directory) {
  clip_test::Fixture fixture;
  auto source = fixture.session();
  c::validate_clip(source, selection.clip);
  std::filesystem::create_directories(directory);
  const auto path =
      directory /
      ("named-clip-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".pxd2");
  ExportRequest request;
  request.destination = path.u8string();
  request.mode = ExportMode::native_frame_range;
  request.scene_id = selection.clip.binding.scene.id;
  request.first_native_frame = selection.clip.frames.first;
  request.last_native_frame = selection.clip.frames.last;
  ExportJob job;
  job.start(source, request);
  job.wait();
  check(job.status().stage == ExportStage::completed,
        "real Save As ExportJob failed: " + job.status().error);
  auto opened = Session::load_file(path.u8string());
  std::size_t count = 0;
  for (const auto &frame : opened.frames())
    if (frame.scene_id == selection.clip.binding.scene.id)
      ++count;
  check(count == selection.clip.frames.count(),
        "saved named clip reopens exact native frame count");
  const auto &last = opened.frames().back();
  check(opened.snapshot_at_event(last.event_count)
                .instance(20)
                ->property("Counter")
                ->as_uint() == 9007199254740998ull,
        "saved named range retains its exact final 64-bit property state");
  check(source.event_count() == fixture.events.size(),
        "save job did not edit source events");
  std::filesystem::remove(path);
  std::cout << "Named clip Save As -> ExportJob -> file reopen passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc <= 2,
          "usage: pvd_clip_panel_tests [temporary_output_directory]");
    ClipSaveRequest selected;
    for (float scale : {1.f, 1.5f, 2.f}) {
      context(scale);
      selected = workflow(1060, scale);
      ImGui::DestroyContext();
    }
    context(1);
    workflow(420, 1);
    live();
    ImGui::DestroyContext();
    save_roundtrip(selected, argc == 2
                                 ? std::filesystem::u8path(argv[1])
                                 : std::filesystem::temp_directory_path());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
