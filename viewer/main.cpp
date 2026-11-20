#include "deformable_panel.hpp"
#include "display_panel.hpp"
#include "ui_scale.hpp"
#include "ui_widgets.hpp"
#include "camera_panel.hpp"
#include "recorded_camera.hpp"
#include "clip_panel.hpp"
#include "pvd/coordinate_frames.hpp"
#include "pvd/body_overlays.hpp"
#include "scene_query_panel.hpp"
#include "export_panel.hpp"
#include "imgui_internal.h"
#include "load_job.hpp"
#include "model.hpp"
#include "panels.hpp"
#include "renderer.hpp"
#include "scene_tools.hpp"
#include "workspace_panel.hpp"
#ifdef PVD_VIEWER_HAS_ANALYSIS
#include "analysis_panels.hpp"
#include "curve_panels.hpp"
#include "profile_curve_panel.hpp"
#endif
#ifdef PVD_VIEWER_HAS_TRANSPORT
#include "live_session.hpp"
#endif
#ifdef PVD_VIEWER_HAS_REPX
#include "repx_panel.hpp"
#endif
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <commdlg.h>
#include <cwchar>
#include <d3d11.h>
#include <deque>
#include <dwmapi.h>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <wrl/client.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);
using Microsoft::WRL::ComPtr;
using namespace pvd::viewer;
namespace {
struct Options {
  std::filesystem::path capture, screenshot, report;
  std::filesystem::path recording, ready;
  std::filesystem::path live_snapshot;
  std::filesystem::path output;
  bool smoke = false, warp = false, hidden = false;
  bool scene_first_smoke = false;
  bool live_smoke = false;
  bool analysis_smoke = false;
  bool deformable_smoke = false;
  bool query_smoke = false;
  bool dpi_smoke = false;
  std::optional<float> ui_scale;
  bool export_smoke = false, repx_smoke = false, allow_partial = false;
  bool workspace_smoke = false, clip_smoke = false, no_settings = false;
  std::filesystem::path workspace_state;
  std::size_t crop_first = 0, crop_last = 0;
  int analysis_page = 0;
  int listen_port = -1;
  int window_width = 1440, window_height = 900;
};
std::string utf8(const std::wstring &s) {
  if (s.empty())
    return {};
  const int n =
      WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      result.data(), n, nullptr, nullptr);
  return result;
}
std::string escaped(const std::string &text) {
  std::ostringstream out;
  for (unsigned char c : text) {
    if (c == '"' || c == '\\')
      out << '\\' << c;
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << unsigned(c) << std::dec;
    else
      out << c;
  }
  return out.str();
}
void check(HRESULT hr, const char *context) {
  if (FAILED(hr)) {
    std::ostringstream s;
    s << context << " (HRESULT 0x" << std::hex << unsigned(hr) << ')';
    throw std::runtime_error(s.str());
  }
}
Options arguments() {
  Options result;
  int count = 0;
  LPWSTR *args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!args)
    throw std::runtime_error("Cannot read command line");
  try {
    for (int i = 1; i < count; ++i) {
      std::wstring a = args[i];
      auto value = [&]() -> std::filesystem::path {
        if (++i == count)
          throw std::runtime_error("Missing option value");
        return args[i];
      };
      if (a == L"--smoke" || a == L"--layout-smoke") {
        result.smoke = true;
        result.scene_first_smoke = a == L"--layout-smoke";
        result.capture = value();
      } else if (a == L"--deformable-smoke") {
        result.deformable_smoke = true;
        result.capture = value();
      } else if (a == L"--query-smoke") {
        result.query_smoke = true;
        result.capture = value();
      } else if (a == L"--dpi-smoke") {
        result.dpi_smoke = true;
        result.capture = value();
      } else if (a == L"--ui-scale") {
        const auto text = value().wstring();
        std::size_t used = 0;
        const auto scale = std::stof(text, &used);
        if (used != text.size() || !std::isfinite(scale) || scale < .5f || scale > 4)
          throw std::runtime_error("UI scale must be a number in [0.5,4]");
        result.ui_scale = scale;
      } else if (a == L"--analysis-smoke") {
        result.analysis_smoke = true;
        result.capture = value();
        result.analysis_page = 1;
      } else if (a == L"--export-smoke") {
        result.export_smoke = true;
        result.capture = value();
      } else if (a == L"--repx-smoke") {
        result.repx_smoke = true;
        result.capture = value();
      } else if (a == L"--allow-partial") {
        result.allow_partial = true;
      } else if (a == L"--workspace-smoke") {
        result.workspace_smoke = true;
        result.capture = value();
      } else if (a == L"--clip-smoke") {
        result.clip_smoke = true;
        result.capture = value();
      } else if (a == L"--workspace-state")
        result.workspace_state = value();
      else if (a == L"--size") {
        auto number = [&]() {
          auto text = value().wstring();
          std::size_t used = 0;
          int n = std::stoi(text, &used);
          if (used != text.size())
            throw std::runtime_error("Invalid window size");
          return n;
        };
        result.window_width = number();
        result.window_height = number();
        if (result.window_width < 1060 || result.window_width > 4096 ||
            result.window_height < 700 || result.window_height > 2160)
          throw std::runtime_error(
              "Window size must be in [1060,4096] by [700,2160]");
      } else if (a == L"--no-settings")
        result.no_settings = true;
      else if (a == L"--output")
        result.output = value();
      else if (a == L"--crop") {
        result.crop_first = std::stoull(value().wstring());
        result.crop_last = std::stoull(value().wstring());
        if (!result.crop_first || result.crop_last < result.crop_first)
          throw std::runtime_error("Crop frames are inclusive and start at 1");
      } else if (a == L"--page") {
        const auto page = value().wstring();
        if (page == L"profile")
          result.analysis_page = 1;
        else if (page == L"memory")
          result.analysis_page = 2;
        else if (page == L"errors")
          result.analysis_page = 3;
        else if (page == L"curves")
          result.analysis_page = 4;
        else if (page == L"diff")
          result.analysis_page = 5;
        else if (page == L"event-curves")
          result.analysis_page = 7;
        else
          throw std::runtime_error("Unknown workspace page");
      } else if (a == L"--screenshot")
        result.screenshot = value();
      else if (a == L"--report")
        result.report = value();
      else if (a == L"--warp")
        result.warp = true;
      else if (a == L"--hidden")
        result.hidden = true;
      else if (a == L"--capture")
        result.capture = value();
      else if (a == L"--record")
        result.recording = value();
      else if (a == L"--ready")
        result.ready = value();
      else if (a == L"--live-snapshot")
        result.live_snapshot = value();
      else if (a == L"--live-smoke")
        result.live_smoke = true;
      else if (a == L"--listen") {
        const auto number = value().wstring();
        std::size_t used = 0;
        const auto port = std::stoul(number, &used);
        if (used != number.size() || port > 65535)
          throw std::runtime_error("Listen port must be in [0,65535]");
        result.listen_port = static_cast<int>(port);
      } else if (!a.empty() && a[0] != L'-' && result.capture.empty())
        result.capture = a;
      else
        throw std::runtime_error("Unknown command-line option: " + utf8(a));
    }
  } catch (...) {
    LocalFree(args);
    throw;
  }
  LocalFree(args);
  if (unsigned(result.smoke) + unsigned(result.live_smoke) +
          unsigned(result.analysis_smoke) + unsigned(result.export_smoke) +
          unsigned(result.repx_smoke) + unsigned(result.workspace_smoke) + unsigned(result.clip_smoke) +
          unsigned(result.deformable_smoke) + unsigned(result.query_smoke) + unsigned(result.dpi_smoke) >
      1)
    throw std::runtime_error("Choose one smoke mode");
  if ((result.workspace_smoke || result.clip_smoke) && result.workspace_state.empty())
    throw std::runtime_error("Workspace smoke requires --workspace-state");
  if (result.dpi_smoke && result.ui_scale)
    throw std::runtime_error("DPI transition smoke controls its own UI scale");
  if ((result.export_smoke || result.repx_smoke || result.clip_smoke) && result.output.empty())
    throw std::runtime_error("Export smoke requires --output");
  if (result.listen_port >= 0 && !result.capture.empty())
    throw std::runtime_error(
        "Choose a file or a live listener as the initial source");
  if (result.live_smoke && (result.listen_port < 0 || result.recording.empty()))
    throw std::runtime_error("Live smoke requires --listen and --record");
#ifndef PVD_VIEWER_HAS_REPX
  if (result.repx_smoke)
    throw std::runtime_error("This viewer was built without the RepX module");
#endif
#ifndef PVD_VIEWER_HAS_TRANSPORT
  if (result.listen_port >= 0 || result.live_smoke)
    throw std::runtime_error(
        "This viewer was built without the transport module");
#endif
#ifndef PVD_VIEWER_HAS_ANALYSIS
  if (result.analysis_smoke || result.analysis_page)
    throw std::runtime_error(
        "This viewer was built without the analysis module");
#endif
  return result;
}

class App {
public:
  explicit App(Options options) : options_(std::move(options)) {
    workspace_.create_empty();
  }
  ~App() {
    cancel_loads();
    export_job_.cancel();
    cancel_associations();
    load_job_.wait();
    export_job_.wait();
#ifdef PVD_VIEWER_HAS_REPX
    repx_job_.cancel();
    repx_job_.wait();
#endif
    if (bind_identity_.valid()) {
      bind_identity_.wait();
      try {
        bind_identity_.get();
      } catch (...) {
      }
      if (bind_slot_)
        workspace_.unlock(*bind_slot_, SlotLock::loading);
    }
#ifdef PVD_VIEWER_HAS_TRANSPORT
    live_.stop();
#endif
    save_automatic_settings();
    renderer_.reset();
    if (dx11_backend_)
      ImGui_ImplDX11_Shutdown();
    if (win32_backend_)
      ImGui_ImplWin32_Shutdown();
    ui_scale_.reset();
    if (imgui_)
      ImGui::DestroyContext();
    if (context_)
      context_->ClearState();
    backbuffer_.Reset();
    swapchain_.Reset();
    context_.Reset();
    device_.Reset();
    if (window_)
      DestroyWindow(window_);
    UnregisterClassW(L"PvdStudioWindow", GetModuleHandleW(nullptr));
  }
  int run() {
    initialize();
#ifdef PVD_VIEWER_HAS_TRANSPORT
    if (options_.listen_port >= 0) {
      capture_port_ = options_.listen_port;
      record_capture_ = !options_.recording.empty();
      if (record_capture_)
        copy_path(options_.recording);
      start_capture();
      if (!error_.empty())
        throw std::runtime_error(error_);
      if (!options_.ready.empty()) {
        auto temporary = options_.ready;
        temporary += L".tmp";
        {
          std::ofstream ready(temporary);
          ready << "{\"port\":" << live_.status().port << "}\n";
          if (!ready)
            throw std::runtime_error("Cannot write live listener ready file");
        }
        std::filesystem::rename(temporary, options_.ready);
      }
    }
#endif
    if (!options_.capture.empty()) {
      if (options_.smoke || options_.analysis_smoke || options_.export_smoke ||
          options_.repx_smoke || options_.deformable_smoke || options_.query_smoke || options_.dpi_smoke || options_.clip_smoke) {
        auto incoming = std::make_unique<Document>();
        const auto path =
            std::filesystem::absolute(options_.capture).lexically_normal();
        incoming->open_file(utf8(path.wstring()));
        workspace_.replace(active_slot().id(), incoming, utf8(path.wstring()));
        loaded();
      } else
        open(options_.capture);
    }
    if (options_.export_smoke) {
      ExportRequest request;
      request.destination = utf8(options_.output.wstring());
      request.allow_partial_memory = options_.allow_partial;
      if (options_.crop_first) {
        request.mode = ExportMode::native_frame_range;
        request.scene_id = document().scene_id();
        request.first_native_frame = options_.crop_first - 1;
        request.last_native_frame = options_.crop_last - 1;
      }
      start_export(active_slot().id(), request);
      export_status_visible_ = true;
      export_popup_requested_ = true;
      export_popup_slot_ = active_slot().id();
      ui().export_panel.source = document().session();
      ui().export_panel.scene_id = document().scene_id();
      ui().export_panel.mode = request.mode;
      ui().export_panel.first_native_frame =
          static_cast<int>(request.first_native_frame + 1);
      ui().export_panel.last_native_frame =
          static_cast<int>(request.last_native_frame + 1);
    }
#ifdef PVD_VIEWER_HAS_REPX
    if (options_.repx_smoke) {
      document().set_sample_point(SamplePoint::native_frame);
      if (!document().timeline().empty())
        document().set_frame(document().timeline().size() - 1);
      auto request = make_repx_request(
          document(), RepxScope::current_scene, options_.allow_partial,
          ExportDestination{utf8(options_.output.wstring()), false});
      start_repx(active_slot().id(), std::move(request));
      repx_popup_requested_ = true;
      repx_popup_slot_ = active_slot().id();
      ui().repx_panel.source = document().session();
      ui().repx_panel.allow_partial = options_.allow_partial;
    }
#endif
    const auto started = std::chrono::steady_clock::now();
    auto previous = started;
    while (!quit_) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
          quit_ = true;
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (quit_)
        break;
      poll_export();
#ifdef PVD_VIEWER_HAS_REPX
      poll_repx();
#endif
      poll_binding();
      poll_load();
#ifdef PVD_VIEWER_HAS_TRANSPORT
      poll_live();
#endif
      if (resize_width_ && resize_height_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        backbuffer_.Reset();
        check(swapchain_->ResizeBuffers(0, resize_width_, resize_height_,
                                        DXGI_FORMAT_UNKNOWN, 0),
              "Resize window");
        create_backbuffer();
        resize_width_ = resize_height_ = 0;
      }
      if (IsIconic(window_)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        continue;
      }
      auto now = std::chrono::steady_clock::now();
      try {
        document().advance(std::min(0.1, std::chrono::duration<double>(now - previous).count()));
      }catch(const std::exception &e) {
        if(smoke_mode())throw;
        document().set_playing(false);error_=e.what();
      }
      previous = now;
      if (smoke_mode() &&
          std::chrono::duration<double>(now - started).count() > 30)
        throw std::runtime_error("Viewer smoke test timed out");
      update_ui_scale();
      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      if (options_.scene_first_smoke) layout_smoke_input();
      popup_open_at_frame_start_ = ImGui::IsPopupOpen(nullptr,
          ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
      ImGui::NewFrame();
      draw();
      ImGui::Render();
      apply_closed_slots();
      const float background[] = {0.035f, 0.047f, 0.067f, 1};
      auto *target = backbuffer_.Get();
      context_->OMSetRenderTargets(1, &target, nullptr);
      context_->ClearRenderTargetView(target, background);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      ++host_frames_;
      if (options_.smoke)
        smoke_step();
      else if (options_.deformable_smoke)
        deformable_smoke_step();
      else if (options_.query_smoke)
        query_smoke_step();
      else if (options_.dpi_smoke)
        dpi_smoke_step();
      else if (options_.export_smoke)
        export_smoke_step();
      else if (options_.workspace_smoke)
        workspace_smoke_step();
      else if (options_.clip_smoke)
        clip_smoke_step();
#ifdef PVD_VIEWER_HAS_REPX
      else if (options_.repx_smoke)
        repx_smoke_step();
#endif
#ifdef PVD_VIEWER_HAS_ANALYSIS
      else if (options_.analysis_smoke)
        analysis_smoke_step();
#endif
#ifdef PVD_VIEWER_HAS_TRANSPORT
      else if (options_.live_smoke)
        live_smoke_step();
#endif
      else if (!options_.screenshot.empty() && document().loaded() &&
               host_frames_ > 8) {
        screenshot();
        options_.screenshot.clear();
      }
      const HRESULT present = swapchain_->Present(smoke_mode() ? 0 : 1, 0);
      if (FAILED(present))
        check(present, "Present frame");
      if (options_.live_smoke || present == DXGI_STATUS_OCCLUDED)
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    return 0;
  }

private:
  Options options_;
  HWND window_ = nullptr;
  bool quit_ = false, imgui_ = false, using_warp_ = false;
  bool popup_open_at_frame_start_ = false;
  bool win32_backend_ = false, dx11_backend_ = false;
  UINT resize_width_ = 0, resize_height_ = 0;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain> swapchain_;
  ComPtr<ID3D11RenderTargetView> backbuffer_;
  std::unique_ptr<Renderer> renderer_;
  struct SlotUiState {
    // Drawers overlay the scene instead of changing its camera aspect/texture.
    enum class Sidebar { none, objects, properties, diagnostics };
    Sidebar sidebar = Sidebar::none;
    ImVec2 auto_fit_size{};
    unsigned auto_fit_stable_frames = 0;
    PanelState panels;
    RenderOptions render_options;
    SceneToolState scene_tools;
    CameraNavigationPreferences navigation;
    CameraInputState camera_input;
    CameraPanelState camera_panel;
    ClipPanelState clips;
    std::uint64_t clips_revision = std::numeric_limits<std::uint64_t>::max();
    bool clips_resync = false;
    bool view_popup_requested = false;
    int view_settings_page = 2;
    DisplayPanelState display;
    SceneQueryPanelState scene_queries;
    ExportPanelState export_panel;
    bool show_debug_text = true;
    int page = 0;
    DeformablePanelState deformables;
    std::optional<std::uint64_t> displayed_scene;
#ifdef PVD_VIEWER_HAS_ANALYSIS
    AnalysisPanels analysis;
    CurvePanels curves;
    ProfileCurvePanel profile_curves;
#endif
#ifdef PVD_VIEWER_HAS_REPX
    RepxPanelState repx_panel;
#endif
  };
  Workspace workspace_;
  std::map<SlotId, std::unique_ptr<SlotUiState>> slot_ui_;
  WorkspacePanelState workspace_panel_;
  LoadJob load_job_;
  struct LoadTask {
    SlotId id;
    std::filesystem::path path;
  };
  std::deque<LoadTask> load_queue_;
  std::optional<SlotId> loading_slot_;
  bool load_status_visible_ = false;
  std::vector<SlotId> close_requests_;
  ExportJob export_job_;
  std::optional<SlotId> export_slot_id_, last_export_slot_id_;
#ifdef PVD_VIEWER_HAS_REPX
  RepxJob repx_job_;
  std::optional<SlotId> repx_slot_id_, repx_popup_slot_;
  bool repx_resume_live_ = false, repx_popup_requested_ = false,
       repx_status_visible_ = false;
#endif
  bool export_resume_live_ = false;
  ExportMode export_mode_ = ExportMode::whole;
  std::string export_destination_;
  std::optional<SlotId> export_popup_slot_;
  std::future<SourceIdentity> bind_identity_;
  std::shared_ptr<std::atomic<bool>> bind_cancel_;
  std::shared_ptr<std::atomic<std::size_t>> bind_progress_;
  std::optional<SlotId> bind_slot_;
  std::string bind_path_;
  struct BindTask {
    SlotId id;
    std::string path;
  };
  std::deque<BindTask> bind_queue_;
  std::filesystem::path settings_directory_, settings_file_;
  std::string ini_path_;
  bool automatic_settings_ = false, settings_writable_ = false,
       help_requested_ = false, activity_popup_requested_ = false;
  int layout_generation_ = 0;
  std::unique_ptr<UiScale> ui_scale_;
  std::optional<float> pending_ui_scale_;
  std::size_t dpi_smoke_phase_ = 0;
  std::vector<std::array<unsigned,6>> dpi_smoke_samples_;
  std::string notice_;
  int workspace_smoke_phase_ = 0;
  int clip_smoke_phase_ = 0;
  std::size_t clip_smoke_end_prefix_ = 0;
  std::optional<pvd::clips::Clip> clip_smoke_saved_;
  SlotId smoke_slot_a_ = 0, smoke_slot_b_ = 0;
  BookmarkId smoke_bookmark_ = 0;
  const pvd::Session *smoke_session_a_ = nullptr;
  OrbitCamera smoke_camera_a_, smoke_camera_b_;
  bool smoke_workspace_target_ = false, smoke_workspace_state_ = false,
       smoke_workspace_locks_ = false, smoke_workspace_curves_ = true;
  bool export_popup_requested_ = false, export_status_visible_ = false;
  std::string loading_, error_;
  std::size_t host_frames_ = 0;
  ImVec2 viewport_origin_{}, viewport_size_{};
  ImVec2 scene_drawer_min_{}, scene_drawer_max_{};
  ImVec2 objects_button_center_{}, properties_button_center_{}, drawer_close_center_{};
  ImVec2 selection_chip_center_{};
  ImVec2 file_menu_center_{}, more_menu_center_{}, activity_button_center_{};
  ImVec2 display_menu_center_{};
  ImGuiID file_menu_popup_ = 0, more_menu_popup_ = 0, activity_popup_id_ = 0;
  ImGuiID display_menu_popup_ = 0, view_settings_popup_ = 0;
  ImGuiWindow *inspector_test_window_ = nullptr;
  ImVec2 layout_smoke_viewport_{};
  OrbitCamera layout_smoke_camera_;
  std::optional<PickingKey> layout_smoke_selection_;
  bool layout_smoke_drawer_visible_ = false;
  RgbaImage unselected_;
  bool smoke_navigation_ = false, smoke_picking_ = false;
  std::size_t selection_pixels_ = 0;
  int native_smoke_phase_ = 0;
  std::size_t motion_pixels_ = 0;
#ifdef PVD_VIEWER_HAS_TRANSPORT
  LiveSession live_{LivePollLimits{2048, 2u * 1024u * 1024u}};
  std::optional<SlotId> live_slot_id_;
  std::string live_recording_path_;
  bool live_bound_ = false;
  std::array<char, 64> capture_address_{'0', '.', '0', '.', '0', '.', '0'};
  std::array<char, 4096> capture_path_{};
  int capture_port_ = 5425;
  bool record_capture_ = true, live_document_seen_ = false,
       capture_setup_requested_ = false;
  bool live_snapshot_started_ = false, live_background_started_ = false,
       live_background_verified_ = false;
  SlotId live_background_slot_ = 0;
  std::size_t live_background_prefix_ = 0;
  std::chrono::steady_clock::time_point next_live_poll_{};
  std::size_t online_render_frames_ = 0, live_display_changes_ = 0;
  std::optional<std::size_t> last_live_event_count_;
#endif

  bool smoke_mode() const {
    return options_.smoke || options_.analysis_smoke || options_.live_smoke ||
           options_.export_smoke || options_.repx_smoke ||
           options_.workspace_smoke || options_.clip_smoke || options_.deformable_smoke || options_.query_smoke || options_.dpi_smoke;
  }
  WorkspaceSlot &active_slot() {
    if (!workspace_.active())
      workspace_.create_empty();
    return *workspace_.active();
  }
  Document &document() { return active_slot().document(); }
  OrbitCamera &camera() { return active_slot().camera(); }
  bool fitted() { return active_slot().camera_ready(); }
  void set_fitted(bool value) { active_slot().set_camera_ready(value); }
  void apply_preferences(SlotId id, SlotUiState &state) {
    if (const auto *slot = workspace_.find(id)) {
      const auto &preferences = slot->preferences();
      state.render_options = preferences.rendering;
      state.panels.inspector_preferences = preferences.inspection;
      state.show_debug_text = preferences.show_debug_text;
      state.navigation = preferences.navigation;
    }
  }
  void store_preferences(SlotId id) {
    auto *slot = workspace_.find(id);
    auto cached = slot_ui_.find(id);
    if (!slot || cached == slot_ui_.end() || !cached->second) return;
    ViewPreferences preferences;
    preferences.rendering = cached->second->render_options;
    preferences.inspection = cached->second->panels.inspector_preferences;
    preferences.show_debug_text = cached->second->show_debug_text;
    preferences.navigation = cached->second->navigation;
    if (!valid_view_preferences(preferences)) return;
    slot->preferences() = preferences;
    if (workspace_.active_id() == id) {
      workspace_.set_default_preferences(preferences);
      workspace_.set_default_view_options(slot->document().view_options());
    }
  }
  void restore_default_preferences() {
    auto &state=ui();
    document().set_view_options(ViewOptions{});
    state.render_options={};state.panels.inspector_preferences={};
    state.navigation={};state.show_debug_text=true;
    const OrbitCamera defaults;
    set_camera_projection(camera(),defaults.near_plane,defaults.far_plane,defaults.vertical_fov);
    store_preferences(active_slot().id());
    notice_="Built-in display, navigation and property preferences restored.";
  }
  SlotUiState &ui_for(SlotId id) {
    auto &value = slot_ui_[id];
    if (!value) {
      value = std::make_unique<SlotUiState>();
      apply_preferences(id, *value);
      if (options_.analysis_smoke)
        value->page = options_.analysis_page;
    }
    return *value;
  }
  SlotUiState &ui() { return ui_for(active_slot().id()); }
  VisualizationSelection visualization_selection() {
    return {document().inspected_object(),document().selected_key()};
  }
  void sync_clips(SlotId id) {
    auto *slot=workspace_.find(id);
    if(!slot)return;
    auto &state=ui_for(id);
    if(!state.clips_resync&&state.clips_revision==slot->clip_revision()&&
        state.clips.observed_source==slot->document().session())return;
    const auto selected=state.clips.active_catalog;
    state.clips={};state.clips.catalogs=slot->clip_catalogs();
    state.clips.source_slot=id;state.clips.observed_source=slot->document().session();
    if(selected&&state.clips.catalogs.count(*selected))state.clips.active_catalog=selected;
    else if(!state.clips.catalogs.empty())state.clips.active_catalog=state.clips.catalogs.begin()->first;
    state.clips_revision=slot->clip_revision();state.clips_resync=false;
  }
  void persist_clips(ClipPanelChange change) {
    auto *slot=workspace_.find(change.source_slot);
    if(!slot)throw std::runtime_error("The original clip tab was closed.");
    auto &state=ui_for(change.source_slot);
    try {
      if(change.kind==ClipPanelChangeKind::discarded)
        workspace_.discard_pending_clip_states(change.source_slot);
      std::vector<pvd::clips::State> values;
      for(const auto &catalog:state.clips.catalogs)values.push_back(catalog.second.state());
      std::vector<std::string> warnings;
      if(!workspace_.set_clip_states(change.source_slot,std::move(values),&warnings))
        throw std::runtime_error("This clip source is locked or no longer available.");
      state.clips_revision=slot->clip_revision();
      if(!warnings.empty())notice_=warnings.front();
    }catch(...){state.clips_resync=true;throw;}
  }
  void save_clip(ClipSaveRequest command) {
    auto *slot=workspace_.find(command.source_slot);
    if(!slot||!slot->document().session())throw std::runtime_error("The original clip source is unavailable.");
    const auto *source=slot->document().session();
    pvd::clips::validate_clip(*source,command.clip);
    auto filename=command.clip.name;
    for(auto &c:filename)if(static_cast<unsigned char>(c)<32||
        std::string("<>:\"/\\|?*").find(c)!=std::string::npos)c='_';
    auto destination=options_.clip_smoke
        ? std::optional<ExportDestination>{{utf8(options_.output.wstring()),false}}
        : choose_export_path(("clip-"+filename+".pxd2").c_str());
    if(!destination)return;
    slot=workspace_.find(command.source_slot);
    if(!slot||slot->document().session()!=source)
      throw std::runtime_error("The clip source changed while choosing its output.");
    pvd::clips::validate_clip(*source,command.clip);
    ExportRequest request;
    request.destination=std::move(destination->path);request.overwrite=destination->overwrite;
    request.mode=ExportMode::native_frame_range;request.scene_id=command.clip.binding.scene.id;
    request.first_native_frame=command.clip.frames.first;request.last_native_frame=command.clip.frames.last;
    start_export(command.source_slot,std::move(request));
    export_popup_slot_=command.source_slot;
  }
  enum class ClipEdit { split, trim_left, trim_right };
  void edit_clip_at_playhead(ClipEdit command) {
    const auto id=active_slot().id();
    sync_clips(id);
    auto &state=ui_for(id);
    if(active_slot().lock_reason()&&active_slot().lock_reason()!=SlotLock::live)
      throw std::runtime_error("Finish the source operation before editing clips.");
    const auto frame=document().frame();
    const auto scene=document().scene_id();
    const auto *object=frame&&scene?frame->snapshot.instance(*scene):nullptr;
    if(!object||document().sample_point()!=SamplePoint::native_frame)
      throw std::runtime_error("Choose a scene's Frame end timeline before editing clips.");
    const pvd::clips::SceneKey key{object->id,object->generation};
    if(state.clips.active_catalog&&*state.clips.active_catalog!=key)
      throw std::runtime_error("Choose the current scene lifetime in Clips before editing its playhead.");
    const auto at=state.clips.catalogs.find(key);
    auto candidate=at==state.clips.catalogs.end()
        ? pvd::clips::Catalog::create(*document().session(),key):at->second;
    if(command==ClipEdit::split)candidate.split(*document().session(),document().frame_index());
    else if(command==ClipEdit::trim_left)candidate.trim_left(*document().session(),document().frame_index());
    else candidate.trim_right(*document().session(),document().frame_index());
    state.clips.catalogs.insert_or_assign(key,std::move(candidate));
    state.clips.active_catalog=key;
    state.clips.edited_clip.reset();
    persist_clips({id,ClipPanelChangeKind::edited});
  }
  void draw_clips() {
    const auto id=active_slot().id();sync_clips(id);
    auto &state=ui_for(id);auto *slot=workspace_.find(id);
    if(!slot)return;
    if(!slot->pending_clip_states().empty()) {
      ImGui::TextWrapped("%zu archived clip catalog(s) are unverified for this source; "
                         "they remain saved until explicitly discarded.",slot->pending_clip_states().size());
      if(ImGui::Button("Verify archived clips"))try {
        std::vector<std::string> warnings;
        if(!workspace_.verify_clip_states(id,&warnings))
          throw std::runtime_error("Finish this source operation before verification.");
        if(!warnings.empty())notice_=warnings.front();sync_clips(id);
      }catch(const std::exception &e){state.clips.error=e.what();}
      ImGui::SameLine();
      if(ImGui::Button("Discard unverified clip archives")) {
        workspace_.discard_pending_clip_states(id);sync_clips(id);
      }
    }
    ClipPanelActions actions;
    actions.busy=slot->lock_reason()&&slot->lock_reason()!=SlotLock::live;
    actions.save_as=[&](ClipSaveRequest request){save_clip(std::move(request));};
    actions.changed=[&](ClipPanelChange change){persist_clips(change);};
    drawClipRanges(slot->document(),id,state.clips,actions);
  }
  void update_title() {
    const auto &slot = active_slot();
    const auto label = std::filesystem::u8path(slot.title()).wstring();
    const auto title = L"PVD Studio  |  " + label;
    SetWindowTextW(window_, title.c_str());
  }
  void activate_slot(SlotId id) {
    if (!workspace_.activate(id)) {
      error_ = "This document tab no longer exists.";
      return;
    }
    ui_for(id).camera_input={};
    update_title();
  }
  void close_slot(SlotId id) {
    const auto *slot = workspace_.find(id);
    if (!slot)
      return;
    if (slot->lock_reason()) {
      error_ = std::string("Cannot close this tab while ") +
               slot_lock_name(*slot->lock_reason()) +
               ". Stop or cancel its operation first.";
      return;
    }
    close_requests_.push_back(id);
  }
  void apply_closed_slots() {
    for (auto id : close_requests_) {
      if (workspace_.close(id))
        slot_ui_.erase(id);
    }
    close_requests_.clear();
    active_slot();
    update_title();
  }
  WorkspaceActions workspace_actions() {
    WorkspaceActions actions;
    actions.open = [&](SlotId id, const std::string &path) {
      if (path.empty())
        choose_file(id);
      else
        open(std::filesystem::u8path(path), id);
    };
    actions.activate = [&](SlotId id) { activate_slot(id); };
    actions.close = [&](SlotId id) { close_slot(id); };
    actions.jump_bookmark = [&](SlotId id, BookmarkId book) {
      auto *slot = workspace_.find(id);
      if (!slot)
        throw std::runtime_error("The bookmark tab was closed.");
      bool has_camera = false;
      for (const auto &b : slot->bookmarks())
        if (b.id == book)
          has_camera = b.camera.has_value();
      if (!workspace_.jump_to_bookmark(id, book))
        throw std::runtime_error(
            "Bookmark source, scene or frame is unavailable.");
      if (has_camera)
        stopTracking(ui_for(id).scene_tools);
      activate_slot(id);
    };
    return actions;
  }
  SlotId new_target() {
    auto &slot = active_slot();
    if (!slot.document().loaded() && slot.source_path().empty() &&
        !slot.lock_reason())
      return slot.id();
    return workspace_.create_empty();
  }
  void cancel_loads() {
    load_job_.cancel();
    for (const auto &task : load_queue_)
      workspace_.unlock(task.id, SlotLock::loading);
    load_queue_.clear();
  }
  void cancel_associations() {
    if (bind_cancel_)
      bind_cancel_->store(true);
    for (const auto &task : bind_queue_)
      workspace_.unlock(task.id, SlotLock::loading);
    bind_queue_.clear();
  }
  void launch_binding(SlotId id, const std::string &path) {
    auto *slot = workspace_.find(id);
    if (!slot) {
      workspace_.unlock(id, SlotLock::loading);
      return;
    }
    try {
      bind_slot_ = id;
      bind_path_ = path;
      bind_cancel_ = std::make_shared<std::atomic<bool>>(false);
      bind_progress_ = std::make_shared<std::atomic<std::size_t>>(0);
      const auto *session = slot->document().session();
      const auto cancel = bind_cancel_;
      const auto progress = bind_progress_;
      bind_identity_ =
          std::async(std::launch::async, [session, cancel, progress] {
            return workspace_source_identity(*session, [&](std::size_t done,
                                                           std::size_t) {
              if (cancel->load())
                throw std::runtime_error("Recording association cancelled.");
              progress->store(done);
            });
          });
    } catch (...) {
      workspace_.unlock(id, SlotLock::loading);
      bind_slot_.reset();
      bind_path_.clear();
      throw;
    }
  }
  bool begin_binding(SlotId id, const std::string &path) {
    auto *slot = workspace_.find(id);
    if (!slot || path.empty() || slot->document().live() ||
        !slot->document().loaded() || slot->lock_reason())
      return false;
    if (!workspace_.lock(id, SlotLock::loading))
      return false;
    if (bind_identity_.valid()) {
      try {
        bind_queue_.push_back({id, path});
      } catch (...) {
        workspace_.unlock(id, SlotLock::loading);
        throw;
      }
    } else
      launch_binding(id, path);
    return true;
  }
  void poll_binding() {
    if (bind_identity_.valid()) {
      if (bind_identity_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
        return;
      const auto id = bind_slot_;
      if (id)
        workspace_.unlock(*id, SlotLock::loading);
      try {
        auto identity = bind_identity_.get();
        if (id) {
          std::vector<std::string> warnings;
          if (!workspace_.bind_source(*id, bind_path_, identity, &warnings))
            throw std::runtime_error("The recorded source could not be "
                                     "associated with its original tab.");
          if (!warnings.empty())
            notice_ = warnings.front();
        }
      } catch (const std::exception &e) {
        error_ = e.what();
      }
      bind_slot_.reset();
      bind_path_.clear();
      bind_cancel_.reset();
      bind_progress_.reset();
    }
    if (!bind_queue_.empty()) {
      auto task = std::move(bind_queue_.front());
      bind_queue_.pop_front();
      try {
        launch_binding(task.id, task.path);
      } catch (const std::exception &e) {
        error_ = e.what();
      }
    }
  }
  void poll_export() {
    if (!export_slot_id_ || export_job_.running())
      return;
    const auto id = *export_slot_id_;
    workspace_.unlock(id, SlotLock::exporting);
#ifdef PVD_VIEWER_HAS_TRANSPORT
    if (export_resume_live_ && live_slot_id_ == id && live_.active())
      workspace_.lock(id, SlotLock::live);
#endif
    export_resume_live_ = false;
    export_slot_id_.reset();
    const auto status = export_job_.status();
    if (status.stage == ExportStage::failed)
      error_ = status.error;
    if (status.stage == ExportStage::completed &&
        export_mode_ == ExportMode::whole) {
      if (auto *slot = workspace_.find(id))
        if (slot->source_path().empty() && !slot->document().live())
          begin_binding(id, export_destination_);
    }
  }
  void start_export(SlotId id, ExportRequest request) {
    auto *slot = workspace_.find(id);
    if (!slot || !slot->document().session())
      throw std::runtime_error("Choose a loaded capture to export.");
    if (export_job_.running())
      throw std::runtime_error("Another export is still running.");
#ifdef PVD_VIEWER_HAS_REPX
    if (repx_job_.running())
      throw std::runtime_error("A RepX export is still running.");
#endif
    bool resume = false;
    if (slot->lock_reason()) {
#ifdef PVD_VIEWER_HAS_TRANSPORT
      if (slot->lock_reason() == SlotLock::live && live_slot_id_ == id) {
        workspace_.unlock(id, SlotLock::live);
        resume = live_.active();
      } else
#endif
        throw std::runtime_error("This tab is locked by another operation.");
    }
    if (!workspace_.lock(id, SlotLock::exporting))
      throw std::runtime_error("Cannot borrow this tab for export.");
    try {
      export_mode_ = request.mode;
      export_destination_ = request.destination;
      export_job_.start(*slot->document().session(), std::move(request));
      export_slot_id_ = id;
      last_export_slot_id_ = id;
      export_resume_live_ = resume;
      export_status_visible_ = true;
    } catch (...) {
      workspace_.unlock(id, SlotLock::exporting);
#ifdef PVD_VIEWER_HAS_TRANSPORT
      if (resume)
        workspace_.lock(id, SlotLock::live);
#endif
      throw;
    }
  }
  bool same_path(const std::filesystem::path &a,
                 const std::filesystem::path &b) const {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec)
      return true;
    const auto aa = std::filesystem::absolute(a).lexically_normal().wstring(),
               bb = std::filesystem::absolute(b).lexically_normal().wstring();
    return _wcsicmp(aa.c_str(), bb.c_str()) == 0;
  }
  void protect_recording_path(const std::filesystem::path &path) {
#ifdef PVD_VIEWER_HAS_TRANSPORT
    if (live_.active() && !live_recording_path_.empty() &&
        same_path(path, std::filesystem::u8path(live_recording_path_)))
      throw std::runtime_error(
          "Choose a different path from the active raw recording.");
#else
    (void)path;
#endif
  }
  void configure_persistence() {
    automatic_settings_ =
        !smoke_mode() && !options_.hidden && !options_.no_settings;
    if (!automatic_settings_)
      return;
    try {
      PWSTR base = nullptr;
      check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                 nullptr, &base),
            "Locate application settings");
      settings_directory_ = std::filesystem::path(base) / L"PvdStudio";
      CoTaskMemFree(base);
      std::filesystem::create_directories(settings_directory_);
      settings_file_ = settings_directory_ / L"workspace.pvdws";
      ini_path_ = utf8((settings_directory_ / L"layout.ini").wstring());
      ImGui::GetIO().IniFilename = ini_path_.c_str();
      settings_writable_ = true;
      if (std::filesystem::exists(settings_file_)) {
        auto saved = load_workspace_file(utf8(settings_file_.wstring()));
        if (!options_.capture.empty() || options_.listen_port >= 0) {
          saved.slots.clear();
          saved.active_slot.reset();
        }
        auto pending = workspace_.restore(saved);
        slot_ui_.clear();
        active_slot();
        for (auto id : pending.source_slots)
          if (const auto *slot = workspace_.find(id))
            schedule_load(id, std::filesystem::u8path(slot->source_path()));
      }
    } catch (const std::exception &e) {
      settings_writable_ = false;
      error_ = std::string("Settings were not loaded; automatic workspace "
                           "replacement is disabled: ") +
               e.what();
    }
  }
  void save_automatic_settings() noexcept {
    if (!automatic_settings_ || !settings_writable_ || settings_file_.empty())
      return;
    try {
      workspace_.save_file(utf8(settings_file_.wstring()), true);
      if (!ini_path_.empty())
        ImGui::SaveIniSettingsToDisk(ini_path_.c_str());
    } catch (const std::exception &e) {
      OutputDebugStringA(e.what());
    }
  }
  std::optional<std::filesystem::path> workspace_dialog(bool save) {
    std::wstring buffer(32768, L'\0');
    if (save) {
      const std::wstring name = L"workspace.pvdws";
      std::copy(name.begin(), name.end(), buffer.begin());
    }
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter = L"PVD workspace (*.pvdws)\0*.pvdws\0All files\0*.*\0";
    dialog.lpstrDefExt = L"pvdws";
    dialog.lpstrTitle = save ? L"Save workspace" : L"Load workspace";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)))
      return {};
    buffer.resize(wcslen(buffer.c_str()));
    return std::filesystem::path(buffer);
  }
  void save_workspace() {
    try {
      if (auto path = workspace_dialog(true)) {
        protect_recording_path(*path);
        for (const auto *slot : workspace_.slots())
          if (!slot->source_path().empty() &&
              same_path(*path, std::filesystem::u8path(slot->source_path())))
            throw std::runtime_error(
                "Choose a workspace filename different from a capture file.");
        workspace_.save_file(utf8(path->wstring()),
                             std::filesystem::exists(*path));
        notice_ = "Workspace saved. Unsaved TCP tabs are included only after "
                  "recording association.";
      }
    } catch (const std::exception &e) {
      error_ = e.what();
    }
  }
  void restore_workspace(const std::filesystem::path &path) {
    if (workspace_.any_locked())
      throw std::runtime_error("Stop or finish loading, live reception and "
                               "export before replacing the workspace.");
    auto pending = workspace_.load_file(utf8(path.wstring()));
    slot_ui_.clear();
    active_slot();
    workspace_panel_ = {};
    for (auto id : pending.source_slots)
      if (const auto *slot = workspace_.find(id))
        schedule_load(id, std::filesystem::u8path(slot->source_path()));
    update_title();
  }
  void load_workspace() {
    try {
      if (auto path = workspace_dialog(false))
        restore_workspace(*path);
    } catch (const std::exception &e) {
      error_ = e.what();
    }
  }
  void reset_layout() {
    ImGui::ClearIniSettings();
    for (auto &entry : slot_ui_)
      entry.second->sidebar = SlotUiState::Sidebar::none;
    ++layout_generation_;
    if (automatic_settings_ && !ini_path_.empty())
      ImGui::SaveIniSettingsToDisk(ini_path_.c_str());
    notice_ = "Layout reset. Document views and bookmarks were kept.";
  }
  void help_popup() {
    if (help_requested_) {
      ImGui::OpenPopup("Help and shortcuts");
      help_requested_ = false;
    }
    ImGui::SetNextWindowSize({ui_pixels(560), 0}, ImGuiCond_Appearing);
    if (ui::begin_popup_modal("Help and shortcuts", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("PVD Studio " PVD_VIEWER_VERSION " / PhysX 3.4 raw captures");
      ImGui::TextWrapped(
          "Ctrl+O opens a capture in a tab. Ctrl+S saves or crops the current "
          "tab. Ctrl+B bookmarks the current frame and camera.");
      ImGui::TextWrapped(
          "Space plays or pauses; Left/Right step; Home/End choose the "
          "first/last frame; F fits visible geometry. Shortcuts pause while "
          "editing or using a popup.");
      ImGui::TextWrapped("Clips: Ctrl+Alt+Y splits at the native playhead; "
                         "Ctrl+Alt+S trims left; Ctrl+Alt+E trims right.");
      ImGui::TextWrapped("Ctrl+Shift+S opens view preferences; Ctrl+Shift+R restores built-in preferences.");
      ImGui::TextWrapped("Scene-first layout: Objects and Properties open one overlay drawer at a time. Escape closes the drawer. Analysis and scene tools are grouped in the top menu.");
      ImGui::TextWrapped(
          "RMB looks in place, Alt+LMB orbits, MMB pans, wheel zooms. "
          "Hover the viewport for WASD/QE; hold to accelerate, Shift doubles speed. "
          "View offers speed, look-at and clipping. Click picks a visible shape. View "
          "tools contains visibility, colors, tracking and captured cameras.");
      ImGui::TextWrapped(
          "Workspace files store source paths, views and verified bookmarks, "
          "never raw capture data. Live reception remains bound to its own "
          "tab. Save a live snapshot temporarily pauses only model import "
          "while network recording continues.");
      if (automatic_settings_)
        ImGui::TextWrapped("Settings: %s",
                           utf8(settings_directory_.wstring()).c_str());
      else
        ImGui::TextDisabled(
            "Automatic user settings are disabled for this run.");
      if (ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
      ui::end_popup();
    }
  }
  void load_bar() {
    if (!load_status_visible_ && !bind_slot_)
      return;
    if (bind_slot_) {
      ImGui::TextDisabled("Associating recording: %zu events fingerprinted",
                          bind_progress_ ? bind_progress_->load() : 0);
      ImGui::SameLine();
      if (ImGui::SmallButton("Cancel association"))
        cancel_associations();
      if (!bind_queue_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu queued", bind_queue_.size());
      }
    }
    if (load_status_visible_) {
      const auto status = load_job_.status();
      ImGui::TextUnformatted(load_stage_name(status.stage));
      ImGui::SameLine();
      if (status.file_size)
        ImGui::TextDisabled("%.2f / %.2f MiB  |  %zu events",
                            double(status.bytes_read) / (1024 * 1024),
                            double(*status.file_size) / (1024 * 1024),
                            status.event_count);
      else
        ImGui::TextDisabled("%zu events", status.event_count);
      ImGui::SameLine();
      if (load_job_.running()) {
        if (!status.cancel_requested) {
          if (ImGui::SmallButton("Cancel loading"))
            cancel_loads();
        } else
          ImGui::TextDisabled("Cancelling...");
      } else if (ImGui::SmallButton("Dismiss load"))
        load_status_visible_ = false;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", status.source_path.c_str(),
                          status.message.c_str());
      if (!load_queue_.empty())
        ImGui::TextDisabled("%zu more sources queued", load_queue_.size());
    }
    ImGui::Separator();
  }
  static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) {
    App *self = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<App *>(
          reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (ImGui::GetCurrentContext() &&
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
      return 1;
    if (self) {
      if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
        self->resize_width_ = LOWORD(lp);
        self->resize_height_ = HIWORD(lp);
        return 0;
      }
      if (msg == WM_DPICHANGED) {
        if (!self->options_.ui_scale && (!self->smoke_mode() || self->options_.dpi_smoke))
          self->pending_ui_scale_ = ui_scale_from_dpi(LOWORD(wp));
        const auto *suggested = reinterpret_cast<const RECT *>(lp);
        if (suggested)
          SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                       suggested->right-suggested->left, suggested->bottom-suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      if (msg == WM_DROPFILES) {
        const auto drop = reinterpret_cast<HDROP>(wp);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; ++index) {
          const UINT length = DragQueryFileW(drop, index, nullptr, 0);
          std::wstring path(length + 1, L'\0');
          DragQueryFileW(drop, index, path.data(), length + 1);
          path.resize(length);
          self->open(path);
        }
        DragFinish(drop);
        return 0;
      }
      if (msg == WM_GETMINMAXINFO) {
        auto *limits = reinterpret_cast<MINMAXINFO *>(lp);
        const auto minimum = self->window_plan(hwnd, self->effective_ui_scale(hwnd)).minimum_outer_pixels;
        limits->ptMinTrackSize = {static_cast<LONG>(std::ceil(minimum.x)),
                                 static_cast<LONG>(std::ceil(minimum.y))};
        return 0;
      }
      if (msg == WM_CLOSE) {
        self->quit_ = true;
        return 0;
      }
      if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
      }
      if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_KEYMENU)
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  void create_backbuffer() {
    ComPtr<ID3D11Texture2D> buffer;
    check(swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Get backbuffer");
    check(device_->CreateRenderTargetView(buffer.Get(), nullptr, &backbuffer_),
          "Create backbuffer view");
  }
  float effective_ui_scale(HWND hwnd) const {
    if (options_.ui_scale) return *options_.ui_scale;
    if (pending_ui_scale_) return *pending_ui_scale_;
    if (options_.dpi_smoke) return ui_scale_ ? ui_scale_->current().screen_scale : 1.f;
    return smoke_mode() ? 1.f : ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
  }
  static ImVec2 native_chrome(float screen_scale) {
    RECT rectangle{0,0,100,100};
    using AdjustForDpi = BOOL(WINAPI *)(LPRECT,DWORD,BOOL,DWORD,UINT);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto adjust = reinterpret_cast<AdjustForDpi>(GetProcAddress(user32,"AdjustWindowRectExForDpi"));
    if (adjust)
      adjust(&rectangle,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_ACCEPTFILES,
             static_cast<UINT>(std::lround(screen_scale*96)));
    else AdjustWindowRectEx(&rectangle,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_ACCEPTFILES);
    return {float(rectangle.right-rectangle.left-100),float(rectangle.bottom-rectangle.top-100)};
  }
  UiWindowPlan window_plan(HWND hwnd, float scale) const {
    std::optional<ImVec2> area;
    if (!options_.hidden) {
      MONITORINFO monitor{sizeof(MONITORINFO)};
      if (GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
        area=ImVec2(float(monitor.rcWork.right-monitor.rcWork.left),
                    float(monitor.rcWork.bottom-monitor.rcWork.top));
    }
    UiWindowPolicy policy;
    policy.initial_client_dp={float(options_.window_width),float(options_.window_height)};
    // Native chrome follows the actual OS DPI even when the user overrides UI
    // font/layout scale. It must not be multiplied by the UI scale a second time.
    return plan_ui_window(scale,area,native_chrome(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd)),policy);
  }
  void update_ui_scale() {
    if (!ui_scale_ || !pending_ui_scale_) return;
    const auto scale=*pending_ui_scale_;pending_ui_scale_.reset();
    if (!ui_scale_->needs_reload(scale)) return;
    ImGui_ImplDX11_InvalidateDeviceObjects();
    try {
      ui_scale_->reload_fonts(scale);
      if (!ImGui_ImplDX11_CreateDeviceObjects())
        throw std::runtime_error("Cannot upload scaled UI fonts");
      ++layout_generation_;
    } catch (const std::exception &e) {
      // The font helper stages before publication. Recreate backend objects for
      // the retained atlas as well, so a rejected scale keeps the UI usable.
      ImGui_ImplDX11_CreateDeviceObjects();
      error_=std::string("UI scale: ")+e.what();
    }
  }
  void initialize() {
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PvdStudioWindow";
    if (!RegisterClassExW(&wc))
      throw std::runtime_error("Cannot register viewer window");
    POINT cursor{};GetCursorPos(&cursor);
    const auto monitor_handle=MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY);
    const auto initial_scale=options_.ui_scale.value_or(smoke_mode()?1.f:
        ImGui_ImplWin32_GetDpiScaleForMonitor(monitor_handle));
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const bool have_monitor=GetMonitorInfoW(monitor_handle,&monitor)!=FALSE;
    UiWindowPolicy policy;
    policy.initial_client_dp={float(options_.window_width),float(options_.window_height)};
    const auto area=(!options_.hidden&&have_monitor)?
        std::optional<ImVec2>{{float(monitor.rcWork.right-monitor.rcWork.left),
                              float(monitor.rcWork.bottom-monitor.rcWork.top)}}:std::nullopt;
    const auto plan=plan_ui_window(initial_scale,area,
        native_chrome(ImGui_ImplWin32_GetDpiScaleForMonitor(monitor_handle)),policy);
    window_ = CreateWindowExW(
        WS_EX_ACCEPTFILES, wc.lpszClassName, L"PVD Studio", WS_OVERLAPPEDWINDOW,
        have_monitor?monitor.rcWork.left+16:CW_USEDEFAULT,
        have_monitor?monitor.rcWork.top+16:CW_USEDEFAULT,
        static_cast<int>(std::ceil(plan.initial_outer_pixels.x)),
        static_cast<int>(std::ceil(plan.initial_outer_pixels.y)),nullptr,nullptr,wc.hInstance,this);
    if (!window_)
      throw std::runtime_error("Cannot create viewer window");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    auto create = [&](D3D_DRIVER_TYPE driver) {
      return D3D11CreateDeviceAndSwapChain(
          nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
          D3D11_SDK_VERSION, &sd, &swapchain_, &device_, nullptr, &context_);
    };
    HRESULT hr =
        create(options_.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
    using_warp_ = options_.warp;
    if (FAILED(hr) && !options_.warp) {
      using_warp_ = true;
      hr = create(D3D_DRIVER_TYPE_WARP);
    }
    check(hr, "Create Direct3D 11 device");
    create_backbuffer();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_ = true;
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename =
        nullptr; // Viewer layout is deterministic; no cwd side effects.
    theme();
    configure_persistence();
    UiScaleOptions font_options;
    font_options.base_font_pixels = ui::font_size_dp;
    ui_scale_=std::make_unique<UiScale>(ImGui::GetStyle(),[](ImFontAtlas &atlas,float pixels) {
      ImFontConfig font;font.SizePixels=pixels;
      const std::filesystem::path segoe=L"C:/Windows/Fonts/segoeui.ttf";
      auto *primary=std::filesystem::exists(segoe)?
          atlas.AddFontFromFileTTF(segoe.string().c_str(),pixels,&font):nullptr;
      const std::filesystem::path cjk=L"C:/Windows/Fonts/msyh.ttc";
      if(primary&&std::filesystem::exists(cjk)) {
        // The bundled common-character subset omits the debugging term 帧.
        // Keep this bounded fallback range alive across all atlas rebuilds.
        static ImVector<ImWchar> ranges;
        if(ranges.empty()) {
          ImFontGlyphRangesBuilder builder;
          builder.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
          builder.AddText(u8"帧文件工作区帮助场景分析更多透视聚焦显示选择已模拟完成后结束对象属性诊断数值格式精度右键观察中平移滚轮缩放就绪");
          builder.BuildRanges(&ranges);
        }
        ImFontConfig merge;merge.MergeMode=true;merge.PixelSnapH=true;
        atlas.AddFontFromFileTTF(cjk.string().c_str(),pixels,&merge,
            ranges.Data);
      }
      return primary;
    }, font_options);
    ui_scale_->reload_fonts(effective_ui_scale(window_));
    pending_ui_scale_.reset();
    if (!ImGui_ImplWin32_Init(window_))
      throw std::runtime_error("Initialize Win32 backend");
    win32_backend_ = true;
    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
      throw std::runtime_error("Initialize Direct3D UI backend");
    }
    dx11_backend_ = true;
    renderer_ = std::make_unique<Renderer>(device_.Get(), context_.Get());
    if (!options_.hidden) {
      ShowWindow(window_, SW_SHOWDEFAULT);
      UpdateWindow(window_);
    }
    resize_width_ = resize_height_ = 0;
  }
  void theme() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {16, 12};
    s.FramePadding = {9, 6};
    s.ItemSpacing = {9, 8};
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 4;
    s.PopupRounding = 7;
    s.PopupBorderSize = 1;
    s.GrabRounding = 4;
    s.ScrollbarRounding = 5;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 0;
    auto *c = s.Colors;
    c[ImGuiCol_WindowBg] = {0.063f, 0.082f, 0.106f, 1};
    c[ImGuiCol_ChildBg] = {0.090f, 0.118f, 0.149f, 1};
    c[ImGuiCol_MenuBarBg] = {0.110f, 0.141f, 0.176f, 1};
    c[ImGuiCol_Text] = {0.871f, 0.902f, 0.929f, 1};
    c[ImGuiCol_TextDisabled] = {0.584f, 0.651f, 0.714f, 1};
    c[ImGuiCol_Border] = {0.173f, 0.216f, 0.263f, 1};
    c[ImGuiCol_FrameBg] = {0.125f, 0.169f, 0.212f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.14f, 0.22f, 0.26f, 1};
    c[ImGuiCol_FrameBgActive] = {0.12f, 0.28f, 0.29f, 1};
    c[ImGuiCol_Button] = {0.125f, 0.169f, 0.212f, 1};
    c[ImGuiCol_ButtonHovered] = {0.18f, 0.26f, 0.29f, 1};
    c[ImGuiCol_ButtonActive] = {0.12f, 0.31f, 0.30f, 1};
    c[ImGuiCol_Header] = {0.137f, 0.243f, 0.243f, 1};
    c[ImGuiCol_HeaderHovered] = {0.12f, 0.33f, 0.36f, 1};
    c[ImGuiCol_HeaderActive] = {0.12f, 0.39f, 0.40f, 1};
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = {0.341f, 0.812f, 0.729f, 1};
    c[ImGuiCol_SliderGrabActive] = {0.40f, 0.94f, 0.82f, 1};
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_TableHeaderBg] = {0.08f, 0.12f, 0.16f, 1};
    c[ImGuiCol_TableRowBgAlt] = {0.12f, 0.16f, 0.21f, 0.35f};
    c[ImGuiCol_Tab] = {0.090f, 0.118f, 0.149f, 1};
    c[ImGuiCol_TabHovered] = {0.12f, 0.33f, 0.35f, 1};
    c[ImGuiCol_TabSelected] = {0.090f, 0.118f, 0.149f, 1};
    c[ImGuiCol_TabSelectedOverline] = {0.341f, 0.812f, 0.729f, 1};
    c[ImGuiCol_TitleBg] = {0.06f, 0.10f, 0.14f, 1};
    c[ImGuiCol_TitleBgActive] = {0.08f, 0.18f, 0.20f, 1};
    c[ImGuiCol_PopupBg] = {0.110f, 0.141f, 0.176f, 1};
    c[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, .62f};
  }
  void choose_file(std::optional<SlotId> target = {}) {
    std::wstring buffer(32768, L'\0');
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter =
        L"PhysX raw captures (*.pxd2;*.pvd)\0*.pxd2;*.pvd\0All files\0*.*\0";
    dialog.lpstrTitle = L"Open PhysX capture in workspace";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
      buffer.resize(wcslen(buffer.c_str()));
      open(buffer, target);
    }
  }
  void schedule_load(SlotId id, const std::filesystem::path &source) {
    auto *slot = workspace_.find(id);
    if (!slot)
      throw std::runtime_error("The target tab was closed.");
    if (slot->lock_reason())
      throw std::runtime_error(
          "This tab is already loading, receiving or exporting.");
    const auto path = std::filesystem::absolute(source).lexically_normal();
    if (!workspace_.lock(id, SlotLock::loading))
      throw std::runtime_error("Cannot reserve the target tab for loading.");
    try {
      load_queue_.push_back({id, path});
    } catch (...) {
      workspace_.unlock(id, SlotLock::loading);
      throw;
    }
    load_status_visible_ = true;
    error_.clear();
  }
  void open(const std::filesystem::path &path,
            std::optional<SlotId> target = {}) {
    try {
      if (_wcsicmp(path.extension().c_str(),L".pvdws")==0) {
        restore_workspace(path);
        return;
      }
      const auto id = (target ? *target : new_target());
      schedule_load(id, path);
      activate_slot(id);
    } catch (const std::exception &e) {
      error_ = e.what();
    }
  }
  void poll_load() {
    if (loading_slot_ && !load_job_.running()) {
      const auto id = *loading_slot_;
      const auto status = load_job_.status();
      workspace_.unlock(id, SlotLock::loading);
      try {
        if (auto result = load_job_.take_ready()) {
          std::vector<std::string> warnings;
          if (!workspace_.replace(result->target_slot, result->document,
                                  result->source_path, &warnings,
                                  result->identity))
            throw std::runtime_error(
                "The original load target is no longer available.");
          loaded(result->target_slot);
          if (!warnings.empty())
            notice_ = warnings.front();
        } else if (status.stage == LoadStage::failed)
          error_ = status.error;
        else if (status.stage == LoadStage::cancelled)
          notice_ =
              "Capture loading cancelled; the previous document was kept.";
      } catch (const std::exception &e) {
        error_ = e.what();
      }
      loading_slot_.reset();
      loading_.clear();
    }
    if (!loading_slot_ && !load_queue_.empty()) {
      const auto task = load_queue_.front();
      load_queue_.pop_front();
      if (!workspace_.find(task.id))
        return;
      LoadRequest request;
      request.target_slot = task.id;
      request.source_path = utf8(task.path.wstring());
      try {
        load_job_.start(std::move(request));
        loading_slot_ = task.id;
        loading_ = utf8(task.path.filename().wstring());
      } catch (const std::exception &e) {
        workspace_.unlock(task.id, SlotLock::loading);
        error_ = e.what();
      }
    }
  }
  void loaded(std::optional<SlotId> target = {}) {
    const auto id = (target ? *target : active_slot().id());
    auto *slot = workspace_.find(id);
    if (!slot)
      return;
    slot_ui_[id] = std::make_unique<SlotUiState>();
    apply_preferences(id, *slot_ui_[id]);
    auto &state = ui_for(id);
    if (options_.analysis_smoke)
      state.page = options_.analysis_page;
    if (options_.deformable_smoke) {
      state.page = 8;
      const auto frame = slot->document().frame();
      if (!frame || frame->deformables.cloths.empty())
        throw std::runtime_error("Native smoke needs a recorded cloth");
      slot->document().fit_deformable(frame->deformables.cloths.front().key,
                                      slot->camera(), 1.5);
      slot->set_camera_ready();
    }
    if (options_.query_smoke) {
      state.page = 9;
      auto &doc = slot->document();
      if (doc.frame()->scene_queries.queries.empty())
        throw std::runtime_error("Query smoke needs recorded native queries");
      doc.select_query(doc.frame()->scene_queries.queries.front().source);
      doc.fit_query(slot->camera(), 1.5);
      doc.clear_selection();
      slot->set_camera_ready();
    }
    if (renderer_)
      renderer_->clear_mesh_cache();
#ifdef PVD_VIEWER_HAS_ANALYSIS
    if (options_.analysis_smoke &&
        (options_.analysis_page == 4 || options_.analysis_page == 5)) {
      auto &doc = slot->document();
      bool found = false;
      for (const auto &actor : doc.actor_tree()) {
        const auto *object = doc.frame()->snapshot.instance(actor.key.id);
        if (!object || object->class_name.name != "PxRigidDynamic" ||
            !object->property("GlobalPose"))
          continue;
        doc.inspect(actor.key.id);
        found = true;
        if (actor.name == "falling_box")
          break;
      }
      if (!found)
        throw std::runtime_error("Curve smoke capture has no dynamic actor");
      state.curves.add_inspected(doc, "GlobalPose", "position.y");
      if (doc.timeline().size() < 2)
        throw std::runtime_error("Curve smoke needs at least two frames");
      state.curves.set_diff_frames(0, doc.timeline().size() - 1);
      state.curves.refresh_diff(doc);
    }
#endif
#ifdef PVD_VIEWER_HAS_ANALYSIS
    if (options_.analysis_smoke && options_.analysis_page == 7) {
      pvd::analysis::ProfileCurveRequest request;
      request.metric = pvd::analysis::ProfileMetric::count;
      state.profile_curves.add_series(slot->document(), request,
                                      "Observed events");
    }
#endif
    if (workspace_.active_id() == id)
      update_title();
  }
#ifdef PVD_VIEWER_HAS_TRANSPORT
  void copy_path(const std::filesystem::path &path) {
    const auto text = utf8(path.wstring());
    if (text.size() >= capture_path_.size())
      throw std::runtime_error(
          "Recording path is too long for the capture field");
    capture_path_.fill('\0');
    std::copy(text.begin(), text.end(), capture_path_.begin());
  }
  void default_recording_path() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[128]{};
    swprintf_s(name, L"capture-%04u%02u%02u-%02u%02u%02u-%03u.pxd2", time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    copy_path((settings_directory_.empty()
                   ? std::filesystem::current_path() / L"captures"
                   : settings_directory_ / L"Captures") /
              name);
  }
  void choose_recording() {
    std::wstring buffer(32768, L'\0');
    if (capture_path_[0]) {
      const auto path = std::filesystem::u8path(capture_path_.data()).wstring();
      std::copy_n(path.data(), std::min(path.size(), buffer.size() - 1),
                  buffer.data());
    }
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = window_;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter =
        L"PhysX raw capture (*.pxd2)\0*.pxd2\0All files\0*.*\0";
    dialog.lpstrDefExt = L"pxd2";
    dialog.lpstrTitle = L"Choose a new recording file";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog)) {
      buffer.resize(wcslen(buffer.c_str()));
      copy_path(buffer);
    }
  }
  void start_capture() {
    std::optional<SlotId> reserved;
    try {
      if (live_.active())
        throw std::runtime_error("A live receiver is already active.");
      pvd::CaptureOptions options;
      // Display uses immutable indexed views; legacy full-state checkpoints
      // would periodically stall reception and the UI without helping it.
      options.session_options.max_checkpoints = 0;
      options.address = capture_address_.data();
      if (capture_port_ < 0 || capture_port_ > 65535)
        throw std::runtime_error(
            "Port must be in [0,65535]; 0 chooses a free port");
      options.port = static_cast<std::uint16_t>(capture_port_);
      if (record_capture_) {
        if (!capture_path_[0])
          default_recording_path();
        auto path = std::filesystem::absolute(
                        std::filesystem::u8path(capture_path_.data()))
                        .lexically_normal();
        if (!path.parent_path().empty())
          std::filesystem::create_directories(path.parent_path());
        options.recording_path = utf8(path.wstring());
        copy_path(path);
      }
      const auto id = new_target();
      if (!workspace_.lock(id, SlotLock::live))
        throw std::runtime_error("Cannot reserve a tab for live reception.");
      reserved = id;
      live_.start(options);
      live_slot_id_ = id;
      live_recording_path_ = options.recording_path;
      live_bound_ = false;
      activate_slot(id);
      workspace_.rename(id, "Live capture");
      loaded(id);
      live_document_seen_ = false;
      last_live_event_count_.reset();
      online_render_frames_ = live_display_changes_ = 0;
      next_live_poll_ = {};
      error_.clear();
    } catch (const std::exception &e) {
      if (reserved)
        workspace_.unlock(*reserved, SlotLock::live);
      error_ = e.what();
    }
  }
  void poll_live() {
    if (!live_slot_id_)
      return;
    const auto id = *live_slot_id_;
    auto *slot = workspace_.find(id);
    if (!slot) {
      if (live_.active()) {
        live_.stop();
        error_ = "The bound live tab is unavailable.";
      } else
        live_slot_id_.reset();
      return;
    }
    if (export_job_.running() && export_slot_id_ == id)
      return;
#ifdef PVD_VIEWER_HAS_REPX
    if (repx_job_.running() && repx_slot_id_ == id)
      return;
#endif
    const auto now = std::chrono::steady_clock::now();
    if (now < next_live_poll_)
      return;
      next_live_poll_ = now + std::chrono::milliseconds(16);
    try {
      const auto *previous = slot->document().session();
      const bool changed = live_.poll(slot->document());
      if (!live_document_seen_ && slot->document().session() != previous &&
          live_.status().header_complete) {
        loaded(id);
        live_document_seen_ = true;
      }
      if (changed)
        ++live_display_changes_;
      if (!live_.error().empty())
        error_ = live_.error();
      if (!live_.active()) {
        workspace_.unlock(id, SlotLock::live);
        const auto &status = live_.status();
        if (!live_bound_ && !live_recording_path_.empty() &&
            !slot->document().live() && slot->document().loaded() &&
            status.recorded_bytes >= 28 &&
            status.recorded_bytes == status.recoverable_bytes &&
            slot->document().session()->event_count() ==
                status.recoverable_event_count) {
          live_bound_ = begin_binding(id, live_recording_path_);
          if (status.state != pvd::CaptureState::completed)
            notice_ =
                "The stopped recording contains its recoverable event prefix; "
                "discarded tails remain reported by capture status.";
        }
      }
    } catch (const std::exception &e) {
      live_.stop();
      error_ = e.what();
    }
  }
  void capture_setup() {
    if (capture_setup_requested_) {
      if (!capture_path_[0])
        default_recording_path();
      ImGui::OpenPopup("Live capture");
      capture_setup_requested_ = false;
    }
    ImGui::SetNextWindowSize({ui_pixels(580), 0}, ImGuiCond_Appearing);
    if (!ui::begin_popup_modal("Live capture", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
      return;
    ImGui::TextUnformatted("Receive a PhysX PVD connection");
    ImGui::TextWrapped("Start listening, then connect the PhysX application to "
                       "the address and port below.");
    ImGui::Spacing();
    ImGui::BeginDisabled(live_.active());
    ImGui::SetNextItemWidth(ui_pixels(290));
    ImGui::InputText("Bind address", capture_address_.data(),
                     capture_address_.size());
    ImGui::SetNextItemWidth(ui_pixels(140));
    ImGui::InputInt("Port", &capture_port_);
    ImGui::TextDisabled(
        "5425 is the PVD default; 0 chooses an available port.");
    ImGui::Checkbox("Record raw capture", &record_capture_);
    ImGui::BeginDisabled(!record_capture_);
    ImGui::SetNextItemWidth(ui_pixels(425));
    ImGui::InputText("##recording", capture_path_.data(), capture_path_.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
      choose_recording();
    ImGui::TextDisabled("Choose a new file. Existing recordings are kept.");
    ImGui::EndDisabled();
    if (ImGui::Button("Start listening", {ui_pixels(150), 0})) {
      start_capture();
      if (error_.empty())
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close"))
      ImGui::CloseCurrentPopup();
    if (!error_.empty())
      ImGui::TextWrapped("%s", error_.c_str());
    ui::end_popup();
  }
  void capture_bar() {
    const auto &status = live_.status();
    if (status.state == pvd::CaptureState::idle)
      return;
    const bool failed = status.state == pvd::CaptureState::failed ||
                        status.state == pvd::CaptureState::truncated;
    ImGui::TextColored(failed ? ImVec4{1, .57f, .42f, 1}
                              : ImVec4{.28f, .86f, .75f, 1},
                       "%s", pvd::capture_state_name(status.state));
    ImGui::SameLine();
    ImGui::TextDisabled("%s:%u", status.address.c_str(), unsigned(status.port));
    ImGui::SameLine();
    ImGui::Text("%.2f MiB  |  %zu frames",
                double(status.wire_bytes) / (1024 * 1024), status.frame_count);
    ImGui::SameLine();
    ImGui::BeginDisabled(!live_.active());
    if (ImGui::SmallButton("Stop"))
      live_.stop();
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool follow = live_.follow_live();
    if (ImGui::Checkbox("Follow live", &follow))
      live_.set_follow_live(follow);
    if (live_slot_id_ && workspace_.find(*live_slot_id_) &&
        workspace_.active_id() != live_slot_id_) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Show live tab"))
        activate_slot(*live_slot_id_);
    }
    if (export_slot_id_ == live_slot_id_ && export_job_.running()) {
      ImGui::SameLine();
      ImGui::TextDisabled("Snapshot import paused; recording continues");
    }
    if (status.recoverable_event_count > live_.accepted_event_count()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%zu events pending",
                          status.recoverable_event_count -
                              live_.accepted_event_count());
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Received %llu bytes; recorded %llu; recoverable %llu; discarded "
          "tail %llu",
          static_cast<unsigned long long>(status.wire_bytes),
          static_cast<unsigned long long>(status.recorded_bytes),
          static_cast<unsigned long long>(status.recoverable_bytes),
          static_cast<unsigned long long>(status.discarded_tail_bytes));
    if (!status.errors.empty())
      ImGui::TextWrapped("%s", status.errors.front().c_str());
    if(live_slot_id_)if(const auto *slot=workspace_.find(*live_slot_id_)) {
      const auto frame=slot->document().frame();
      if(!slot->document().follow_live()&&frame&&!frame->errors.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,{1,.72f,.38f,1});
        ImGui::TextWrapped("Paused view: %s",frame->errors.back().c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::Separator();
  }
  void live_smoke_step() {
    if (!live_slot_id_ || !workspace_.find(*live_slot_id_))
      throw std::runtime_error("Live smoke source slot was lost");
    auto &doc = workspace_.find(*live_slot_id_)->document();
    const auto &status = live_.status();
    if (!live_background_started_ &&
        status.state == pvd::CaptureState::receiving &&
        status.frame_count >= 80 && doc.loaded()) {
      live_background_slot_ =
          workspace_.create_empty("Background ownership test");
      live_background_prefix_ = doc.live_event_count();
      live_background_started_ = true;
      activate_slot(live_background_slot_);
      return;
    }
    if (live_background_started_ &&
        workspace_.active_id() == live_background_slot_) {
      if (doc.live_event_count() > live_background_prefix_)
        live_background_verified_ = true;
      if (status.frame_count >= 180 || status.terminal()) {
        activate_slot(*live_slot_id_);
        return;
      }
    }
    if (!options_.live_snapshot.empty() && !live_snapshot_started_ &&
        status.state == pvd::CaptureState::receiving &&
        status.frame_count >= 40 && doc.loaded()) {
      ExportRequest request;
      request.destination = utf8(options_.live_snapshot.wstring());
      start_export(*live_slot_id_, request);
      live_snapshot_started_ = true;
    }
    if (workspace_.active_id() == live_slot_id_ && doc.loaded() &&
        !doc.frame()->draws.empty() &&
        status.state == pvd::CaptureState::receiving) {
      ++online_render_frames_;
      last_live_event_count_ = doc.frame()->snapshot.event_count;
    }
    if (!status.terminal() || live_.active() || !live_.caught_up())
      return;
    if (export_job_.running() || bind_identity_.valid())
      return;
    if (!live_background_verified_ || !workspace_.find(live_background_slot_) ||
        workspace_.find(live_background_slot_)->document().loaded())
      throw std::runtime_error("Switching tabs redirected live Session import");
    if (status.state != pvd::CaptureState::completed ||
        !status.errors.empty() || !live_.error().empty())
      throw std::runtime_error("Live GPU capture did not finish cleanly");
    if (online_render_frames_ < 3 || live_display_changes_ < 2 ||
        !renderer_->stats().triangles)
      throw std::runtime_error(
          "The GPU did not render enough changing frames while receiving");
    if (status.wire_bytes != status.recorded_bytes ||
        status.discarded_tail_bytes ||
        status.event_count != live_.accepted_event_count())
      throw std::runtime_error(
          "Live recording did not preserve the complete event prefix");
    Document replay;
    replay.open_file(utf8(options_.recording.wstring()));
    if (!replay.session() ||
        replay.session()->event_count() != doc.session()->event_count() ||
        replay.scenes().size() != doc.scenes().size())
      throw std::runtime_error(
          "Live recording cannot be reopened with identical indices");
    std::size_t snapshot_events = 0;
    if (!options_.live_snapshot.empty()) {
      if (!live_snapshot_started_ ||
          export_job_.status().stage != ExportStage::completed)
        throw std::runtime_error("Live snapshot export did not complete");
      Document snapshot;
      snapshot.open_file(utf8(options_.live_snapshot.wstring()));
      snapshot_events = snapshot.session()->event_count();
      if (!snapshot_events || snapshot_events >= doc.session()->event_count())
        throw std::runtime_error(
            "The snapshot was not captured during reception");
#ifdef PVD_VIEWER_HAS_ANALYSIS
      const auto difference = pvd::analysis::diff_snapshots(
          doc.session()->snapshot_at_event(snapshot_events),
          snapshot.session()->latest());
      if (!difference.complete || !difference.objects.empty() ||
          !difference.cameras.empty() || !difference.origin_shifts.empty())
        throw std::runtime_error("Live snapshot changed source state");
#endif
    }
    if (!options_.screenshot.empty())
      screenshot();
    if (!options_.report.empty()) {
      std::ofstream report(options_.report);
      report << "{\n  \"passed\":true,\n  \"state\":\"completed\",\n  "
                "\"online_render_frames\":"
             << online_render_frames_
             << ",\n  \"live_display_changes\":" << live_display_changes_
             << ",\n  \"wire_bytes\":" << status.wire_bytes
             << ",\n  \"recorded_bytes\":" << status.recorded_bytes
             << ",\n  \"events\":" << status.event_count
             << ",\n  \"frames\":" << status.frame_count
             << ",\n  \"reopened\":true,\n  \"stable_live_slot\":true";
      if (snapshot_events)
        report << ",\n  \"snapshot_events\":" << snapshot_events;
      report << "\n}\n";
      if (!report)
        throw std::runtime_error("Cannot write live GPU smoke report");
    }
    quit_ = true;
  }
#endif
  void shortcuts() {
    const auto &io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                        ImGuiPopupFlags_AnyPopupLevel))
      return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      // NewFrame may already have dismissed a native popup. That same Escape
      // belongs to the popup and must not also close its owning drawer.
      if (!popup_open_at_frame_start_)
        ui().sidebar = SlotUiState::Sidebar::none;
      return;
    }
    if(io.KeyCtrl&&io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_R,false)) {
      try{restore_default_preferences();}catch(const std::exception &e){error_=e.what();}
      return;
    }
    if(io.KeyCtrl&&io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_S,false)) {
      ui().page=0;ui().view_popup_requested=true;return;
    }
    if(io.KeyCtrl&&io.KeyAlt&&document().loaded()) {
      std::optional<ClipEdit> edit;
      if(ImGui::IsKeyPressed(ImGuiKey_Y,false))edit=ClipEdit::split;
      else if(ImGui::IsKeyPressed(ImGuiKey_S,false))edit=ClipEdit::trim_left;
      else if(ImGui::IsKeyPressed(ImGuiKey_E,false))edit=ClipEdit::trim_right;
      if(edit) {
        ui().page=10;
        try{edit_clip_at_playhead(*edit);}
        catch(const std::exception &e){ui().clips.error=e.what();}
        return;
      }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
      choose_file();
      return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
        document().loaded()) {
      export_popup_requested_ = true;
      export_popup_slot_ = active_slot().id();
      return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
      try {
        workspace_.add_bookmark(
            active_slot().id(),
            "Frame " + std::to_string(document().frame_index() + 1), true);
        notice_ = "Current frame and camera bookmarked.";
      } catch (const std::exception &e) {
        error_ = e.what();
      }
      return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
      help_requested_ = true;
      return;
    }
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper || ImGui::IsAnyItemFocused() ||
        !document().loaded())
      return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
      document().set_playing(!document().playback().playing);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
      document().set_playing(false);
      document().step(-1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
      document().set_playing(false);
      document().step(1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
      document().set_frame(0);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false) &&
        !document().timeline().empty())
      document().set_frame(document().timeline().size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
      stopTracking(ui().scene_tools);
      set_fitted(document().fit_scene(
          camera(), viewport_size_.x / std::max(1.f, viewport_size_.y)));
    }
  }
  std::optional<ExportDestination> choose_export_path(const char *name) {
    std::wstring buffer(32768, L'\0');
    const auto initial = std::filesystem::u8path(name).wstring();
    std::copy(initial.begin(), initial.end(), buffer.begin());
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = window_;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const bool repx = std::filesystem::u8path(name).extension() == L".repx";
    dialog.lpstrFilter =
        repx ? L"PhysX RepX XML (*.repx;*.xml)\0*.repx;*.xml\0All files\0*.*\0"
             : L"PhysX raw capture (*.pxd2)\0*.pxd2\0All files\0*.*\0";
    dialog.lpstrDefExt = repx ? L"repx" : L"pxd2";
    dialog.lpstrTitle = repx ? L"Export PhysX RepX" : L"Save raw capture";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog))
      return std::nullopt;
    buffer.resize(wcslen(buffer.c_str()));
    protect_recording_path(std::filesystem::path(buffer));
    if (repx)
      for (const auto *slot : workspace_.slots())
        if (!slot->source_path().empty() &&
            same_path(std::filesystem::path(buffer),
                      std::filesystem::u8path(slot->source_path())))
          throw std::runtime_error(
              "Choose an XML destination different from an open raw capture.");
    return ExportDestination{
        utf8(buffer), std::filesystem::exists(std::filesystem::path(buffer))};
  }
  void export_popup() {
    if (export_popup_requested_) {
      if (!export_popup_slot_)
        export_popup_slot_ = active_slot().id();
      ImGui::OpenPopup("Save / crop capture");
      export_popup_requested_ = false;
    }
    ImGui::SetNextWindowSize({ui_pixels(660), ui_pixels(530)}, ImGuiCond_Appearing);
    if (!ui::begin_popup_modal("Save / crop capture", nullptr,
                                ImGuiWindowFlags_None))
      return;
    const auto id = export_popup_slot_.value_or(active_slot().id());
    auto *slot = workspace_.find(id);
    if (!slot) {
      ImGui::TextWrapped("The source tab was closed.");
      draw_export_progress(export_job_);
      if (ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
      ui::end_popup();
      return;
    }
    bool other_lock = slot->lock_reason() &&
                      slot->lock_reason() != SlotLock::live &&
                      slot->lock_reason() != SlotLock::exporting;
#ifdef PVD_VIEWER_HAS_REPX
    other_lock = other_lock || repx_job_.running();
#endif
    if (other_lock) {
      ImGui::TextWrapped("Finish this tab's current operation before saving.");
      if (ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
      ui::end_popup();
      return;
    }
    if (slot->document().live()) {
      ImGui::TextUnformatted("SAVE LIVE SNAPSHOT");
      ImGui::TextWrapped("Save this tab's complete safe event prefix. Model "
                         "import pauses while the worker borrows it; network "
                         "reception and raw recording continue. Native-frame "
                         "cropping is available after reception stops.");
      ImGui::BeginDisabled(export_job_.running() ||
                           !slot->document().session());
      const bool save = ImGui::Button("Choose output and save snapshot...");
      ImGui::EndDisabled();
      if (save)
        try {
          if (auto path = choose_export_path("live-snapshot.pxd2")) {
            ExportRequest request;
            request.destination = path->path;
            request.overwrite = path->overwrite;
            start_export(id, std::move(request));
          }
        } catch (const std::exception &e) {
          error_ = e.what();
        }
      draw_export_progress(export_job_);
      if (ImGui::Button("Hide"))
        ImGui::CloseCurrentPopup();
    } else {
      const auto result = draw_export_panel(
          slot->document(), export_job_, ui_for(id).export_panel,
          [&](const char *name) { return choose_export_path(name); });
      if (result.started) {
        if (!workspace_.lock(id, SlotLock::exporting)) {
          export_job_.cancel();
          error_ = "Cannot retain the source tab for export.";
        } else {
          export_slot_id_ = id;
          last_export_slot_id_ = id;
          export_resume_live_ = false;
          export_mode_ = ui_for(id).export_panel.mode;
          export_destination_ = export_job_.status().destination;
          export_status_visible_ = true;
        }
      }
      if (result.close_requested)
        ImGui::CloseCurrentPopup();
    }
    ui::end_popup();
  }
#ifdef PVD_VIEWER_HAS_REPX
  void start_repx(SlotId id, RepxRequest request) {
    auto *slot = workspace_.find(id);
    if (!slot || !slot->document().session())
      throw std::runtime_error("Choose a loaded capture for RepX export.");
    if (export_job_.running() || repx_job_.running())
      throw std::runtime_error("Another export is still running.");
    bool resume = false;
    if (slot->lock_reason()) {
#ifdef PVD_VIEWER_HAS_TRANSPORT
      if (slot->lock_reason() == SlotLock::live && live_slot_id_ == id) {
        workspace_.unlock(id, SlotLock::live);
        resume = live_.active();
      } else
#endif
        throw std::runtime_error("This tab is locked by another operation.");
    }
    if (!workspace_.lock(id, SlotLock::exporting))
      throw std::runtime_error("Cannot borrow this tab for RepX export.");
    try {
      repx_job_.start(*slot->document().session(), std::move(request));
      repx_slot_id_ = id;
      repx_resume_live_ = resume;
      repx_status_visible_ = true;
    } catch (...) {
      workspace_.unlock(id, SlotLock::exporting);
#ifdef PVD_VIEWER_HAS_TRANSPORT
      if (resume)
        workspace_.lock(id, SlotLock::live);
#endif
      throw;
    }
  }
  void repx_smoke_step() {
    if (host_frames_ < 6 || repx_job_.running() || repx_slot_id_)
      return;
    const auto status = repx_job_.status();
    if (status.stage != RepxStage::completed ||
        (!status.complete && !options_.allow_partial) || !status.actors)
      throw std::runtime_error("RepX export failed: " + status.error);
    if (std::filesystem::file_size(options_.output) != status.xml_bytes)
      throw std::runtime_error(
          "RepX output size differs from its completed worker result");
    if (!options_.screenshot.empty())
      screenshot();
    if (!options_.report.empty()) {
      std::ofstream report(options_.report);
      report << "{\"passed\":true,\"complete\":"
             << (status.complete ? "true" : "false") << ",\"allow_partial\":"
             << (options_.allow_partial ? "true" : "false")
             << ",\"diagnostics\":" << status.diagnostics.size()
             << ",\"event_count\":" << status.event_count
             << ",\"actors\":" << status.actors
             << ",\"shapes\":" << status.shapes
             << ",\"joints\":" << status.joints
             << ",\"xml_bytes\":" << status.xml_bytes
             << ",\"auto_settings_io\":"
             << (automatic_settings_ ? "true" : "false") << "}\n";
      if (!report)
        throw std::runtime_error("Cannot write RepX host smoke report");
    }
    quit_ = true;
  }
  void poll_repx() {
    if (!repx_slot_id_ || repx_job_.running())
      return;
    const auto id = *repx_slot_id_;
    workspace_.unlock(id, SlotLock::exporting);
#ifdef PVD_VIEWER_HAS_TRANSPORT
    if (repx_resume_live_ && live_slot_id_ == id && live_.active())
      workspace_.lock(id, SlotLock::live);
#endif
    repx_resume_live_ = false;
    repx_slot_id_.reset();
    const auto status = repx_job_.status();
    if (status.stage == RepxStage::failed)
      error_ = status.error;
  }
  void repx_popup() {
    if (repx_popup_requested_) {
      if (!repx_popup_slot_)
        repx_popup_slot_ = active_slot().id();
      ImGui::OpenPopup("Export RepX");
      repx_popup_requested_ = false;
    }
    ImGui::SetNextWindowSize({ui_pixels(650), ui_pixels(600)}, ImGuiCond_Appearing);
    if (!ui::begin_popup_modal("Export RepX", nullptr, ImGuiWindowFlags_None))
      return;
    const auto id = repx_popup_slot_.value_or(active_slot().id());
    auto *slot = workspace_.find(id);
    if (!slot) {
      ImGui::TextWrapped("The source tab was closed.");
      draw_repx_progress(repx_job_);
      if (ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
      ui::end_popup();
      return;
    }
    if (export_job_.running() ||
        (slot->lock_reason() && slot->lock_reason() != SlotLock::live &&
         slot->lock_reason() != SlotLock::exporting)) {
      ImGui::TextWrapped("Finish this tab's load or the current raw export "
                         "before exporting RepX.");
      if (ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
      ui::end_popup();
      return;
    }
    const auto result = draw_repx_panel(
        slot->document(), repx_job_, ui_for(id).repx_panel,
        [&](const char *name) { return choose_export_path(name); });
    if (result.started) {
      bool resume = false;
      if (slot->lock_reason() == SlotLock::live) {
        workspace_.unlock(id, SlotLock::live);
#ifdef PVD_VIEWER_HAS_TRANSPORT
        resume = live_slot_id_ == id && live_.active();
#endif
      }
      if (!workspace_.lock(id, SlotLock::exporting)) {
        repx_job_.cancel();
        error_ = "Cannot retain the source tab for RepX export.";
      } else {
        repx_slot_id_ = id;
        repx_resume_live_ = resume;
        repx_status_visible_ = true;
      }
    }
    if (result.close_requested)
      ImGui::CloseCurrentPopup();
    ui::end_popup();
  }
#endif
  void clip_smoke_step() {
    if(host_frames_<4)return;
    const auto require=[](bool ok,const char *message) {
      if(!ok)throw std::runtime_error(message);
    };
    if(clip_smoke_phase_==0) {
      document().set_sample_point(SamplePoint::native_frame);
      require(document().timeline().size()>12,"Clip smoke needs at least thirteen native frames");
      smoke_slot_a_=active_slot().id();smoke_session_a_=document().session();
      document().set_frame(10);
      edit_clip_at_playhead(ClipEdit::split);
      document().set_frame(2);
      edit_clip_at_playhead(ClipEdit::trim_left);
      document().set_frame(7);
      edit_clip_at_playhead(ClipEdit::trim_right);
      auto &state=ui().clips;
      auto &catalog=state.catalogs.at(*state.active_catalog);
      const auto selected=catalog.selected()->id;
      catalog.rename(*document().session(),selected,u8"关键帧 / 3–8");
      const auto temporary=catalog.add(*document().session(),"temporary",{3,4});
      require(catalog.erase(*document().session(),temporary),"Temporary clip deletion failed");
      require(catalog.select(*document().session(),selected),"Stable clip selection was lost");
      persist_clips({smoke_slot_a_,ClipPanelChangeKind::edited});
      clip_smoke_saved_=*catalog.selected();
      require(clip_smoke_saved_->frames.first==2&&clip_smoke_saved_->frames.last==7&&
              catalog.state().clips.size()==2,"Split/trim clip ranges are incorrect");
      clip_smoke_end_prefix_=document().timeline()[7].event_count;
      save_clip({smoke_slot_a_,*clip_smoke_saved_});
      auto candidate=std::make_unique<Document>();
      require(!workspace_.close(smoke_slot_a_)&&!workspace_.replace(smoke_slot_a_,candidate),
              "Clip export failed to retain its source");
      smoke_slot_b_=workspace_.create_empty("Other tab during clip export");
      activate_slot(smoke_slot_b_);
      clip_smoke_phase_=1;return;
    }
    if(clip_smoke_phase_==1) {
      if(export_job_.running()||export_slot_id_)return;
      const auto status=export_job_.status();
      require(status.stage==ExportStage::completed,"Clip asynchronous export failed");
      require(workspace_.active_id()==smoke_slot_b_&&last_export_slot_id_==smoke_slot_a_&&
              workspace_.find(smoke_slot_a_)->document().session()==smoke_session_a_,
              "Clip export retargeted the active tab");
      Document replay;replay.open_file(utf8(options_.output.wstring()));
      require(replay.set_scene(clip_smoke_saved_->binding.scene.id),"Exported clip lost its scene");
      replay.set_sample_point(SamplePoint::native_frame);
      require(replay.timeline().size()==6,"Exported clip native range is incorrect");
#ifdef PVD_VIEWER_HAS_ANALYSIS
      const auto diff=pvd::analysis::diff_snapshots(
          smoke_session_a_->snapshot_at_event(clip_smoke_end_prefix_),replay.session()->latest());
      require(diff.complete&&diff.objects.empty()&&diff.cameras.empty()&&diff.origin_shifts.empty(),
              "Exported clip end state differs from its original source");
#endif
      workspace_.save_file(utf8(options_.workspace_state.wstring()));
      restore_workspace(options_.workspace_state);
      clip_smoke_phase_=2;return;
    }
    if(clip_smoke_phase_==2) {
      if(load_job_.running()||loading_slot_||!load_queue_.empty())return;
      auto *slot=workspace_.find(smoke_slot_a_);
      require(slot&&slot->document().loaded(),"Workspace did not reload the clip source");
      require(slot->pending_clip_states().empty()&&slot->clip_catalogs().size()==1,
              "Workspace did not verify the saved clip seal");
      const auto &catalog=slot->clip_catalogs().begin()->second;
      const auto *clip=catalog.find(clip_smoke_saved_->id);
      require(catalog.state().clips.size()==2&&clip&&clip->name==clip_smoke_saved_->name&&
              clip->frames.first==2&&clip->frames.last==7&&
              clip->binding.source==clip_smoke_saved_->binding.source,
              "Workspace changed a stable clip identity, name or native range");
      activate_slot(smoke_slot_a_);ui().page=10;document().set_frame(7);
      clip_smoke_phase_=3;return;
    }
    if(clip_smoke_phase_==3) {
      // Two actual host draws allow the requested tab and list clipping to settle.
      clip_smoke_phase_=4;return;
    }
    require(ui().clips.visible_rows==2&&ui().clips.error.empty(),
            "Restored clip list did not draw both rows");
    if(std::filesystem::exists(L"C:/Windows/Fonts/msyh.ttc")&&
        std::filesystem::exists(L"C:/Windows/Fonts/segoeui.ttf"))
      require(ImGui::GetFont()->FindGlyphNoFallback(0x5e27)!=nullptr,
              "The native-frame glyph is missing from the clip-name font");
    if(!options_.screenshot.empty())screenshot();
    if(!options_.report.empty()) {
      std::ofstream report(options_.report);
      report<<"{\"passed\":true,\"split_trim\":true,\"stable_source\":true,"
               "\"export_verified\":true,\"workspace_verified\":true,"
               "\"visible_rows\":"<<ui().clips.visible_rows<<",\"native_frames\":6,"
               "\"auto_settings_io\":"<<(automatic_settings_?"true":"false")<<"}\n";
      if(!report)throw std::runtime_error("Cannot write clip workflow report");
    }
    quit_=true;
  }
  void workspace_smoke_step() {
    auto require = [](bool condition, const char *message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    auto camera_equal = [](const OrbitCamera &a, const OrbitCamera &b) {
      return a.target.x == b.target.x && a.target.y == b.target.y &&
             a.target.z == b.target.z && a.reference_up.x == b.reference_up.x &&
             a.reference_up.y == b.reference_up.y &&
             a.reference_up.z == b.reference_up.z && a.yaw == b.yaw &&
             a.pitch == b.pitch && a.distance == b.distance;
    };
    if (workspace_smoke_phase_ == 0) {
      if (load_job_.running() || loading_slot_ || !load_queue_.empty() ||
          !document().loaded())
        return;
      require(document().timeline().size() >= 5,
              "Workspace smoke needs at least five native frames");
      smoke_slot_a_ = active_slot().id();
      smoke_session_a_ = document().session();
      document().set_sample_point(SamplePoint::native_frame);
      document().set_frame(2);
      require(document().frame() && !document().frame()->draws.empty(),
              "Workspace smoke needs a visible shape");
      document().select(document().frame()->draws.front().key);
      camera().yaw = 0.4;
      camera().pitch = 0.3;
      camera().distance = 14;
      set_fitted(true);
      smoke_camera_a_ = camera();
      std::copy_n("workspace-a", 12, ui().panels.hierarchy_search.begin());
#ifdef PVD_VIEWER_HAS_ANALYSIS
      bool curve = false;
      for (const auto &actor : document().actor_tree()) {
        const auto *object =
            document().frame()->snapshot.instance(actor.key.id);
        if (object && object->class_name.name == "PxRigidDynamic" &&
            object->property("GlobalPose")) {
          document().inspect(actor.key.id);
          ui().curves.add_inspected(document(), "GlobalPose", "position.y");
          curve = true;
          break;
        }
      }
      require(curve, "Workspace smoke needs a recorded dynamic pose curve");
#endif
      smoke_bookmark_ =
          workspace_.add_bookmark(smoke_slot_a_, "Camera / frame 3", true);
      smoke_slot_b_ = workspace_.create_empty("Second capture");
      schedule_load(smoke_slot_b_, options_.capture);
      activate_slot(smoke_slot_a_);
      workspace_smoke_phase_ = 1;
      return;
    }
    if (workspace_smoke_phase_ == 1) {
      if (load_job_.running() || loading_slot_ || !load_queue_.empty())
        return;
      auto *a = workspace_.find(smoke_slot_a_);
      auto *b = workspace_.find(smoke_slot_b_);
      require(a && b && b->document().loaded(),
              "Second capture did not finish loading");
      smoke_workspace_target_ = workspace_.active_id() == smoke_slot_a_ &&
                                a->document().session() == smoke_session_a_;
      require(smoke_workspace_target_,
              "Background loading changed the active tab or its Session");
      activate_slot(smoke_slot_b_);
      document().set_frame(4);
      camera().yaw = -0.7;
      camera().pitch = 0.15;
      camera().distance = 21;
      set_fitted(true);
      smoke_camera_b_ = camera();
      auto view = document().view_options();
      view.color_mode = ColorMode::uniform;
      view.uniform_color = {42, 177, 160, 255};
      view.geometry_scale=1.25f;view.show_planes=false;
      view.joint_frame_scale=.75f;view.joint_limit_scale=1.5f;
      view.palette.active={.17f,.31f,.57f,.9f};
      view.palette.profile_by_event=false;view.palette.profile_bar={.73f,.19f,.27f,1};
      document().set_view_options(view);
      ui().render_options.antialiasing = Antialiasing::msaa4x;
      ui().render_options.background_color = {.12f, .08f, .17f, 1};
      ui().render_options.face_culling=FaceCulling::front;
      ui().render_options.show_orbit_target=true;
      ui().panels.inspector_preferences.number_format = NumberFormat::scientific;
      ui().panels.inspector_preferences.array_page_size = 512;
      store_preferences(smoke_slot_b_);
      activate_slot(smoke_slot_a_);
      smoke_workspace_state_ =
          document().frame_index() == 2 &&
          camera_equal(camera(), smoke_camera_a_) &&
          document().selected_key().has_value() &&
          std::string(ui().panels.hierarchy_search.data()) == "workspace-a";
#ifdef PVD_VIEWER_HAS_ANALYSIS
      smoke_workspace_curves_ =
          ui().curves.series_count() == 1 &&
          ui_for(smoke_slot_b_).curves.series_count() == 0;
#endif
      require(smoke_workspace_state_ && smoke_workspace_curves_,
              "Switching tabs lost document, camera or curve state");
      require(workspace_.lock(smoke_slot_a_, SlotLock::exporting),
              "Cannot lock source for protection test");
      auto candidate = std::make_unique<Document>();
      smoke_workspace_locks_ = !workspace_.close(smoke_slot_a_) &&
                               !workspace_.replace(smoke_slot_a_, candidate) &&
                               bool(candidate);
      workspace_.unlock(smoke_slot_a_, SlotLock::exporting);
      require(smoke_workspace_locks_, "A locked source was closed or replaced");
      auto output = options_.workspace_state;
      output += L".capture.pxd2";
      ExportRequest request;
      request.destination = utf8(output.wstring());
      start_export(smoke_slot_a_, request);
      activate_slot(smoke_slot_b_);
      workspace_smoke_phase_ = 2;
      return;
    }
    if (workspace_smoke_phase_ == 2) {
      if (export_job_.running() || export_slot_id_)
        return;
      require(export_job_.status().stage == ExportStage::completed,
              "Background workspace export failed");
      require(workspace_.active_id() == smoke_slot_b_,
              "Background export changed the active document");
      Document replay;
      replay.open_file(export_job_.status().destination);
      require(workspace_source_identity(*replay.session()) ==
                  *workspace_.find(smoke_slot_a_)->source_identity(),
              "Saved inactive source differs from its Session");
      workspace_.save_file(utf8(options_.workspace_state.wstring()));
      restore_workspace(options_.workspace_state);
      workspace_smoke_phase_ = 3;
      return;
    }
    if (workspace_smoke_phase_ == 3) {
      if (load_job_.running() || loading_slot_ || !load_queue_.empty())
        return;
      auto *a = workspace_.find(smoke_slot_a_);
      auto *b = workspace_.find(smoke_slot_b_);
      require(a && b && a->document().loaded() && b->document().loaded(),
              "Saved workspace sources did not reload");
      require(workspace_.active_id() == smoke_slot_b_ &&
                  b->document().frame_index() == 4 &&
                  camera_equal(b->camera(), smoke_camera_b_) &&
                  b->document().view_options().color_mode == ColorMode::uniform,
              "Workspace did not restore its active frame, camera and "
              "visibility color");
      require(ui_for(smoke_slot_b_).render_options.antialiasing == Antialiasing::msaa4x &&
                  ui_for(smoke_slot_b_).render_options.background_color[0] == .12f &&
                  ui_for(smoke_slot_b_).panels.inspector_preferences.number_format == NumberFormat::scientific &&
                  ui_for(smoke_slot_b_).panels.inspector_preferences.array_page_size == 512,
              "Workspace did not restore display and inspector preferences");
      const auto restored=b->document().view_options();
      require(restored.geometry_scale==1.25f&&!restored.show_planes&&
              restored.joint_frame_scale==.75f&&restored.joint_limit_scale==1.5f&&
              restored.palette.active==PaletteRgba{.17f,.31f,.57f,.9f}&&
              !restored.palette.profile_by_event&&restored.palette.profile_bar==PaletteRgba{.73f,.19f,.27f,1}&&
              ui_for(smoke_slot_b_).render_options.face_culling==FaceCulling::front&&
              ui_for(smoke_slot_b_).render_options.show_orbit_target,
              "Workspace did not restore the visual geometry, palette or raster preferences");
      const auto *reset_source=b->document().session();
      const auto reset_identity=workspace_source_identity(*reset_source);
      restore_default_preferences();
      require(document().session()==reset_source&&document().frame_index()==4&&
              workspace_source_identity(*reset_source)==reset_identity&&camera_equal(camera(),smoke_camera_b_)&&
              document().view_options().geometry_scale==1&&document().view_options().show_planes&&
              document().view_options().palette.active==ColorPalette{}.active&&
              ui().render_options.face_culling==FaceCulling::back&&!ui().render_options.show_orbit_target&&
              ui().panels.inspector_preferences.number_format==NumberFormat::general,
              "Restoring built-in preferences changed source/playhead/pose or failed to reset display settings");
      require(a->bookmark_verified(smoke_bookmark_),
              "Restored bookmark source identity was not verified");
      workspace_actions().jump_bookmark(smoke_slot_a_, smoke_bookmark_);
      require(document().frame_index() == 2 &&
                  camera_equal(camera(), smoke_camera_a_),
              "Verified bookmark failed to restore frame and camera");
      ui().page = 6;
      workspace_smoke_phase_ = 4;
      return;
    }
    if (workspace_smoke_phase_ == 4) {
      workspace_smoke_phase_ = 5;
      return;
    }
    if (workspace_smoke_phase_ == 5) {
      if (!options_.screenshot.empty())
        screenshot();
      if (!options_.report.empty()) {
        std::ofstream report(options_.report);
        report << "{\"passed\":true,\"stable_loading_target\":true,"
                  "\"independent_state\":true,\"locks_protected\":true,"
                  "\"inactive_export_verified\":true,\"persistence\":true,\"preferences_reset\":true,"
                  "\"identity_verified\":true,\"curves_retained\":"
               << (smoke_workspace_curves_ ? "true" : "false")
               << ",\"auto_settings_io\":"
               << (automatic_settings_ ? "true" : "false")
               << ",\"slots\":" << workspace_.slots().size()
               << ",\"bookmarks\":" << active_slot().bookmarks().size()
               << "}\n";
        if (!report)
          throw std::runtime_error("Cannot write workspace smoke report");
      }
      quit_ = true;
    }
  }
  void export_smoke_step() {
    if (host_frames_ < 6 || export_job_.running())
      return;
    const auto status = export_job_.status();
    if (status.stage != ExportStage::completed)
      throw std::runtime_error("Export failed: " + status.error);
    Document replay;
    replay.open_file(utf8(options_.output.wstring()));
    std::size_t expected_prefix = document().session()->event_count();
    if (options_.crop_first) {
      std::size_t seen = 0;
      bool found = false;
      for (const auto &f : document().session()->frames())
        if (document().scene_id() == f.scene_id) {
          if (++seen == options_.crop_last) {
            expected_prefix = f.event_count;
            found = true;
            break;
          }
        }
      if (!found)
        throw std::runtime_error("Export smoke source frame was not found");
    }
    bool verified = false;
#ifdef PVD_VIEWER_HAS_ANALYSIS
    const auto difference = pvd::analysis::diff_snapshots(
        document().session()->snapshot_at_event(expected_prefix),
        replay.session()->latest());
    if (!difference.complete || !difference.objects.empty() ||
        !difference.cameras.empty() || !difference.origin_shifts.empty())
      throw std::runtime_error(
          "Saved capture state differs from its source prefix");
    verified = true;
#endif
    if (!options_.screenshot.empty())
      screenshot();
    if (!options_.report.empty()) {
      std::ofstream report(options_.report);
      report << "{\"passed\":true,\"state_verified\":"
             << (verified ? "true" : "false")
             << ",\"events\":" << replay.session()->event_count()
             << ",\"frames\":" << replay.session()->frames().size()
             << ",\"bytes\":" << status.result.bytes_written
             << ",\"warnings\":" << status.warnings.size() << "}\n";
      if (!report)
        throw std::runtime_error("Cannot write export smoke report");
    }
    quit_ = true;
  }
#ifdef PVD_VIEWER_HAS_ANALYSIS
  void export_csv(const char *name,
                  const std::function<void(std::ostream &)> &writer) {
    try {
      std::wstring buffer(32768, L'\0');
      const auto initial = std::filesystem::u8path(name).wstring();
      std::copy(initial.begin(), initial.end(), buffer.begin());
      OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
      dialog.hwndOwner = window_;
      dialog.lpstrFile = buffer.data();
      dialog.nMaxFile = static_cast<DWORD>(buffer.size());
      dialog.lpstrFilter = L"CSV file (*.csv)\0*.csv\0All files\0*.*\0";
      dialog.lpstrDefExt = L"csv";
      dialog.lpstrTitle = L"Export analysis CSV";
      dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
      if (!GetSaveFileNameW(&dialog))
        return;
      buffer.resize(wcslen(buffer.c_str()));
      protect_recording_path(std::filesystem::path(buffer));
      for (const auto *slot : workspace_.slots())
        if (!slot->source_path().empty() &&
            same_path(std::filesystem::path(buffer),
                      std::filesystem::u8path(slot->source_path())))
          throw std::runtime_error(
              "Choose a CSV destination different from an open capture.");
      std::ofstream output(std::filesystem::path(buffer),
                           std::ios::binary | std::ios::trunc);
      if (!output)
        throw std::runtime_error("Cannot create CSV file");
      writer(output);
      output.flush();
      if (!output)
        throw std::runtime_error("Cannot finish CSV export");
    } catch (const std::exception &e) {
      error_ = e.what();
    }
  }
  void analysis_smoke_step() {
    if (host_frames_ < 6)
      return;
    const auto page = static_cast<AnalysisPage>(ui().page - 1);
    const auto curve_page =
        ui().page == 5 ? CurvePage::diff : CurvePage::curves;
    const auto rows = ui().page == 7
                          ? ui().profile_curves.row_count()
                          : (ui().page >= 4 ? ui().curves.row_count(curve_page)
                                            : ui().analysis.row_count(page));
    const auto visible =
        ui().page == 7
            ? ui().profile_curves.visible_row_count()
            : (ui().page >= 4 ? ui().curves.visible_row_count(curve_page)
                              : ui().analysis.visible_row_count(page));
    if (ui().page != options_.analysis_page || !rows || !visible)
      throw std::runtime_error(
          "Analysis workspace did not show actual capture data");
    if (ui().page == 7 && !ui().profile_curves.error().empty())
      throw std::runtime_error(ui().profile_curves.error());
    if ((ui().page == 4 || ui().page == 5) && !ui().curves.error().empty())
      throw std::runtime_error(ui().curves.error());
    if (!options_.screenshot.empty())
      screenshot();
    if (!options_.report.empty()) {
      std::ofstream report(options_.report);
      report << "{\"passed\":true,\"page\":" << ui().page
             << ",\"rows\":" << rows << ",\"visible_rows\":" << visible
             << "}\n";
      if (!report)
        throw std::runtime_error("Cannot write analysis GPU report");
    }
    quit_ = true;
  }
#endif
  void deformable_smoke_step() {
    if (host_frames_ < 6) return;
    const auto frame = document().frame();
    const auto stats = renderer_->stats();
    if (ui().page != 8 || !frame || frame->deformables.cloths.empty() ||
        !stats.cloth_points || !stats.particle_points || !stats.cloth_constraints)
      throw std::runtime_error("Native objects did not reach the displayed GPU viewport");
    if (!native_smoke_phase_) {
      unselected_ = readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
      const auto &cloth = frame->deformables.cloths.front();
      if (!document().select_deformable({cloth.key,
          pvd::deformables::ElementKind::cloth_vertex, 0, frame->snapshot.event_count}))
        throw std::runtime_error("Native cloth point selection was rejected");
      native_smoke_phase_ = 1;
      return;
    }
    auto selected = readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
    if (selected.pixels.size() != unselected_.pixels.size())
      throw std::runtime_error("Native viewport changed dimensions during selection");
    for (std::size_t i = 0; i < selected.pixels.size(); i += 4)
      if (selected.pixels[i] != unselected_.pixels[i] ||
          selected.pixels[i+1] != unselected_.pixels[i+1] ||
          selected.pixels[i+2] != unselected_.pixels[i+2]) ++selection_pixels_;
    if (!selection_pixels_) throw std::runtime_error("Native selection did not change GPU pixels");
    if (!options_.screenshot.empty()) screenshot();
    if (!options_.report.empty()) {
      std::ofstream report(options_.report);
      report << "{\"passed\":true,\"cloth_points\":" << stats.cloth_points
             << ",\"particle_points\":" << stats.particle_points
             << ",\"cloth_constraints\":" << stats.cloth_constraints
             << ",\"selection_pixels\":" << selection_pixels_
             << ",\"auto_settings_io\":" << (automatic_settings_ ? "true" : "false") << "}\n";
      if (!report) throw std::runtime_error("Cannot write native GPU smoke report");
    }
    quit_ = true;
  }
  void query_smoke_step() {
    if (host_frames_ < 6) return;
    const auto frame = document().frame();
    const auto stats = renderer_->stats();
    if (ui().page != 9 || !frame || frame->scene_queries.queries.empty() ||
        !stats.query_lines || !stats.query_volumes || !ui().scene_queries.visible_row_count)
      throw std::runtime_error("Recorded scene queries/volumes did not reach the GPU viewport");
    if (!native_smoke_phase_) {
      unselected_ = readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
      if (!document().select_query(frame->scene_queries.queries.front().source))
        throw std::runtime_error("Recorded query selection was rejected");
      native_smoke_phase_ = 1;
      return;
    }
    auto selected = readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
    if (selected.pixels.size() != unselected_.pixels.size())
      throw std::runtime_error("Query viewport changed dimensions during selection");
    for (std::size_t i=0;i<selected.pixels.size();i+=4)
      if (selected.pixels[i]!=unselected_.pixels[i] || selected.pixels[i+1]!=unselected_.pixels[i+1] ||
          selected.pixels[i+2]!=unselected_.pixels[i+2]) ++selection_pixels_;
    if (!selection_pixels_) throw std::runtime_error("Query selection did not change GPU pixels");
    if (!options_.screenshot.empty()) screenshot();
    if (!options_.report.empty()) {
      std::size_t hits=0;for(const auto &query:frame->scene_queries.queries)hits+=query.hits.size();
      std::ofstream report(options_.report);
      report << "{\"passed\":true,\"queries\":" << frame->scene_queries.queries.size()
             << ",\"hits\":" << hits << ",\"query_lines\":" << stats.query_lines
             << ",\"query_volumes\":" << stats.query_volumes
             << ",\"selection_pixels\":" << selection_pixels_
             << ",\"auto_settings_io\":" << (automatic_settings_ ? "true" : "false") << "}\n";
      if (!report) throw std::runtime_error("Cannot write query GPU smoke report");
    }
    quit_ = true;
  }
  void dpi_smoke_step() {
    if (host_frames_ != 4 + dpi_smoke_phase_*2) return;
    constexpr float scales[]={1.f,1.5f,2.f,1.f};
    const auto &actual=ui_scale_->current();
    const auto expected=scales[dpi_smoke_phase_];
    if (dpi_smoke_phase_==0) {
      auto expected_camera=camera();
      if (!fitted() || !document().fit_scene(expected_camera,
              viewport_size_.x/std::max(1.f,viewport_size_.y)) ||
          std::abs(camera().distance-expected_camera.distance)>1e-4)
        throw std::runtime_error("Initial auto-fit used a transient ImGui viewport size");
    }
    RECT client{};GetClientRect(window_,&client);
    // Windows may cap large suggested rectangles at its native maximum track
    // size. Validate actual growth and exact return, not an invented unclamped
    // 2x desktop; all reported dimensions come from the actual client rect.
    const bool base_size=dpi_smoke_phase_==0||dpi_smoke_phase_==3;
    const bool client_size=base_size?
        client.right==options_.window_width&&client.bottom==options_.window_height:
        client.right>options_.window_width&&client.bottom>options_.window_height;
    if (!document().loaded() || !document().frame() || !renderer_->stats().triangles ||
        actual.screen_scale!=expected || actual.generation!=dpi_smoke_phase_+1 ||
        actual.font_pixels!=std::floor(ui::font_size_dp*expected+.5f) ||
        ImGui::GetIO().Fonts->TexID==0 || ImGui::GetIO().FontGlobalScale!=1 ||
        ImGui::GetDrawData()->TotalVtxCount<100 || !client_size) {
      std::ostringstream reason;
      reason << "DPI transition phase " << dpi_smoke_phase_ << ": scale=" << actual.screen_scale
             << " generation=" << actual.generation << " font=" << actual.font_pixels
             << " texture=" << (ImGui::GetIO().Fonts->TexID!=0)
             << " glyph_vertices=" << ImGui::GetDrawData()->TotalVtxCount
             << " triangles=" << renderer_->stats().triangles
             << " client=" << client.right << 'x' << client.bottom << " expected_scale=" << expected;
      throw std::runtime_error(reason.str());
    }
    dpi_smoke_samples_.push_back({static_cast<unsigned>(std::lround(expected*96)),
        static_cast<unsigned>(actual.generation),static_cast<unsigned>(actual.atlas_width),
        static_cast<unsigned>(actual.atlas_height),static_cast<unsigned>(client.right),
        static_cast<unsigned>(client.bottom)});
    if (!options_.screenshot.empty()) {
      const auto original=options_.screenshot;
      if (dpi_smoke_phase_<3)
        options_.screenshot=original.parent_path()/
            (original.stem().wstring()+L"-"+std::to_wstring(std::lround(expected*100))+original.extension().wstring());
      screenshot();options_.screenshot=original;
    }
    if (++dpi_smoke_phase_==4) {
      if (ImGui::GetStyle().WindowPadding.x!=16 || ImGui::GetStyle().FramePadding.x!=9)
        throw std::runtime_error("DPI roundtrip accumulated style rounding");
      if (!options_.report.empty()) {
        if (!options_.report.parent_path().empty())
          std::filesystem::create_directories(options_.report.parent_path());
        std::ofstream report(options_.report,std::ios::binary);
        auto fit_camera=camera();
        document().fit_scene(fit_camera,viewport_size_.x/std::max(1.f,viewport_size_.y));
        report << "{\"passed\":true,\"font_generations\":4,\"auto_settings_io\":"
               << (automatic_settings_?"true":"false")
               << ",\"camera_distance\":" << camera().distance
               << ",\"fit_scene_distance\":" << fit_camera.distance << ",\"samples\":[";
        for (std::size_t i=0;i<dpi_smoke_samples_.size();++i) {
          const auto &s=dpi_smoke_samples_[i];if(i)report<<',';
          report << "{\"dpi\":"<<s[0]<<",\"generation\":"<<s[1]
                 <<",\"atlas_width\":"<<s[2]<<",\"atlas_height\":"<<s[3]
                 <<",\"client_width\":"<<s[4]<<",\"client_height\":"<<s[5]<<'}';
        }
        report<<"]}\n";
        if(!report)throw std::runtime_error("Cannot write DPI smoke report");
      }
      quit_=true;return;
    }
    const auto next=scales[dpi_smoke_phase_];
    RECT outer{};GetWindowRect(window_,&outer);
    const auto chrome=native_chrome(ImGui_ImplWin32_GetDpiScaleForHwnd(window_));
    RECT suggested{outer.left,outer.top,
        outer.left+static_cast<LONG>(std::ceil(options_.window_width*next+chrome.x)),
        outer.top+static_cast<LONG>(std::ceil(options_.window_height*next+chrome.y))};
    const auto dpi=static_cast<WORD>(std::lround(next*96));
    // Exercise the same native message path as a monitor transition. This is
    // deterministic host/GPU validation, not an assertion that physical monitors
    // were moved or their system DPI was changed during this test.
    SendMessageW(window_,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
  }
  bool top_menu(const char *id, const char *label) {
    ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
    const std::string button = std::string(label) + "###" + id;
    const bool clicked = ImGui::Button(button.c_str(), {0, ui_pixels(30)});
    if (options_.scene_first_smoke && std::string(id) == "File") {
      const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
      file_menu_center_ = {(min.x + max.x) / 2, (min.y + max.y) / 2};
      file_menu_popup_ = ImGui::GetID(id);
    }
    ImGui::PopStyleColor();
    if (clicked) {
      ui::anchor_popup_to_item();
      ImGui::OpenPopup(id);
    }
    return ui::begin_popup(id);
  }
  void menu_bar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ui_pixels(16), ui_pixels(8)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ui_pixels(8), 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.110f, 0.141f, 0.176f, 1});
    ImGui::BeginChild("Application bar", {0, ui_pixels(ui::menu_height_dp)},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    const auto brand = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    ui::icon(ui::Icon::cube, {brand.x, brand.y + ui_pixels(7)}, ui_pixels(16), ImGui::GetColorU32(ImGuiCol_Text));
    draw->AddText({brand.x + ui_pixels(26), brand.y + ui_pixels(6)},
                   ImGui::GetColorU32(ImGuiCol_Text), "PVD");
    draw->AddText(ImGui::GetFont(), ui_pixels(11),
        {brand.x + ui_pixels(63), brand.y + ui_pixels(8)},
        ImGui::GetColorU32(ImGuiCol_TextDisabled), "S T U D I O");
    ImGui::Dummy({ui_pixels(119), ui_pixels(30)});
    ImGui::SameLine();
    if (top_menu("File", "File")) {
      if (ImGui::MenuItem("Open capture...", "Ctrl+O")) choose_file();
#ifdef PVD_VIEWER_HAS_TRANSPORT
      if (ImGui::MenuItem("Live capture...")) capture_setup_requested_ = true;
#endif
      ImGui::Separator();
      if (ImGui::MenuItem("Save / crop...", "Ctrl+S", false, document().loaded())) {
        export_popup_requested_ = true;
        export_popup_slot_ = active_slot().id();
      }
#ifdef PVD_VIEWER_HAS_REPX
      if (ImGui::MenuItem("Export RepX...", nullptr, false, document().loaded())) {
        repx_popup_requested_ = true;
        repx_popup_slot_ = active_slot().id();
      }
#endif
      ui::end_popup();
    }
    ImGui::SameLine();
    if (top_menu("Workspace", "Workspace")) {
      if (ImGui::MenuItem("Manage tabs and bookmarks")) ui().page = 6;
      if (ImGui::MenuItem("New empty tab"))
        try { activate_slot(workspace_.create_empty()); }
        catch (const std::exception &e) { error_ = e.what(); }
      ImGui::BeginDisabled(active_slot().lock_reason().has_value());
      if (ImGui::MenuItem("Replace active source...")) choose_file(active_slot().id());
      if (ImGui::MenuItem("Close active tab")) close_slot(active_slot().id());
      ImGui::EndDisabled();
      ImGui::Separator();
      if (ImGui::MenuItem("Save workspace...")) save_workspace();
      if (ImGui::MenuItem("Load workspace...", nullptr, false, !workspace_.any_locked())) load_workspace();
      if (ImGui::MenuItem("Reset layout")) reset_layout();
      ui::end_popup();
    }
    ImGui::SameLine();
    if (top_menu("Help", "Help")) {
      if (ImGui::MenuItem("Help / shortcuts", "F1")) help_requested_ = true;
      ui::end_popup();
    }
    const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    draw->AddLine({pos.x, pos.y + size.y - 1}, {pos.x + size.x, pos.y + size.y - 1},
                  ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
  }
  bool mode_menu(const char *id, ui::Icon icon, const char *label, bool selected) {
    const auto text = std::string(label) + "    ";
    bool clicked = false;
    if (icon == ui::Icon::more) {
      const float width = ImGui::CalcTextSize(label).x + ui_pixels(39);
      ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4{0,0,0,0});
      ImGui::PushStyleColor(ImGuiCol_Text, {0,0,0,0});
      clicked = ImGui::Button(id, {width, ui_pixels(30)});
      ImGui::PopStyleColor(2);
      const auto min = ImGui::GetItemRectMin();
      ImGui::GetWindowDrawList()->AddText({min.x + ui_pixels(9), min.y + (ui_pixels(30) - ImGui::GetFontSize()) / 2},
          ImGui::GetColorU32(selected ? ImGuiCol_CheckMark : ImGuiCol_Text), label);
    } else clicked = ui::labeled_button(id, icon, text.c_str(), selected);
    if (clicked) {
      ui::anchor_popup_to_item();
      ImGui::OpenPopup(id);
    }
    if (options_.scene_first_smoke && icon == ui::Icon::more) {
      const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
      more_menu_center_ = {(min.x + max.x) / 2, (min.y + max.y) / 2};
      more_menu_popup_ = ImGui::GetID(id);
    }
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ui::icon(ui::Icon::chevron_down, {max.x - ui_pixels(21), min.y + ui_pixels(8)},
              ui_pixels(14), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    return ui::begin_popup(id);
  }
  void workspace_modes() {
    if (ui::labeled_button("Scene mode", ui::Icon::cube, "Scene", ui().page == 0)) ui().page = 0;
    ImGui::SameLine(0, ui_pixels(4));
#ifdef PVD_VIEWER_HAS_ANALYSIS
    if (mode_menu("Analysis", ui::Icon::chart, "Analysis", ui().page >= 1 && ui().page <= 7 && ui().page != 6)) {
      const auto page = [&](const char *label, int value) {
        if (ImGui::MenuItem(label, nullptr, ui().page == value)) ui().page = value;
      };
      ImGui::TextDisabled("Performance and diagnostics");
      page("Profiler", 1); page("Memory", 2); page("Errors", 3);
      ImGui::Separator();
      ImGui::TextDisabled("Changes over time");
      page("Property curves", 4); page("Event curves", 7); page("Frame diff", 5);
      ui::end_popup();
    }
    ImGui::SameLine(0, ui_pixels(4));
#endif
    if (mode_menu("More", ui::Icon::more, "More", ui().page >= 8 || ui().page == 6)) {
      if (ImGui::MenuItem("Deformables", nullptr, ui().page == 8)) ui().page = 8;
      if (ImGui::MenuItem("Scene queries", nullptr, ui().page == 9)) ui().page = 9;
      if (ImGui::MenuItem("Named clips", nullptr, ui().page == 10)) ui().page = 10;
      ImGui::Separator();
      if (ImGui::BeginMenu("Recorded scene", !document().scenes().empty())) {
        for (const auto &scene : document().scenes()) {
          ImGui::PushID(std::to_string(scene.key.id).c_str());
          const auto name = scene.name.empty() ? "Scene " + std::to_string(scene.key.id) : scene.name;
          if (ImGui::MenuItem(name.c_str(), nullptr, document().scene_id() == scene.key.id))
            try { document().set_scene(scene.key.id); }
            catch (const std::exception &e) { error_ = e.what(); }
          ImGui::PopID();
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Workspace", nullptr, ui().page == 6)) ui().page = 6;
      ui::end_popup();
    }
  }
  void session_bar() {
    ImGui::SetCursorPosY(ui_pixels(ui::menu_height_dp));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ui_pixels(16), 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
    ImGui::BeginChild("Session bar", {0, ui_pixels(ui::documents_height_dp)},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    float modes_width = ui_pixels(41 + 39 + 4) + ImGui::CalcTextSize("Scene").x + ImGui::CalcTextSize("More").x;
#ifdef PVD_VIEWER_HAS_ANALYSIS
    modes_width += ui_pixels(41 + 4) + ImGui::CalcTextSize("Analysis    ").x;
#endif
    const auto width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("Recording tabs", {std::max(ui_pixels(120), width - modes_width), ui_pixels(42)},
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    draw_workspace_tabs(workspace_, workspace_actions());
    ImGui::EndChild();
    ImGui::SetCursorPos({ImGui::GetWindowWidth() - modes_width - ui_pixels(16), ui_pixels(6)});
    workspace_modes();
    const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine({pos.x, pos.y + size.y - 1}, {pos.x + size.x, pos.y + size.y - 1},
                                      ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
  }
  void draw() {
    const UiPixelScope pixel_scope(ui_scale_->current().screen_scale);
    layout_smoke_drawer_visible_ = false;
    try{shortcuts();}
    catch(const std::exception &e) {
      if(smoke_mode())throw;
      document().set_playing(false);error_=e.what();
    }
    const auto *main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(main->Pos);
    ImGui::SetNextWindowSize(main->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("PVD Studio##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    menu_bar();
#ifdef PVD_VIEWER_HAS_TRANSPORT
    capture_setup();
#endif
    help_popup();
    export_popup();
#ifdef PVD_VIEWER_HAS_REPX
    repx_popup();
#endif
    session_bar();
    // Task details no longer add rows above the scene while work is running.
    if (options_.scene_first_smoke) activity_popup_id_ = ImGui::GetID("Activity");
    if (activity_popup_requested_) {
      ImGui::OpenPopup("Activity");
      activity_popup_requested_ = false;
    }
    ImGui::SetNextWindowSizeConstraints({ui_pixels(480), 0},
        {std::max(ui_pixels(480), main->Size.x - ui_pixels(40)), main->Size.y - ui_pixels(100)});
    if (ui::begin_popup("Activity")) {
      ImGui::TextUnformatted("ACTIVITY");
      load_bar();
      if (export_status_visible_) {
        draw_export_progress(export_job_, true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Export details")) {
          export_popup_slot_ = last_export_slot_id_;
          export_popup_requested_ = true;
        }
        if (!export_job_.running()) {
          ImGui::SameLine();
          if (ImGui::SmallButton("Dismiss export"))
            export_status_visible_ = false;
        }
        ImGui::Separator();
      }
#ifdef PVD_VIEWER_HAS_REPX
      if (repx_status_visible_) {
        draw_repx_progress(repx_job_, true);
        const auto repx_status = repx_job_.status();
        if (repx_status.stage == RepxStage::completed && !repx_status.complete) {
          ImGui::SameLine();
          ImGui::TextColored({1, .7f, .35f, 1},
                             "Partial collection / %zu diagnostics",
                             repx_status.diagnostics.size());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("RepX details"))
          repx_popup_requested_ = true;
        if (!repx_job_.running()) {
          ImGui::SameLine();
          if (ImGui::SmallButton("Dismiss RepX"))
            repx_status_visible_ = false;
        }
        ImGui::Separator();
      }
#endif
#ifdef PVD_VIEWER_HAS_TRANSPORT
      capture_bar();
#endif
      if (!error_.empty()) {
        ImGui::TextColored({1, .57f, .42f, 1}, "Error");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", error_.c_str());
        if (ImGui::SmallButton("Dismiss error"))
          error_.clear();
      }
      if (!notice_.empty()) {
        ImGui::TextWrapped("%s", notice_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss notice"))
          notice_.clear();
      }
      ui::end_popup();
    }
    const auto scope = std::to_string(active_slot().id());
    ImGui::PushID(scope.c_str());
    const float body_top = ui_pixels(ui::menu_height_dp + ui::documents_height_dp);
    ImGui::SetCursorPosY(body_top);
    const float timeline_height = ui_pixels(96), status_height = ui_pixels(28);
    const float body_height =
        std::max(ui_pixels(100), ImGui::GetContentRegionAvail().y - timeline_height -
                            status_height);
    const auto table_id =
        "workspace-layout-" + std::to_string(layout_generation_) + "-" +
        std::to_string(ui_scale_->current().screen_scale);
    if (ui().page == 0) {
      ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.063f, 0.094f, 0.125f, 1});
      ImGui::BeginChild("Scene", {0, body_height}, ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse);
      viewport();
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    if (ui().page == 8 || ui().page == 9) {
      const auto native_table=(ui().page==8?"Deformable layout-":"Query layout-")+table_id;
      if (ImGui::BeginTable(native_table.c_str(), 2,
          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Native objects", ImGuiTableColumnFlags_WidthFixed, ui_pixels(390));
        ImGui::TableSetupColumn("Native viewport", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("Deformable workspace", {0, body_height}, ImGuiChildFlags_Borders);
        ui().deformables.viewport_aspect = viewport_size_.x / std::max(1.f, viewport_size_.y);
        if (ui().page == 8)
          drawDeformables(document(), camera(), ui().render_options, ui().deformables);
        else {
          ui().scene_queries.viewport_aspect = ui().deformables.viewport_aspect;
          drawSceneQueries(document(), camera(), ui().render_options, ui().scene_queries);
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("Native scene", {0, body_height}, ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        viewport();
        ImGui::EndChild();
        ImGui::EndTable();
      }
    } else if (ui().page == 10) {
      ImGui::BeginChild("Named clip workspace", {0,body_height},ImGuiChildFlags_Borders);
      draw_clips();
      ImGui::EndChild();
    } else if (ui().page == 6) {
      ImGui::BeginChild("Workspace manager", {0, body_height},
                        ImGuiChildFlags_Borders);
      draw_workspace_panel(workspace_, workspace_panel_, workspace_actions());
      ImGui::EndChild();
    }
#ifdef PVD_VIEWER_HAS_ANALYSIS
    else if (ui().page > 0) {
      ImGui::BeginChild("Analysis workspace", {0, body_height},
                        ImGuiChildFlags_Borders);
      const CsvExport save = [&](const char *name, const auto &writer) {
        export_csv(name, writer);
      };
      if (ui().page == 7)
        ui().profile_curves.draw(document(), save);
      else if (ui().page == 4 || ui().page == 5)
        ui().curves.draw(document(),
                         ui().page == 5 ? CurvePage::diff : CurvePage::curves,
                         save);
      else
        ui().analysis.draw(document(), static_cast<AnalysisPage>(ui().page - 1),
                           save);
      ImGui::EndChild();
    }
#endif
    ImGui::SetCursorPosY(body_top + body_height);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ui_pixels(16), ui_pixels(8)});
    ImGui::BeginChild("Timeline", {0, timeline_height}, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    const auto timeline_pos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(timeline_pos,
        {timeline_pos.x + ImGui::GetWindowWidth(), timeline_pos.y}, ImGui::GetColorU32(ImGuiCol_Border));
    drawTimeline(document(), ui().panels);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    store_preferences(active_slot().id());
    ImGui::PopID();
    const auto frame = document().frame();
    const auto stats = renderer_->stats();
    ImGui::SetCursorPosY(body_top + body_height + timeline_height);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ui_pixels(16), ui_pixels(5)});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ui_pixels(5), ui_pixels(2)});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
    ImGui::BeginChild("Status", {0, status_height}, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    std::string activity = "Ready";
    if (load_job_.running()) activity = load_stage_name(load_job_.status().stage);
    else if (bind_slot_) activity = "Associating capture";
    else if (export_job_.running()) activity = "Exporting capture";
#ifdef PVD_VIEWER_HAS_REPX
    else if (repx_job_.running()) activity = "Exporting RepX";
#endif
#ifdef PVD_VIEWER_HAS_TRANSPORT
    else if (live_.status().state != pvd::CaptureState::idle)
      activity = std::string("Live: ") + pvd::capture_state_name(live_.status().state);
#endif
    bool failed = !error_.empty();
    if (load_status_visible_ && load_job_.status().stage == LoadStage::failed) {
      activity = "Load failed - details";
      failed = true;
    }
    if (export_status_visible_ && export_job_.status().stage == ExportStage::failed) {
      activity = "Export failed - details";
      failed = true;
    }
#ifdef PVD_VIEWER_HAS_TRANSPORT
    if (live_.status().state == pvd::CaptureState::failed ||
        live_.status().state == pvd::CaptureState::truncated)
      failed = true;
#endif
#ifdef PVD_VIEWER_HAS_REPX
    if (repx_status_visible_) {
      const auto status = repx_job_.status();
      if (status.stage == RepxStage::failed) {
        activity = "RepX failed - details";
        failed = true;
      } else if (status.stage == RepxStage::completed && !status.complete) {
        activity = "Partial RepX - diagnostics";
        failed = true;
      } else if (!repx_job_.running()) activity = "RepX / Activity";
    }
#endif
    if (!error_.empty()) activity = "Error - details";
    else if (!notice_.empty()) activity = "Notice - details";
    else if (load_status_visible_ || export_status_visible_)
      activity += " / Activity";
    const auto status_start = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        {status_start.x + ui_pixels(3), status_start.y + ui_pixels(7)}, ui_pixels(3),
        ImGui::GetColorU32(failed ? ImVec4{1, .57f, .42f, 1} : ImVec4{.34f, .82f, .73f, 1}));
    ImGui::Dummy({ui_pixels(7), ImGui::GetTextLineHeight()});
    ImGui::SameLine(0, ui_pixels(5));
    if (failed) ImGui::PushStyleColor(ImGuiCol_Text, {1, .57f, .42f, 1});
    if (ImGui::SmallButton((activity + "###activity").c_str()))
      activity_popup_requested_ = true;
    if (options_.scene_first_smoke) {
      const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
      activity_button_center_ = {(min.x + max.x) / 2, (min.y + max.y) / 2};
    }
    if (failed) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tasks, live capture, notifications and errors");
    ImGui::SameLine();
    if (document().loaded() && frame) {
      ImGui::TextDisabled("%zu shapes    %zu frames", frame->draws.size(), document().timeline().size());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%llu triangles / %zu GPU meshes",
          static_cast<unsigned long long>(stats.triangles), stats.cached_meshes);
    } else
      ImGui::TextDisabled("Ready / Open or drop a PhysX raw capture");
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + ui_pixels(16),
                           ImGui::GetWindowWidth() - ui_pixels(160)));
    ImGui::TextDisabled("D3D11 %s", using_warp_ ? "WARP" : "GPU");
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::End();
  }
  bool sidebar_button(const char *label, SlotUiState::Sidebar sidebar) {
    const bool selected = ui().sidebar == sidebar;
    const bool clicked = ui::icon_button(label,
        sidebar == SlotUiState::Sidebar::objects ? ui::Icon::panel_left : ui::Icon::panel_right,
        sidebar == SlotUiState::Sidebar::objects ? "Scene objects" : "Object properties", selected);
    if (options_.scene_first_smoke) {
      const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
      const ImVec2 center{(min.x + max.x) / 2, (min.y + max.y) / 2};
      if (sidebar == SlotUiState::Sidebar::objects) objects_button_center_ = center;
      if (sidebar == SlotUiState::Sidebar::properties) properties_button_center_ = center;
    }
    if (clicked) ui().sidebar = selected ? SlotUiState::Sidebar::none : sidebar;
    return clicked;
  }
  bool scene_drawer_contains(ImVec2 point) const {
    return point.x >= scene_drawer_min_.x && point.y >= scene_drawer_min_.y &&
           point.x < scene_drawer_max_.x && point.y < scene_drawer_max_.y;
  }
  void position_scene_drawer() {
    const bool objects = ui().sidebar == SlotUiState::Sidebar::objects;
    const float preferred_width = objects
        ? std::clamp(viewport_size_.x * .55f, ui_pixels(120), ui_pixels(384))
        : ui_pixels(280);
    const float width = std::min(preferred_width,
                                 std::max(ui_pixels(120), viewport_size_.x - ui_pixels(24)));
    scene_drawer_min_ = {viewport_origin_.x + ui_pixels(12), viewport_origin_.y + ui_pixels(8)};
    if (!objects)
      scene_drawer_min_.x = viewport_origin_.x + viewport_size_.x - width - ui_pixels(12);
    scene_drawer_max_ = {scene_drawer_min_.x + width,
                         viewport_origin_.y + viewport_size_.y - ui_pixels(12)};
  }
  void scene_drawer() {
    if (ui().page != 0 || ui().sidebar == SlotUiState::Sidebar::none)
      return;
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(scene_drawer_min_);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.090f, 0.118f, 0.149f, 1});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_pixels(5));
    const ImVec2 size{scene_drawer_max_.x - scene_drawer_min_.x,
                      scene_drawer_max_.y - scene_drawer_min_.y};
    const bool objects = ui().sidebar == SlotUiState::Sidebar::objects;
    const bool diagnostics = ui().sidebar == SlotUiState::Sidebar::diagnostics;
    if (ImGui::BeginChild(objects ? "Objects drawer" : "Properties drawer", size,
                          ImGuiChildFlags_Borders)) {
      ImGui::TextUnformatted(objects ? "Scene objects" : diagnostics ? "Diagnostics" : "Object properties");
      ImGui::SameLine(std::max(ImGui::GetCursorPosX(), size.x - ui_pixels(54)));
      const bool close = ui::icon_button("X##close-drawer", ui::Icon::close, "Close / Escape", false, 24);
      if (options_.scene_first_smoke) {
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        drawer_close_center_ = {(min.x + max.x) / 2, (min.y + max.y) / 2};
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close drawer / Escape");
      ImGui::Separator();
      if (ImGui::BeginChild("Drawer content", {0, 0})) {
        if (options_.scene_first_smoke && !objects && !diagnostics)
          inspector_test_window_ = ImGui::GetCurrentWindow();
        if (objects) {
          drawHierarchy(document(), ui().panels, [this](HierarchyCameraTarget target) {
            const double aspect = std::max(1.f, viewport_size_.x) /
                                  std::max(1.f, viewport_size_.y);
            auto fitted_camera = camera();
            const bool focused = target == HierarchyCameraTarget::scene
                ? document().fit_scene(fitted_camera, aspect)
                : document().fit_selected(fitted_camera, aspect);
            if (!focused)
              return false;
            point_camera_at(camera(), fitted_camera.target);
            stopTracking(ui().scene_tools);
            set_fitted(true);
            return true;
          });
        } else if (diagnostics) drawDiagnostics(document(), ui().panels);
        else drawInspector(document(), ui().panels);
        if (options_.scene_first_smoke)
          layout_smoke_drawer_visible_ = ImGui::GetWindowDrawList()->VtxBuffer.Size > 0;
      }
      ImGui::EndChild();
      if (close) ui().sidebar = SlotUiState::Sidebar::none;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(cursor);
  }
  void viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ui_pixels(10), ui_pixels(7)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ui_pixels(4), 0});
    ImGui::BeginChild("Viewport toolbar", {0, ui_pixels(ui::viewport_bar_height_dp)},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    if (ui().page == 0) {
      sidebar_button("Objects", SlotUiState::Sidebar::objects);
      ImGui::SameLine();
    }
    {
      std::string scene_name = "Select scene";
      const auto label = [](const SceneInfo &scene) {
        return scene.name.empty() ? "Scene " + std::to_string(scene.key.id) : scene.name;
      };
      const auto &scenes = document().scenes();
      for (std::size_t i = 0; i < scenes.size(); ++i)
        if (document().scene_id() == scenes[i].key.id)
          scene_name = "Scene " + std::to_string(i) + " / Perspective";
      ImGui::SetNextItemWidth(ui_pixels(ui().page == 0 ? 190.f : 150.f));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, {0, 0, 0, 0});
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ui_pixels(4), ui_pixels(7)});
      if (ui::begin_combo("##viewport-scene", scene_name.c_str(), ImGuiComboFlags_NoArrowButton)) {
        for (const auto &scene : document().scenes()) {
          ImGui::PushID(std::to_string(scene.key.id).c_str());
          if (ImGui::Selectable(label(scene).c_str(), document().scene_id() == scene.key.id))
            try { document().set_scene(scene.key.id); }
            catch (const std::exception &e) { error_ = e.what(); }
          ImGui::PopID();
        }
        ui::end_combo();
      }
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recorded scene / %s", scene_name.c_str());
      const float tools_width = ui_pixels(82 + 4 + 10) +
          ImGui::CalcTextSize("Fit").x + ImGui::CalcTextSize("Display").x +
          (ui().page == 0 ? ui_pixels(34) : 0.f);
      ImGui::SameLine(std::max(ImGui::GetCursorPosX() + ui_pixels(16), ImGui::GetWindowWidth() - tools_width));
    }
    if (ui::labeled_button("Fit  F", ui::Icon::focus, "Fit")) {
      stopTracking(ui().scene_tools);
      set_fitted(document().fit_scene(
          camera(), viewport_size_.x / std::max(1.f, viewport_size_.y)));
    }
    ImGui::SameLine();
    if (ui::labeled_button("Display", ui::Icon::sliders, "Display")) {
      ui::anchor_popup_to_item();
      ImGui::OpenPopup("Scene display layers");
    }
    if (options_.scene_first_smoke) {
      const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
      display_menu_center_ = {(min.x + max.x) / 2, (min.y + max.y) / 2};
      display_menu_popup_ = ImGui::GetID("Scene display layers");
      view_settings_popup_ = ImGui::GetID("Scene view tools");
    }
    if (ui::begin_popup("Scene display layers")) {
      ImGui::TextDisabled("COMMON LAYERS");
      ImGui::Checkbox("Grid", &ui().render_options.show_grid);
      ImGui::Checkbox("Wireframe", &ui().render_options.show_wireframe);
      ImGui::Checkbox("Contacts", &ui().render_options.show_contacts);
      ImGui::Separator();
      if (ImGui::MenuItem("Display and camera settings...", "Ctrl+Shift+S"))
        ui().view_popup_requested = true;
      if (ImGui::MenuItem("Diagnostics")) {
        ui().page = 0;
        ui().sidebar = SlotUiState::Sidebar::diagnostics;
      }
      ui::end_popup();
    }
    if (ui().page == 0) {
      ImGui::SameLine();
      sidebar_button("Properties", SlotUiState::Sidebar::properties);
    }
    if(ui().view_popup_requested) {
      ImGui::OpenPopup("Scene view tools");ui().view_popup_requested=false;
    }
    const auto *settings_viewport = ImGui::GetMainViewport();
    const float settings_height = std::min(ui_pixels(540), settings_viewport->Size.y - ui_pixels(80));
    ImGui::SetNextWindowPos(
        {settings_viewport->WorkPos.x + settings_viewport->WorkSize.x - ui_pixels(16),
         std::min(ImGui::GetWindowPos().y + ui_pixels(ui::viewport_bar_height_dp + 8),
             settings_viewport->WorkPos.y + settings_viewport->WorkSize.y - settings_height - ui_pixels(16))},
        ImGuiCond_Appearing, {1, 0});
    ImGui::SetNextWindowSize({ui_pixels(600), settings_height});
    ImGui::SetNextWindowSizeConstraints(
        {ui_pixels(480), ui_pixels(240)},
        {ui_pixels(680), std::max(ui_pixels(250), ImGui::GetMainViewport()->Size.y - ui_pixels(80))});
    if (ui::begin_popup("Scene view tools", ImGuiWindowFlags_NoScrollbar)) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("View settings");
      ImGui::SameLine(ImGui::GetWindowWidth() - ui_pixels(143));
      if (ui::labeled_button("Restore built-in preferences", ui::Icon::settings, "Defaults")) {
        try{restore_default_preferences();}
        catch(const std::exception &e){ui().display.error=e.what();}
      }
      ImGui::TextDisabled("Source-specific views take priority over saved defaults.");
      ImGui::Separator();
      bool changed_page = false;
      const char *pages[] = {"Scene", "Camera", "Display", "Layers"};
      for (int page = 0; page != 4; ++page) {
        if (page) ImGui::SameLine(0, ui_pixels(6));
        const bool selected = ui().view_settings_page == page;
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4{0,0,0,0});
        if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        if (ImGui::Button(pages[page], {ui_pixels(100), ui_pixels(30)})) {
          ui().view_settings_page = page;
          changed_page = true;
        }
        if (selected) ImGui::PopStyleColor();
        ImGui::PopStyleColor();
      }
      ImGui::Separator();
      if (ImGui::BeginChild("Settings content", {0, 0}, ImGuiChildFlags_None,
                             ImGuiWindowFlags_NoBackground)) {
        if (changed_page) ImGui::SetScrollY(0);
        if (ui().view_settings_page == 0) {
        ui().scene_tools.viewport_aspect =
            viewport_size_.x / std::max(1.f, viewport_size_.y);
        drawSceneTools(document(), camera(), ui().scene_tools);
        if(ui().scene_tools.recorded_follow.enabled)set_fitted(true);
        }
        if (ui().view_settings_page == 1) {
        if (drawCameraControls(document(),camera(),ui().navigation,ui().camera_panel))
          set_fitted(true);
        }
        if (ui().view_settings_page == 2) {
        ui().display.viewport_aspect = viewport_size_.x / std::max(1.f, viewport_size_.y);
        drawDisplaySettings(document(), camera(), ui().render_options,
                            renderer_->stats(), ui().display);
        }
        if (ui().view_settings_page == 3) {
        ImGui::TextDisabled("Render layers");
        auto &options = ui().render_options;
        ImGui::Checkbox("Grid", &options.show_grid);
        ImGui::SameLine();
        ImGui::Checkbox("Axes", &options.show_axes);
        ImGui::SameLine();
        ImGui::Checkbox("Solid shapes", &options.show_solids);
        ImGui::Checkbox("Wireframe", &options.show_wireframe);
        ImGui::SameLine();
        ImGui::Checkbox("Contacts", &options.show_contacts);
        ImGui::Checkbox("Debug points", &options.show_debug_points);
        ImGui::SameLine();
        ImGui::Checkbox("Debug lines", &options.show_debug_lines);
        ImGui::Checkbox("Debug triangles", &options.show_debug_triangles);
        ImGui::SameLine();
        ImGui::Checkbox("Debug text", &ui().show_debug_text);
        const auto &overlays = renderer_->body_overlays();
        if (!overlays.complete) {
          ImGui::Separator();
          ImGui::TextColored({1,.7f,.35f,1}, "Body overlay diagnostics (%zu retained)",
                             overlays.diagnostics.size());
          for (const auto &diagnostic : overlays.diagnostics)
            ImGui::TextWrapped("Actor %llu:%llu / %s: %s",
                static_cast<unsigned long long>(diagnostic.source.key.actor.id),
                static_cast<unsigned long long>(diagnostic.source.key.actor.generation),
                diagnostic.property.c_str(), diagnostic.message.c_str());
        }
        const auto &frames=renderer_->coordinate_frames();
        if(!frames.complete) {
          ImGui::TextColored({1,.7f,.35f,1},"Coordinate frame diagnostics (%zu retained)",frames.diagnostics.size());
          for(const auto &diagnostic:frames.diagnostics)
            ImGui::TextWrapped("%s: %s",diagnostic.property.c_str(),diagnostic.message.c_str());
        }
        }
      }
      ImGui::EndChild();
      ui::end_popup();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPosY(ui_pixels(ui::viewport_bar_height_dp));
    const auto frame = document().frame();
    if (!document().loaded() || !frame) {
      viewport_origin_ = ImGui::GetCursorScreenPos();
      viewport_size_ = ImGui::GetContentRegionAvail();
      position_scene_drawer();
      ImGui::Dummy({0, ImGui::GetContentRegionAvail().y * 0.32f});
      drawEmptyState("Inspect your simulation",
                     "Open a PhysX 3.4 raw capture to explore geometry, frames "
                     "and object properties.");
      if (ImGui::Button("Open capture..."))
        choose_file();
      scene_drawer();
      return;
    }
    viewport_size_ = ImGui::GetContentRegionAvail();
    viewport_size_.y = std::max(1.0f, viewport_size_.y - ui_pixels(24));
    viewport_size_.x = std::max(1.0f, viewport_size_.x);
    ui().displayed_scene = document().scene_id();
    if (!fitted()) {
      // The first ImGui table/child layout can temporarily report a one-pixel
      // viewport. Fitting that aspect permanently sends the initial camera far
      // away. Wait for two useful, matching layouts before accepting auto-fit.
      const auto previous=ui().auto_fit_size;
      const bool useful=viewport_size_.x>=ui_pixels(64)&&viewport_size_.y>=ui_pixels(64);
      if(useful&&std::abs(previous.x-viewport_size_.x)<1&&std::abs(previous.y-viewport_size_.y)<1)
        ++ui().auto_fit_stable_frames;
      else ui().auto_fit_stable_frames=0;
      ui().auto_fit_size=viewport_size_;
      if(ui().auto_fit_stable_frames)
        set_fitted(document().fit_scene(camera(),viewport_size_.x/viewport_size_.y));
    }
    ui().scene_tools.viewport_aspect = viewport_size_.x / viewport_size_.y;
    updateTrackedCamera(document(), camera(), ui().scene_tools);
    renderer_->resize(static_cast<UINT>(viewport_size_.x),
                      static_cast<UINT>(viewport_size_.y));
    viewport_origin_ = ImGui::GetCursorScreenPos();
    const bool drawer_open = ui().page == 0 && ui().sidebar != SlotUiState::Sidebar::none;
    const auto &io = ImGui::GetIO();
    position_scene_drawer();
    // Exclude overlays before submitting the canvas item, including the first
    // frame a drawer opens. Otherwise a click/scroll could also reach the scene.
    const bool over_drawer = drawer_open && scene_drawer_contains(io.MousePos);
    const bool selection_chip = ui().page == 0 && document().selected_key().has_value();
    const auto selected_key = document().selected_key();
    std::string selection_name;
    if (selected_key)
      if (const auto *actor = frame->snapshot.instance(selected_key->actor.id);
          actor && actor->generation == selected_key->actor.generation) {
        for (const char *field : {"Name", "name"})
          if (const auto *value = actor->property(field))
            if (const auto name = value->as_string(); name && !name->empty()) {
              selection_name = *name;
              break;
            }
        if (selection_name.empty()) selection_name = actor->class_name.name;
      }
    if (selection_name.empty()) selection_name = "Actor";
    const float chip_limit = std::min(viewport_size_.x - ui_pixels(24), ui_pixels(300));
    if (ImGui::CalcTextSize(selection_name.c_str()).x > chip_limit - ui_pixels(100)) {
      const char *end = selection_name.c_str();
      ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::max(1.f, chip_limit - ui_pixels(120)),
          0, selection_name.c_str(), nullptr, &end);
      selection_name = std::string(selection_name.c_str(), end) + "...";
    }
    const std::string chip_label = selection_name + " / Selected";
    const float chip_width = std::min(chip_limit, ImGui::CalcTextSize(chip_label.c_str()).x + ui_pixels(30));
    const ImVec2 chip_min{viewport_origin_.x + ui_pixels(12),
                          viewport_origin_.y + viewport_size_.y - ui_pixels(48)};
    const bool over_chip = selection_chip && io.MousePos.x >= chip_min.x &&
        io.MousePos.x <= chip_min.x + chip_width && io.MousePos.y >= chip_min.y &&
        io.MousePos.y <= chip_min.y + ui_pixels(30);
    if (over_drawer || over_chip)
      ImGui::Dummy(viewport_size_);
    else
      ImGui::InvisibleButton("Scene interaction", viewport_size_,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered() && !over_drawer && !over_chip;
    try {
      if(navigateCameraFromImGui(camera(),ui().navigation,ui().camera_input,hovered)) {
        notifyManualCameraInput(ui().scene_tools);set_fitted(true);
      }
    }catch(const std::exception &e){ui().camera_panel.error=e.what();}
    if (hovered && io.MouseWheel != 0) {
      notifyManualCameraInput(ui().scene_tools);
      camera().zoom(io.MouseWheel);
    }
    if (ImGui::IsItemActive() && !over_drawer && !over_chip) {
      try {
        if(rotateCameraFromImGui(camera(),ui().navigation,true,ui_scale_->current().screen_scale)) {
          notifyManualCameraInput(ui().scene_tools);
          set_fitted(true);
        }
      }catch(const std::exception &e){ui().camera_panel.error=e.what();}
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        notifyManualCameraInput(ui().scene_tools);
        camera().pan(io.MouseDelta.x, io.MouseDelta.y, viewport_size_.y);
      }
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !io.KeyAlt && io.MouseDragMaxDistanceSqr[0] < ui_pixels(4)*ui_pixels(4)) {
      const auto hit =
          (ui().render_options.show_solids ||
           ui().render_options.show_wireframe)
              ? pick(visualization_pick_draws(*frame,ui().render_options,visualization_selection()),
                     camera().screen_ray(io.MousePos.x - viewport_origin_.x,
                                         io.MousePos.y - viewport_origin_.y,
                                         viewport_size_.x, viewport_size_.y))
              : std::optional<PickHit>{};
      DeformablePickOptions options;
      options.cloth = ui().render_options.show_cloth;
      options.particles = ui().render_options.show_particles;
      options.angular_radius =
          6 * 2 * std::tan(camera().vertical_fov / 2) / viewport_size_.y;
      const auto deformable = pick_deformable(
          *frame,
          camera().screen_ray(io.MousePos.x - viewport_origin_.x,
                              io.MousePos.y - viewport_origin_.y,
                              viewport_size_.x, viewport_size_.y),
          options);
      if (deformable && (!hit || deformable->distance < hit->distance))
        document().select_deformable(deformable->key);
      else if (hit)
        document().select(hit->key);
      else
        document().clear_selection();
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      stopTracking(ui().scene_tools);
      if (document().fit_selected(camera(),
                                  viewport_size_.x / viewport_size_.y))
        set_fitted(true);
    }
    renderer_->render(*document().frame(), camera(), ui().render_options,visualization_selection());
    ImGui::GetWindowDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(renderer_->shader_resource_view()),
        viewport_origin_,
        {viewport_origin_.x + viewport_size_.x,
         viewport_origin_.y + viewport_size_.y});
    // Text stays in the UI layer; the reusable GPU renderer has no font
    // dependency.
    auto *draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(viewport_origin_,
                            {viewport_origin_.x + viewport_size_.x,
                             viewport_origin_.y + viewport_size_.y},
                            true);
    const auto projection =
        camera().view_projection(viewport_size_.x / viewport_size_.y);
    const auto &m = projection.values;
    if (ui().show_debug_text)
      for (const auto &text : document().frame()->debug.texts) {
        const auto &p = text.position;
        const double w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (!(w > 0) || !std::isfinite(w))
          continue;
        const double x = (m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / w;
        const double y = (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / w;
        const double z = (m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]) / w;
        if (!(std::abs(x) <= 1 && std::abs(y) <= 1 && z >= 0 && z <= 1))
          continue;
        const ImVec2 pos{
            viewport_origin_.x + float((x + 1) * 0.5) * viewport_size_.x,
            viewport_origin_.y + float((1 - y) * 0.5) * viewport_size_.y};
        const auto color =
            IM_COL32((text.color >> 16) & 255, (text.color >> 8) & 255,
                     text.color & 255, (text.color >> 24) & 255);
        const float size =
            ImGui::GetFontSize() * (std::isfinite(text.size)
                                        ? std::clamp(text.size, 0.6f, 2.0f)
                                        : 1.0f);
        draw_list->AddText(ImGui::GetFont(), size, pos, color, text.text.data(),
                           text.text.data() +
                               std::min<std::size_t>(text.text.size(), 4096));
      }
    draw_list->PopClipRect();
    // Camera-relative orientation stays useful even when the world axes are
    // outside the view; labels retain the recorded world-coordinate meaning.
    const auto view = camera().view_matrix();
    const ImVec2 axes_origin{viewport_origin_.x + viewport_size_.x - ui_pixels(40),
                             viewport_origin_.y + viewport_size_.y - ui_pixels(38)};
    for (int axis = 0; axis != 3; ++axis) {
      const float mirror = camera().handedness == DisplayHandedness::left_handed ? -1.f : 1.f;
      const ImVec2 tip{axes_origin.x + mirror * view.values[axis * 4] * ui_pixels(25),
                       axes_origin.y - view.values[axis * 4 + 1] * ui_pixels(25)};
      const auto color = ImGui::GetColorU32(axis == 1 ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
      draw_list->AddLine(axes_origin, tip, color, ui_pixels(1));
      const char *axis_name = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
      draw_list->AddText(ImGui::GetFont(), ui_pixels(11),
          {tip.x + ui_pixels(3), tip.y - ui_pixels(6)}, color, axis_name);
    }
    if (selection_chip && document().selected_key()) {
      const auto cursor = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(chip_min);
      ImGui::BeginDisabled(over_drawer);
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
      ImGui::PushStyleColor(ImGuiCol_Text, {0, 0, 0, 0});
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, ui_pixels(1));
      if (ImGui::Button("Inspect selection", {chip_width, ui_pixels(28)})) ui().sidebar = SlotUiState::Sidebar::properties;
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);
      draw_list->AddCircleFilled({chip_min.x + ui_pixels(10), chip_min.y + ui_pixels(14)},
                                ui_pixels(3), ImGui::GetColorU32(ImGuiCol_CheckMark));
      draw_list->AddText({chip_min.x + ui_pixels(20), chip_min.y + ui_pixels(7)},
                         ImGui::GetColorU32(ImGuiCol_Text), chip_label.c_str());
      if (options_.scene_first_smoke) {
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        selection_chip_center_ = {(min.x + max.x) / 2, (min.y + max.y) / 2};
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open properties for the current selection");
      ImGui::SetCursorScreenPos(cursor);
    }
    const auto hint = ImGui::GetCursorScreenPos();
    draw_list->AddText(ImGui::GetFont(), ui_pixels(12), {hint.x + ui_pixels(14), hint.y + ui_pixels(5)},
        ImGui::GetColorU32(ImGuiCol_TextDisabled), "RMB look / MMB pan / Wheel zoom");
    const float projection_width = ImGui::CalcTextSize("Perspective").x * 12.f / ui::font_size_dp;
    draw_list->AddText(ImGui::GetFont(), ui_pixels(12),
        {viewport_origin_.x + viewport_size_.x - projection_width - ui_pixels(14), hint.y + ui_pixels(5)},
        ImGui::GetColorU32(ImGuiCol_TextDisabled), "Perspective");
    ImGui::Dummy({0, ui_pixels(24)});
    if (!renderer_->body_overlays().complete)
      ImGui::GetWindowDrawList()->AddText(
          {viewport_origin_.x+8,viewport_origin_.y+8}, IM_COL32(255,185,85,255),
          "Some body overlays unavailable; Display shows diagnostics.");
    scene_drawer();
  }
  void screenshot() {
    ComPtr<ID3D11Texture2D> buffer;
    check(swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer)),
          "Capture backbuffer");
    if (!options_.screenshot.parent_path().empty())
      std::filesystem::create_directories(options_.screenshot.parent_path());
    saveTexturePng(device_.Get(), context_.Get(), buffer.Get(),
                   options_.screenshot);
  }
  void layout_smoke_input() {
    // Submit real ImGui pointer events after the native backend has polled the
    // desktop. Coordinates come from the widgets submitted on the last frame.
    auto &io = ImGui::GetIO();
    io.ConfigInputTrickleEventQueue = false;
    const auto click = [&](ImVec2 center, bool down) {
      io.AddMousePosEvent(center.x, center.y);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    };
    switch (host_frames_) {
    case 6: click(objects_button_center_, true); break;
    case 7: click(objects_button_center_, false); break;
    case 8:
      io.AddMousePosEvent((scene_drawer_min_.x + scene_drawer_max_.x) / 2,
                          (scene_drawer_min_.y + scene_drawer_max_.y) / 2);
      io.AddMouseWheelEvent(0, 1);
      break;
    case 9: click(properties_button_center_, true); break;
    case 10: click(properties_button_center_, false); break;
    case 11: click(drawer_close_center_, true); break;
    case 12: click(drawer_close_center_, false); break;
    case 13: click(objects_button_center_, true); break;
    case 14: click(objects_button_center_, false); break;
    case 15: io.AddKeyEvent(ImGuiKey_Escape, true); break;
    case 16: io.AddKeyEvent(ImGuiKey_Escape, false); break;
    case 17: click(selection_chip_center_, true); break;
    case 18: click(selection_chip_center_, false); break;
    case 19: click(drawer_close_center_, true); break;
    case 20: click(drawer_close_center_, false); break;
    case 21: click({viewport_origin_.x + viewport_size_.x - 4, viewport_origin_.y + 4}, true); break;
    case 22: click({viewport_origin_.x + viewport_size_.x - 4, viewport_origin_.y + 4}, false); break;
    case 24: click(file_menu_center_, true); break;
    case 25: click(file_menu_center_, false); break;
    case 27: io.AddKeyEvent(ImGuiKey_Escape, true); break;
    case 28: io.AddKeyEvent(ImGuiKey_Escape, false); break;
    case 29: click(more_menu_center_, true); break;
    case 30: click(more_menu_center_, false); break;
    case 32: io.AddKeyEvent(ImGuiKey_Escape, true); break;
    case 33: io.AddKeyEvent(ImGuiKey_Escape, false); break;
    case 34: click(activity_button_center_, true); break;
    case 35: click(activity_button_center_, false); break;
    case 37: io.AddKeyEvent(ImGuiKey_Escape, true); break;
    case 38: io.AddKeyEvent(ImGuiKey_Escape, false); break;
    case 39:
      io.AddMousePosEvent(viewport_origin_.x + viewport_size_.x - 4, viewport_origin_.y + 4);
      break;
    case 40: click(display_menu_center_, true); break;
    case 41: click(display_menu_center_, false); break;
    case 46: io.AddKeyEvent(ImGuiKey_Escape, true); break;
    case 47: io.AddKeyEvent(ImGuiKey_Escape, false); break;
    case 48: click(properties_button_center_, true); break;
    case 49: click(properties_button_center_, false); break;
    case 53: io.AddKeyEvent(ImGuiKey_Escape, true); break;
    case 54: io.AddKeyEvent(ImGuiKey_Escape, false); break;
    case 55: click(drawer_close_center_, true); break;
    case 56: click(drawer_close_center_, false); break;
    case 59:
      io.AddMousePosEvent(viewport_origin_.x + viewport_size_.x - 4, viewport_origin_.y + 4);
      break;
    }
  }
  void layout_smoke_step() {
    if (viewport_size_.x != layout_smoke_viewport_.x ||
        viewport_size_.y != layout_smoke_viewport_.y)
      throw std::runtime_error("Scene drawer resized the GPU viewport");
    if (camera().distance != layout_smoke_camera_.distance ||
        camera().yaw != layout_smoke_camera_.yaw || camera().pitch != layout_smoke_camera_.pitch)
      throw std::runtime_error("Drawer input leaked into scene navigation");
    if ((host_frames_ <= 21 || host_frames_ >= 24) && !document().selected_key()) {
      if (!options_.screenshot.empty()) screenshot();
      throw std::runtime_error("Popup / drawer interaction cleared the scene selection at frame " + std::to_string(host_frames_));
    }
    const auto sidebar = ui().sidebar;
    if ((host_frames_ == 9 || host_frames_ == 15) &&
        (sidebar != SlotUiState::Sidebar::objects || !layout_smoke_drawer_visible_)) {
      if (!options_.screenshot.empty()) screenshot();
      throw std::runtime_error("Objects toolbar button did not open a usable drawer: frame " +
          std::to_string(host_frames_) + " sidebar " + std::to_string(static_cast<int>(sidebar)) +
          " visible " + std::to_string(layout_smoke_drawer_visible_) +
          " pointer " + std::to_string(ImGui::GetIO().MousePos.x) + "," + std::to_string(ImGui::GetIO().MousePos.y));
    }
    if ((host_frames_ == 11 || host_frames_ == 19 || host_frames_ == 55) && sidebar != SlotUiState::Sidebar::properties) {
      if (!options_.screenshot.empty()) screenshot();
      throw std::runtime_error("Properties / Inspect did not open the drawer: frame " +
          std::to_string(host_frames_) + " sidebar " + std::to_string(static_cast<int>(sidebar)));
    }
    if ((host_frames_ == 13 || host_frames_ == 17) && sidebar != SlotUiState::Sidebar::none)
      throw std::runtime_error("Close / Escape did not dismiss the scene drawer");
    if ((host_frames_ == 9 || host_frames_ == 12) && !options_.screenshot.empty()) {
      const auto original = options_.screenshot;
      options_.screenshot = original.parent_path() / (original.stem().wstring() +
          (host_frames_ == 9 ? L"-objects" : L"-properties") + original.extension().wstring());
      screenshot();
      options_.screenshot = original;
    }
    if (host_frames_ == 23) {
      if (document().selected_key())
        throw std::runtime_error("Clicking empty scene space did not clear selection");
      document().select(*layout_smoke_selection_);
    }
    if (host_frames_ == 26 || host_frames_ == 31 || host_frames_ == 37) {
      const auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
      const ImGuiID expected = host_frames_ == 26 ? file_menu_popup_ :
          host_frames_ == 31 ? more_menu_popup_ : activity_popup_id_;
      if (popups.empty() || popups.back().PopupId != expected || !popups.back().Window)
        throw std::runtime_error("Shell menu / Activity did not open in its owning window");
    }
    const auto popup_image = [&](const wchar_t *suffix) {
      if (options_.screenshot.empty()) return;
      const auto original = options_.screenshot;
      options_.screenshot = original.parent_path() /
          (original.stem().wstring() + suffix + original.extension().wstring());
      screenshot();
      options_.screenshot = original;
    };
    if (host_frames_ == 27) popup_image(L"-file-menu");
    if (host_frames_ == 32) popup_image(L"-more-menu");
    if (host_frames_ == 43 || host_frames_ == 46 || host_frames_ == 53) {
      const auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
      const ImGuiID expected = host_frames_ == 43 ? display_menu_popup_ :
          host_frames_ == 46 ? view_settings_popup_ :
          inspector_test_window_ ? inspector_test_window_->GetID("Inspector preferences") : 0;
      if (popups.empty() || popups.back().PopupId != expected || !popups.back().Window)
        throw std::runtime_error("Display / inspector popup did not open at frame " + std::to_string(host_frames_));
      popup_image(host_frames_ == 43 ? L"-display-menu" : host_frames_ == 46 ? L"-view-settings" : L"-inspector-settings");
      if (host_frames_ == 43)
        ImGui::ActivateItemByID(popups.back().Window->GetID("Display and camera settings..."));
    }
    if (host_frames_ == 51) {
      if (!inspector_test_window_) throw std::runtime_error("Inspector window was not displayed");
      ImGui::ActivateItemByID(inspector_test_window_->GetID("Settings##Inspector settings"));
    }
  }
  void smoke_step() {
    if (options_.scene_first_smoke && host_frames_ > 6) layout_smoke_step();
    if (!document().loaded())
      throw std::runtime_error("Smoke capture did not load");
    if (host_frames_ == 1) {
      if (document().scenes().empty() || document().timeline().size() < 2)
        throw std::runtime_error("Smoke capture lacks scenes or frames");
      document().set_frame(document().timeline().size() - 1);
      const auto last = document().frame_index();
      document().step(-1);
      if (document().frame_index() + 1 != last)
        throw std::runtime_error("Previous frame navigation failed");
      document().set_frame(0);
      auto playback = document().playback();
      playback.frames_per_second = 60;
      playback.playing = true;
      document().set_playback(playback);
      document().advance(1.0 / 60.0);
      document().set_playing(false);
      if (document().frame_index() != 1)
        throw std::runtime_error("Playback advance failed");
      if (!document().set_sample_point(SamplePoint::after_fetch) ||
          !document().set_frame(3))
        throw std::runtime_error(
            "Explicit after-fetch frame navigation failed");
      smoke_navigation_ = true;
      set_fitted(false);
    }
    if (host_frames_ == 3) {
      document().set_frame(0);
      renderer_->render(*document().frame(), camera(), ui().render_options);
      const auto first =
          readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
      document().set_frame(document().timeline().size() - 1);
      renderer_->render(*document().frame(), camera(), ui().render_options);
      const auto last =
          readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
      for (std::size_t i = 0; i < first.pixels.size(); i += 4)
        if (first.pixels[i] != last.pixels[i] ||
            first.pixels[i + 1] != last.pixels[i + 1] ||
            first.pixels[i + 2] != last.pixels[i + 2])
          ++motion_pixels_;
      if (!motion_pixels_)
        throw std::runtime_error(
            "Frame navigation did not change the GPU image");
      document().set_frame(3);
      renderer_->render(*document().frame(), camera(), ui().render_options);
      unselected_ =
          readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
      auto frame = document().frame();
      // Prefer a visible simulated object, projecting its mesh center and
      // confirming the nearest ray hit. No selection through occluders.
      const auto pick_draws = visualization_pick_draws(
          *frame, ui().render_options, visualization_selection());
      auto accept_hit = [&](const std::optional<PickHit> &hit) {
        if (!hit || !document().select(hit->key))
          return false;
        const auto key = document().selected_key();
        const auto inspected = document().inspected_object();
        return key && inspected && *key == hit->key &&
               *inspected == hit->key.shape && document().inspected_instance();
      };
      for (const auto &shape : frame->render.instances) {
        if (smoke_picking_)
          break;
        if (shape.actor.kind != pvd::render::ActorKind::rigid_dynamic ||
            shape.flags.trigger)
          continue;
        for (const auto &item : frame->draws) {
          if (!(item.key == shape.picking_key()) ||
              item.mesh->positions.empty())
            continue;
          pvd::Vec3 center{};
          for (const auto &p : item.mesh->positions) {
            center.x += p.x;
            center.y += p.y;
            center.z += p.z;
          }
          const float n = static_cast<float>(item.mesh->positions.size());
          center.x /= n;
          center.y /= n;
          center.z /= n;
          const auto m = multiply(
              camera().view_projection(viewport_size_.x / viewport_size_.y),
              pose_matrix(item.world_pose));
          const auto &v = m.values;
          const double w =
              v[3] * center.x + v[7] * center.y + v[11] * center.z + v[15];
          if (w <= 0)
            continue;
          const double x =
              (v[0] * center.x + v[4] * center.y + v[8] * center.z + v[12]) / w;
          const double y =
              (v[1] * center.x + v[5] * center.y + v[9] * center.z + v[13]) / w;
          if (std::abs(x) >= 1 || std::abs(y) >= 1)
            continue;
          auto hit =
              pick(pick_draws,
                   camera().screen_ray((x + 1) * 0.5 * viewport_size_.x,
                                       (1 - y) * 0.5 * viewport_size_.y,
                                       viewport_size_.x, viewport_size_.y));
          if (hit && hit->key == item.key)
            smoke_picking_ = accept_hit(hit);
        }
      }
      for (double y = 0.18; y < 0.83 && !smoke_picking_; y += 0.04) {
        for (double x = 0.12; x < 0.90 && !smoke_picking_; x += 0.04) {
          auto hit = pick(
              pick_draws,
              camera().screen_ray(x * viewport_size_.x, y * viewport_size_.y,
                                  viewport_size_.x, viewport_size_.y));
          smoke_picking_ = accept_hit(hit);
        }
      }
      if (!smoke_picking_)
        throw std::runtime_error(
            "Visible triangle picking or linked inspection failed");
    }
    if (host_frames_ == 6 || (options_.scene_first_smoke && host_frames_ == 60)) {
      const auto selected =
          readTextureRgba8(device_.Get(), context_.Get(), renderer_->texture());
      if (selected.pixels.size() != unselected_.pixels.size())
        throw std::runtime_error("Viewport resized during smoke test");
      selection_pixels_ = 0;
      for (std::size_t i = 0; i < selected.pixels.size(); i += 4)
        if (selected.pixels[i] != unselected_.pixels[i] ||
            selected.pixels[i + 1] != unselected_.pixels[i + 1] ||
            selected.pixels[i + 2] != unselected_.pixels[i + 2])
          ++selection_pixels_;
      const auto stats = renderer_->stats();
      if (!stats.triangles || !stats.draw_calls || selection_pixels_ < 10) {
        if (!options_.screenshot.empty()) screenshot();
        throw std::runtime_error("GPU scene or selection highlight is empty at frame " +
            std::to_string(host_frames_) + ": " + std::to_string(stats.triangles) + " triangles, " +
            std::to_string(selection_pixels_) + " selected pixels");
      }
      if (options_.scene_first_smoke && host_frames_ == 6) {
        if (ui().sidebar != SlotUiState::Sidebar::none)
          throw std::runtime_error("Scene-first layout did not start with a full viewport");
        layout_smoke_viewport_ = viewport_size_;
        layout_smoke_camera_ = camera();
        layout_smoke_selection_ = document().selected_key();
        return;
      }
      if (!options_.screenshot.empty())
        screenshot();
      if (!options_.report.empty()) {
        if (!options_.report.parent_path().empty())
          std::filesystem::create_directories(options_.report.parent_path());
        std::ofstream report(options_.report);
        report << "{\n  \"passed\": true,\n  \"driver\": \""
               << (using_warp_ ? "WARP" : "hardware") << "\",\n"
               << "  \"navigation\": " << (smoke_navigation_ ? "true" : "false")
               << ",\n  \"cpu_triangle_picking\": true,\n  "
                  "\"linked_inspection\": true,\n"
               << "  \"selection_changed_pixels\": " << selection_pixels_
               << ",\n  \"shapes\": " << document().frame()->draws.size()
               << ",\n  \"frame_changed_pixels\": " << motion_pixels_
               << ",\n  \"triangles\": " << stats.triangles
               << ",\n  \"draw_calls\": " << stats.draw_calls
               << ",\n  \"scene_first_interactions\": " << (options_.scene_first_smoke ? "true" : "false")
               << ",\n  \"screenshot\": \""
               << escaped(utf8(options_.screenshot.wstring())) << "\"\n}\n";
        if (!report)
          throw std::runtime_error("Write smoke report failed");
      }
      quit_ = true;
    }
  }
};
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  Options options;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 1;
  try {
    options = arguments();
    {
      App app(options);
      result = app.run();
    }
  } catch (const std::exception &e) {
    OutputDebugStringA(e.what());
    try {
      if (!options.report.empty()) {
        if (!options.report.parent_path().empty())
          std::filesystem::create_directories(options.report.parent_path());
        std::ofstream report(options.report);
        report << "{\"passed\":false,\"error\":\"" << escaped(e.what())
               << "\"}\n";
      } else {
        std::ofstream log("pvd-viewer-error.log", std::ios::app);
        log << e.what() << '\n';
      }
    } catch (...) {
      OutputDebugStringA("Could not write PVD viewer error report");
    }
  }
  if (SUCCEEDED(com))
    CoUninitialize();
  return result;
}
