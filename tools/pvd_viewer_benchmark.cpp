#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <wrl/client.h>
#include "model.hpp"
#include "panels.hpp"
#include "renderer.hpp"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace pvd::viewer;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
void check(HRESULT result, const char *message) {
  if (FAILED(result)) throw std::runtime_error(message);
}
void panels(Document &document, PanelState &state) {
  auto &io = ImGui::GetIO(); io.DisplaySize = {1440, 900}; io.DeltaTime = 1.f / 60;
  ImGui::NewFrame();
  ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({300, 760});
  ImGui::Begin("Hierarchy", nullptr, ImGuiWindowFlags_NoSavedSettings);
  drawHierarchy(document, state); ImGui::End();
  ImGui::SetNextWindowPos({1130, 0}); ImGui::SetNextWindowSize({310, 760});
  ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoSavedSettings);
  drawInspector(document, state); ImGui::End();
  ImGui::SetNextWindowPos({0, 760}); ImGui::SetNextWindowSize({1440, 140});
  ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoSavedSettings);
  drawTimeline(document, state); ImGui::End(); ImGui::Render();
  if (!state.error.empty()) throw std::runtime_error("benchmark panel failed");
}
struct Gui {
  Gui() {
    ImGui::CreateContext(); auto &io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.Fonts->AddFontDefault(); unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }
  ~Gui() { ImGui::DestroyContext(); }
};
struct Gpu {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Query> complete;
  explicit Gpu(bool warp) {
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
        "D3D11 device creation failed");
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    check(device->CreateQuery(&desc, &complete), "GPU completion query creation failed");
  }
  void wait() {
    context->End(complete.Get()); context->Flush(); const auto begin = Clock::now();
    HRESULT result;
    while ((result = context->GetData(complete.Get(), nullptr, 0, 0)) == S_FALSE) {
      if (elapsed(begin) > 5000) throw std::runtime_error("GPU completion timed out");
      std::this_thread::yield();
    }
    check(result, "GPU completion failed");
  }
};
using Measurements = std::map<std::string, std::vector<double>>;
void distribution(const std::string &name, std::vector<double> values) {
  std::sort(values.begin(), values.end());
  std::cout << "    \"" << name << "\": {\"samples\": " << values.size();
  if (!values.empty())
    std::cout << ", \"p50_ms\": " << values[values.size() / 2]
              << ", \"p95_ms\": " << values[(values.size() - 1) * 95 / 100]
              << ", \"max_ms\": " << values.back();
  std::cout << '}';
}
}
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc < 2) throw std::invalid_argument(
        "usage: pvd_viewer_benchmark capture.pxd2 [--samples 16] [--warp] [--browse]");
    std::size_t count = 16; bool warp = false, browse = false;
    for (int i = 2; i < argc; ++i) {
      const std::wstring option = argv[i];
      if (option == L"--warp") warp = true;
      else if (option == L"--browse") browse = true;
      else if (option == L"--samples" && i + 1 < argc) {
        const std::wstring number = argv[++i]; std::size_t end = 0;
        count = std::stoull(number, &end);
        if (end != number.size() || count < 1 || count > 1024)
          throw std::invalid_argument("samples must be 1..1024");
      } else throw std::invalid_argument("unknown benchmark option");
    }
    const auto began = Clock::now(); Document document;
    std::cerr << "Loading and preparing scene...\n" << std::flush;
    document.open_file(std::filesystem::path(argv[1]).u8string());
    const auto load_ms = elapsed(began);
    const auto frame_count = document.timeline().size();
    if (!frame_count) throw std::runtime_error("benchmark requires a completed native frame");
    Gpu gpu(warp); Renderer renderer(gpu.device.Get(), gpu.context.Get());
    renderer.resize(960, 640); OrbitCamera camera; document.fit_scene(camera, 1.5);
    Gui gui; PanelState state; Measurements times;
    if (browse) {
      constexpr char query[] = "physx3";
      std::copy(std::begin(query), std::end(query), state.hierarchy_search.begin());
    }
    const auto render = [&](const std::string &phase) {
      const auto before = Clock::now(); renderer.render(*document.frame(), camera);
      const auto submitted = elapsed(before); gpu.wait();
      times[phase + "_render_submit"].push_back(submitted);
      times[phase + "_render_complete"].push_back(elapsed(before));
      const auto painted = Clock::now(); panels(document, state);
      times[phase + "_panels"].push_back(elapsed(painted));
    };
    render("cold");
    const auto first = document.frame();
    const auto misses = renderer.stats().cache_misses;
    std::cerr << "Measuring orbit, pan and zoom...\n" << std::flush;
    for (std::size_t i = 0; i < count; ++i) {
      const auto before = Clock::now(); camera.orbit(.005, -.002);
      camera.pan(.2, -.1, 640); camera.zoom(i % 2 ? -.01 : .01);
      times["camera_controls"].push_back(elapsed(before)); render("camera");
      if (document.frame() != first) throw std::runtime_error("camera replaced source frame");
    }
    const auto camera_uploads = renderer.stats().cache_misses - misses;
    std::cerr << "Measuring forward, reverse and random playback...\n" << std::flush;
    const auto samples = std::min(count, frame_count);
    std::uint64_t seed = 0x5f34d19;
    for (const std::string phase : {"forward", "reverse", "random"}) {
      if (phase != "random") {
        document.set_frame(phase == "forward" ? 0 : frame_count - 1);
        auto playback = document.playback(); playback.loop = true;
        playback.frames_per_second = phase == "forward" ? 60 : -60;
        playback.playing = true; document.set_playback(playback);
      }
      for (std::size_t i = 0; i < samples; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const auto before = Clock::now();
        if (phase == "random") document.set_frame(static_cast<std::size_t>(seed % frame_count));
        else document.advance(1. / 60);
        times[phase + "_update"].push_back(elapsed(before)); render(phase);
      }
    }
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
    std::cout << std::fixed << std::setprecision(4)
              << "{\n  \"load_ms\": " << load_ms
              << ",\n  \"events\": " << document.session()->event_count()
              << ",\n  \"native_frames\": " << frame_count
              << ",\n  \"initial_draws\": " << first->draws.size()
              << ",\n  \"camera_frame_identity_stable\": true"
              << ",\n  \"camera_mesh_uploads\": " << camera_uploads
              << ",\n  \"warp\": " << (warp ? "true" : "false")
              << ",\n  \"browse_objects\": " << (browse ? "true" : "false")
              << ",\n  \"peak_commit_bytes\": " << memory.PeakPagefileUsage
              << ",\n  \"private_bytes\": " << memory.PrivateUsage
              << ",\n  \"measurements\": {\n";
    bool comma = false;
    for (const auto &entry : times) {
      if (comma) std::cout << ",\n";
      comma = true; distribution(entry.first, entry.second);
    }
    std::cout << "\n  }\n}\n"; return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
