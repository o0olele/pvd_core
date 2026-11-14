#include "model.hpp"
#include "panels.hpp"
#include "renderer.hpp"
#ifdef PVD_VIEWER_HAS_TRANSPORT
#include "live_session.hpp"
#endif
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
using Microsoft::WRL::ComPtr;
using namespace pvd::viewer;
namespace {
struct Options {
    std::filesystem::path capture, screenshot, report;
    std::filesystem::path recording, ready;
    bool smoke = false, warp = false, hidden = false;
    bool live_smoke = false;
    int listen_port = -1;
};
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::string escaped(const std::string& text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    return out.str();
}
void check(HRESULT hr, const char* context) {
    if (FAILED(hr)) {
        std::ostringstream s; s << context << " (HRESULT 0x" << std::hex << unsigned(hr) << ')';
        throw std::runtime_error(s.str());
    }
}
Options arguments() {
    Options result;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) throw std::runtime_error("Cannot read command line");
    try {
        for (int i = 1; i < count; ++i) {
            std::wstring a = args[i];
            auto value = [&]() -> std::filesystem::path {
                if (++i == count) throw std::runtime_error("Missing option value");
                return args[i];
            };
            if (a == L"--smoke") { result.smoke = true; result.capture = value(); }
            else if (a == L"--screenshot") result.screenshot = value();
            else if (a == L"--report") result.report = value();
            else if (a == L"--warp") result.warp = true;
            else if (a == L"--hidden") result.hidden = true;
            else if (a == L"--capture") result.capture = value();
            else if (a == L"--record") result.recording = value();
            else if (a == L"--ready") result.ready = value();
            else if (a == L"--live-smoke") result.live_smoke = true;
            else if (a == L"--listen") {
                const auto number=value().wstring(); std::size_t used=0;
                const auto port=std::stoul(number,&used);
                if (used!=number.size() || port>65535) throw std::runtime_error("Listen port must be in [0,65535]");
                result.listen_port=static_cast<int>(port);
            }
            else if (!a.empty() && a[0] != L'-' && result.capture.empty()) result.capture = a;
            else throw std::runtime_error("Unknown command-line option: " + utf8(a));
        }
    } catch (...) { LocalFree(args); throw; }
    LocalFree(args);
    if (result.listen_port>=0 && !result.capture.empty()) throw std::runtime_error("Choose a file or a live listener as the initial source");
    if (result.live_smoke && (result.listen_port<0 || result.recording.empty())) throw std::runtime_error("Live smoke requires --listen and --record");
#ifndef PVD_VIEWER_HAS_TRANSPORT
    if (result.listen_port>=0 || result.live_smoke) throw std::runtime_error("This viewer was built without the transport module");
#endif
    return result;
}

class App {
public:
    explicit App(Options options) : options_(std::move(options)) {}
    ~App() {
#ifdef PVD_VIEWER_HAS_TRANSPORT
        live_.stop();
#endif
        if (pending_.valid()) pending_.wait();
        renderer_.reset();
        if (dx11_backend_) ImGui_ImplDX11_Shutdown();
        if (win32_backend_) ImGui_ImplWin32_Shutdown();
        if (imgui_) ImGui::DestroyContext();
        if (context_) context_->ClearState();
        backbuffer_.Reset(); swapchain_.Reset(); context_.Reset(); device_.Reset();
        if (window_) DestroyWindow(window_);
        UnregisterClassW(L"PvdStudioWindow", GetModuleHandleW(nullptr));
    }
    int run() {
        initialize();
#ifdef PVD_VIEWER_HAS_TRANSPORT
        if (options_.listen_port>=0) {
            capture_port_=options_.listen_port;
            record_capture_=!options_.recording.empty();
            if (record_capture_) copy_path(options_.recording);
            start_capture();
            if (!error_.empty()) throw std::runtime_error(error_);
            if (!options_.ready.empty()) {
                auto temporary=options_.ready;temporary+=L".tmp";
                { std::ofstream ready(temporary);
                  ready << "{\"port\":" << live_.status().port << "}\n";
                  if (!ready) throw std::runtime_error("Cannot write live listener ready file"); }
                std::filesystem::rename(temporary,options_.ready);
            }
        }
#endif
        if (!options_.capture.empty()) {
            if (options_.smoke) { document_.open_file(utf8(options_.capture.wstring())); loaded(); }
            else open(options_.capture);
        }
        const auto started = std::chrono::steady_clock::now();
        auto previous = started;
        while (!quit_) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) quit_ = true;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (quit_) break;
            poll_load();
#ifdef PVD_VIEWER_HAS_TRANSPORT
            poll_live();
#endif
            if (resize_width_ && resize_height_) {
                context_->OMSetRenderTargets(0, nullptr, nullptr); backbuffer_.Reset();
                check(swapchain_->ResizeBuffers(0, resize_width_, resize_height_, DXGI_FORMAT_UNKNOWN, 0), "Resize window");
                create_backbuffer(); resize_width_ = resize_height_ = 0;
            }
            if (IsIconic(window_)) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); continue; }
            auto now = std::chrono::steady_clock::now();
            document_.advance(std::min(0.1, std::chrono::duration<double>(now - previous).count())); previous = now;
            if ((options_.smoke || options_.live_smoke) && std::chrono::duration<double>(now - started).count() > 30)
                throw std::runtime_error("Viewer smoke test timed out");
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
            draw();
            ImGui::Render();
            const float background[] = {0.035f, 0.047f, 0.067f, 1};
            auto* target = backbuffer_.Get();
            context_->OMSetRenderTargets(1, &target, nullptr);
            context_->ClearRenderTargetView(target, background);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            ++host_frames_;
            if (options_.smoke) smoke_step();
#ifdef PVD_VIEWER_HAS_TRANSPORT
            else if (options_.live_smoke) live_smoke_step();
#endif
            else if (!options_.screenshot.empty() && document_.loaded() && host_frames_ > 8) {
                screenshot(); options_.screenshot.clear();
            }
            const HRESULT present = swapchain_->Present((options_.smoke || options_.live_smoke) ? 0 : 1, 0);
            if (FAILED(present)) check(present, "Present frame");
            if (options_.live_smoke || present==DXGI_STATUS_OCCLUDED) std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        return 0;
    }
private:
    Options options_;
    HWND window_ = nullptr;
    bool quit_ = false, imgui_ = false, fitted_ = false, using_warp_ = false, diagnostics_ = false;
    bool win32_backend_ = false, dx11_backend_ = false;
    UINT resize_width_ = 0, resize_height_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapchain_;
    ComPtr<ID3D11RenderTargetView> backbuffer_;
    std::unique_ptr<Renderer> renderer_;
    Document document_;
    OrbitCamera camera_;
    RenderOptions render_options_;
    PanelState panels_;
    std::future<std::unique_ptr<Document>> pending_;
    std::string loading_, error_;
    std::size_t host_frames_ = 0;
    ImVec2 viewport_origin_{}, viewport_size_{};
    RgbaImage unselected_;
    bool smoke_navigation_ = false, smoke_picking_ = false;
    std::size_t selection_pixels_ = 0;
    std::size_t motion_pixels_ = 0;
    std::optional<std::uint64_t> displayed_scene_;
#ifdef PVD_VIEWER_HAS_TRANSPORT
    LiveSession live_;
    std::array<char,64> capture_address_{'1','2','7','.','0','.','0','.','1'};
    std::array<char,4096> capture_path_{};
    int capture_port_=5425;
    bool record_capture_=true, live_document_seen_=false, capture_setup_requested_=false;
    std::chrono::steady_clock::time_point next_live_poll_{};
    std::size_t online_render_frames_=0, live_display_changes_=0;
    std::optional<std::size_t> last_live_event_count_;
#endif

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
        if (self) {
            if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
                self->resize_width_ = LOWORD(lp); self->resize_height_ = HIWORD(lp); return 0;
            }
            if (msg == WM_DROPFILES) {
                const auto drop = reinterpret_cast<HDROP>(wp);
                const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
                std::wstring path(length + 1, L'\0'); DragQueryFileW(drop, 0, path.data(), length + 1);
                path.resize(length); DragFinish(drop); self->open(path); return 0;
            }
            if (msg == WM_GETMINMAXINFO) {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
                limits->ptMinTrackSize = {1060, 700}; return 0;
            }
            if (msg == WM_CLOSE) { self->quit_ = true; return 0; }
            if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
            if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_KEYMENU) return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    void create_backbuffer() {
        ComPtr<ID3D11Texture2D> buffer;
        check(swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Get backbuffer");
        check(device_->CreateRenderTargetView(buffer.Get(), nullptr, &backbuffer_), "Create backbuffer view");
    }
    void initialize() {
        ImGui_ImplWin32_EnableDpiAwareness();
        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        wc.style = CS_CLASSDC; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"PvdStudioWindow";
        if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register viewer window");
        RECT rectangle{0, 0, 1440, 900}; AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        window_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"PVD Studio", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, wc.hInstance, this);
        if (!window_) throw std::runtime_error("Cannot create viewer window");
        BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = window_;
        sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        auto create = [&](D3D_DRIVER_TYPE driver) {
            return D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, 1, D3D11_SDK_VERSION, &sd, &swapchain_, &device_, nullptr, &context_);
        };
        HRESULT hr = create(options_.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
        using_warp_ = options_.warp;
        if (FAILED(hr) && !options_.warp) { using_warp_ = true; hr = create(D3D_DRIVER_TYPE_WARP); }
        check(hr, "Create Direct3D 11 device"); create_backbuffer();
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui_ = true;
        auto& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr; // Viewer layout is deterministic; no cwd side effects.
        theme();
        ImFontConfig font; font.SizePixels = 17;
        const std::filesystem::path segoe = L"C:/Windows/Fonts/segoeui.ttf";
        if (std::filesystem::exists(segoe)) io.Fonts->AddFontFromFileTTF(segoe.string().c_str(), 17, &font);
        else io.Fonts->AddFontDefault();
        const std::filesystem::path cjk = L"C:/Windows/Fonts/msyh.ttc";
        if (std::filesystem::exists(cjk)) {
            ImFontConfig merge; merge.MergeMode = true; merge.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(cjk.string().c_str(), 17, &merge, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        if (!ImGui_ImplWin32_Init(window_)) throw std::runtime_error("Initialize Win32 backend");
        win32_backend_ = true;
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
            throw std::runtime_error("Initialize Direct3D UI backend");
        }
        dx11_backend_ = true;
        renderer_ = std::make_unique<Renderer>(device_.Get(), context_.Get());
        if (!options_.hidden) { ShowWindow(window_, SW_SHOWDEFAULT); UpdateWindow(window_); }
        resize_width_ = resize_height_ = 0;
    }
    void theme() {
        ImGui::StyleColorsDark();
        auto& s = ImGui::GetStyle();
        s.WindowPadding = {16, 12}; s.FramePadding = {9, 6}; s.ItemSpacing = {9, 8};
        s.WindowRounding = 0; s.ChildRounding = 7; s.FrameRounding = 5;
        s.PopupRounding = 7; s.GrabRounding = 4; s.ScrollbarRounding = 5;
        s.WindowBorderSize = 0; s.ChildBorderSize = 1; s.FrameBorderSize = 0;
        auto* c = s.Colors;
        c[ImGuiCol_WindowBg] = {0.043f,0.059f,0.082f,1};
        c[ImGuiCol_ChildBg] = {0.063f,0.082f,0.110f,1};
        c[ImGuiCol_Text] = {0.87f,0.91f,0.95f,1};
        c[ImGuiCol_TextDisabled] = {0.43f,0.52f,0.62f,1};
        c[ImGuiCol_Border] = {0.15f,0.20f,0.27f,1};
        c[ImGuiCol_FrameBg] = {0.10f,0.14f,0.18f,1};
        c[ImGuiCol_FrameBgHovered] = {0.14f,0.22f,0.26f,1};
        c[ImGuiCol_FrameBgActive] = {0.12f,0.28f,0.29f,1};
        c[ImGuiCol_Button] = {0.11f,0.20f,0.24f,1};
        c[ImGuiCol_ButtonHovered] = {0.13f,0.33f,0.35f,1};
        c[ImGuiCol_ButtonActive] = {0.10f,0.44f,0.43f,1};
        c[ImGuiCol_Header] = {0.10f,0.28f,0.30f,1};
        c[ImGuiCol_HeaderHovered] = {0.12f,0.33f,0.36f,1};
        c[ImGuiCol_HeaderActive] = {0.12f,0.39f,0.40f,1};
        c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = {0.24f,0.82f,0.72f,1};
        c[ImGuiCol_SliderGrabActive] = {0.40f,0.94f,0.82f,1};
        c[ImGuiCol_Separator] = c[ImGuiCol_Border];
        c[ImGuiCol_TableHeaderBg] = {0.08f,0.12f,0.16f,1};
        c[ImGuiCol_TableRowBgAlt] = {0.12f,0.16f,0.21f,0.35f};
    }
    void choose_file() {
        std::wstring buffer(32768, L'\0');
        OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
        dialog.hwndOwner = window_; dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrFilter = L"PhysX raw captures (*.pxd2;*.pvd)\0*.pxd2;*.pvd\0All files\0*.*\0";
        dialog.lpstrTitle = L"Open PhysX capture";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) { buffer.resize(wcslen(buffer.c_str())); open(buffer); }
    }
    void open(const std::filesystem::path& path) {
#ifdef PVD_VIEWER_HAS_TRANSPORT
        if (live_.active()) { error_="Stop the live capture and wait for its remaining events before opening a file."; return; }
#endif
        if (pending_.valid()) return;
        error_.clear(); loading_ = utf8(path.filename().wstring());
        document_.set_playing(false);
        pending_ = std::async(std::launch::async, [path] {
            auto result = std::make_unique<Document>(); result->open_file(utf8(path.wstring())); return result;
        });
    }
    void poll_load() {
        if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try { document_ = std::move(*pending_.get()); loaded(); }
        catch (const std::exception& e) { error_ = e.what(); }
        loading_.clear();
    }
    void loaded() {
        fitted_ = false; panels_ = {}; renderer_->clear_mesh_cache();
        std::wstring title = L"PVD Studio  |  " + std::filesystem::u8path(document_.source_label()).filename().wstring();
        SetWindowTextW(window_, title.c_str());
    }
#ifdef PVD_VIEWER_HAS_TRANSPORT
    void copy_path(const std::filesystem::path& path) {
        const auto text=utf8(path.wstring());
        if (text.size()>=capture_path_.size()) throw std::runtime_error("Recording path is too long for the capture field");
        capture_path_.fill('\0');
        std::copy(text.begin(),text.end(),capture_path_.begin());
    }
    void default_recording_path() {
        SYSTEMTIME time{}; GetLocalTime(&time);
        wchar_t name[128]{};
        swprintf_s(name,L"capture-%04u%02u%02u-%02u%02u%02u-%03u.pxd2",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
        copy_path(std::filesystem::current_path()/L"captures"/name);
    }
    void choose_recording() {
        std::wstring buffer(32768,L'\0');
        if (capture_path_[0]) {
            const auto path=std::filesystem::u8path(capture_path_.data()).wstring();
            std::copy_n(path.data(),std::min(path.size(),buffer.size()-1),buffer.data());
        }
        OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
        dialog.hwndOwner=window_; dialog.lpstrFile=buffer.data(); dialog.nMaxFile=static_cast<DWORD>(buffer.size());
        dialog.lpstrFilter=L"PhysX raw capture (*.pxd2)\0*.pxd2\0All files\0*.*\0";
        dialog.lpstrDefExt=L"pxd2"; dialog.lpstrTitle=L"Choose a new recording file";
        dialog.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) { buffer.resize(wcslen(buffer.c_str())); copy_path(buffer); }
    }
    void start_capture() {
        try {
            if (pending_.valid()) throw std::runtime_error("Wait for the file load to finish before listening");
            pvd::CaptureOptions options;
            options.address=capture_address_.data();
            if (capture_port_<0 || capture_port_>65535) throw std::runtime_error("Port must be in [0,65535]; 0 chooses a free port");
            options.port=static_cast<std::uint16_t>(capture_port_);
            if (record_capture_) {
                if (!capture_path_[0]) default_recording_path();
                const auto path=std::filesystem::u8path(capture_path_.data());
                if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
                options.recording_path=capture_path_.data();
            }
            live_.start(std::move(options)); live_document_seen_=false;
            last_live_event_count_.reset(); online_render_frames_=live_display_changes_=0;
            next_live_poll_={}; error_.clear();
        } catch (const std::exception& e) { error_=e.what(); }
    }
    void poll_live() {
        const auto now=std::chrono::steady_clock::now();
        if (now<next_live_poll_) return;
        next_live_poll_=now+std::chrono::milliseconds(100);
        try {
            const auto* previous=document_.session();
            const bool changed=live_.poll(document_);
            if (!live_document_seen_ && document_.session()!=previous && live_.status().header_complete) {
                loaded(); live_document_seen_=true;
            }
            if (changed) ++live_display_changes_;
            if (!live_.error().empty()) error_=live_.error();
        } catch (const std::exception& e) { live_.stop(); error_=e.what(); }
    }
    void capture_setup() {
        if (capture_setup_requested_) {
            if (!capture_path_[0]) default_recording_path();
            ImGui::OpenPopup("Live capture"); capture_setup_requested_=false;
        }
        ImGui::SetNextWindowSize({580,0},ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Live capture",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::TextUnformatted("Receive a PhysX PVD connection");
        ImGui::TextWrapped("Start listening, then connect the PhysX application to the address and port below.");
        ImGui::Spacing(); ImGui::BeginDisabled(live_.active());
        ImGui::SetNextItemWidth(290); ImGui::InputText("Bind address",capture_address_.data(),capture_address_.size());
        ImGui::SetNextItemWidth(140); ImGui::InputInt("Port",&capture_port_);
        ImGui::TextDisabled("5425 is the PVD default; 0 chooses an available port.");
        ImGui::Checkbox("Record raw capture",&record_capture_);
        ImGui::BeginDisabled(!record_capture_);
        ImGui::SetNextItemWidth(425); ImGui::InputText("##recording",capture_path_.data(),capture_path_.size());
        ImGui::SameLine(); if (ImGui::Button("Browse...")) choose_recording();
        ImGui::TextDisabled("Choose a new file. Existing recordings are kept.");
        ImGui::EndDisabled();
        if (ImGui::Button("Start listening",{150,0})) {
            start_capture(); if (error_.empty()) ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        if (!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
        ImGui::EndPopup();
    }
    void capture_bar() {
        const auto& status=live_.status();
        if (status.state==pvd::CaptureState::idle) return;
        const bool failed=status.state==pvd::CaptureState::failed || status.state==pvd::CaptureState::truncated;
        ImGui::TextColored(failed?ImVec4{1,.57f,.42f,1}:ImVec4{.28f,.86f,.75f,1},"%s",pvd::capture_state_name(status.state));
        ImGui::SameLine(); ImGui::TextDisabled("%s:%u",status.address.c_str(),unsigned(status.port));
        ImGui::SameLine(); ImGui::Text("%.2f MiB  |  %zu frames",double(status.wire_bytes)/(1024*1024),status.frame_count);
        ImGui::SameLine(); ImGui::BeginDisabled(!live_.active());
        if (ImGui::SmallButton("Stop")) live_.stop(); ImGui::EndDisabled(); ImGui::SameLine();
        bool follow=live_.follow_live(); if (ImGui::Checkbox("Follow live",&follow)) live_.set_follow_live(follow);
        if (status.recoverable_event_count>live_.accepted_event_count()) {
            ImGui::SameLine(); ImGui::TextDisabled("%zu events pending",status.recoverable_event_count-live_.accepted_event_count());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Received %llu bytes; recorded %llu; recoverable %llu; discarded tail %llu",static_cast<unsigned long long>(status.wire_bytes),
            static_cast<unsigned long long>(status.recorded_bytes),static_cast<unsigned long long>(status.recoverable_bytes),static_cast<unsigned long long>(status.discarded_tail_bytes));
        if (!status.errors.empty()) ImGui::TextWrapped("%s",status.errors.front().c_str());
        ImGui::Separator();
    }
    void live_smoke_step() {
        const auto& status=live_.status();
        if (document_.loaded() && !document_.frame()->draws.empty() && status.state==pvd::CaptureState::receiving) {
            ++online_render_frames_;
            last_live_event_count_=document_.frame()->snapshot.event_count;
        }
        if (!status.terminal() || live_.active() || !live_.caught_up()) return;
        if (status.state!=pvd::CaptureState::completed || !status.errors.empty() || !live_.error().empty())
            throw std::runtime_error("Live GPU capture did not finish cleanly");
        if (online_render_frames_<3 || live_display_changes_<2 || !renderer_->stats().triangles)
            throw std::runtime_error("The GPU did not render enough changing frames while receiving");
        if (status.wire_bytes!=status.recorded_bytes || status.discarded_tail_bytes || status.event_count!=live_.accepted_event_count())
            throw std::runtime_error("Live recording did not preserve the complete event prefix");
        Document replay; replay.open_file(utf8(options_.recording.wstring()));
        if (!replay.session() || replay.session()->event_count()!=document_.session()->event_count() || replay.scenes().size()!=document_.scenes().size())
            throw std::runtime_error("Live recording cannot be reopened with identical indices");
        if (!options_.screenshot.empty()) screenshot();
        if (!options_.report.empty()) {
            std::ofstream report(options_.report);
            report << "{\n  \"passed\":true,\n  \"state\":\"completed\",\n  \"online_render_frames\":" << online_render_frames_
                << ",\n  \"live_display_changes\":" << live_display_changes_ << ",\n  \"wire_bytes\":" << status.wire_bytes
                << ",\n  \"recorded_bytes\":" << status.recorded_bytes << ",\n  \"events\":" << status.event_count
                << ",\n  \"frames\":" << status.frame_count << ",\n  \"reopened\":true\n}\n";
            if (!report) throw std::runtime_error("Cannot write live GPU smoke report");
        }
        quit_=true;
    }
#endif
    void shortcuts() {
        const auto& io = ImGui::GetIO();
        if (io.WantTextInput) return;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) choose_file();
        if (!document_.loaded()) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) document_.set_playing(!document_.playback().playing);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { document_.set_playing(false); document_.step(-1); }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { document_.set_playing(false); document_.step(1); }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) document_.set_frame(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End) && !document_.timeline().empty()) document_.set_frame(document_.timeline().size()-1);
        if (ImGui::IsKeyPressed(ImGuiKey_F)) document_.fit_scene(camera_, viewport_size_.x / std::max(1.0f, viewport_size_.y));
    }
    void draw() {
        shortcuts();
        const auto* main = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main->Pos); ImGui::SetNextWindowSize(main->Size);
        ImGui::Begin("PVD Studio##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::TextColored({0.28f,0.86f,0.75f,1}, "PVD"); ImGui::SameLine(); ImGui::TextUnformatted("STUDIO");
        ImGui::SameLine(170); ImGui::BeginDisabled(pending_.valid()
#ifdef PVD_VIEWER_HAS_TRANSPORT
            || live_.active()
#endif
        );
        if (ImGui::Button("Open capture  Ctrl+O")) choose_file(); ImGui::EndDisabled();
#ifdef PVD_VIEWER_HAS_TRANSPORT
        ImGui::SameLine(); if (ImGui::Button("Live capture")) capture_setup_requested_=true;
        capture_setup();
#endif
        ImGui::SameLine();
        if (!loading_.empty()) ImGui::TextDisabled("Opening %s ...", loading_.c_str());
        else if (document_.loaded()) ImGui::TextDisabled("%s", std::filesystem::u8path(document_.source_label()).filename().u8string().c_str());
        else ImGui::TextDisabled("PhysX 3.4  /  capture viewer");
        ImGui::Separator();
#ifdef PVD_VIEWER_HAS_TRANSPORT
        capture_bar();
#endif
        if (!error_.empty()) {
            ImGui::TextColored({1,0.57f,0.42f,1}, "Error"); ImGui::SameLine();
            ImGui::TextWrapped("%s", error_.c_str());
            if (ImGui::SmallButton("Dismiss")) error_.clear();
        }
        const float timeline_height = 130, status_height = 24;
        const float body_height = std::max(100.0f, ImGui::GetContentRegionAvail().y - timeline_height - status_height - 16);
        if (ImGui::BeginTable("workspace", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthFixed, 246);
            ImGui::TableSetupColumn("Viewport", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 310);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::BeginChild("Hierarchy", {0,body_height}, ImGuiChildFlags_Borders);
            drawHierarchy(document_, panels_); ImGui::EndChild();
            ImGui::TableNextColumn();
            ImGui::BeginChild("Scene", {0,body_height}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            viewport(); ImGui::EndChild();
            ImGui::TableNextColumn();
            ImGui::BeginChild("Inspector", {0,body_height}, ImGuiChildFlags_Borders);
            if (diagnostics_) drawDiagnostics(document_, panels_); else drawInspector(document_, panels_);
            ImGui::EndChild(); ImGui::EndTable();
        }
        ImGui::BeginChild("Timeline", {0,timeline_height}, ImGuiChildFlags_Borders);
        drawTimeline(document_, panels_); ImGui::EndChild();
        const auto frame = document_.frame(); const auto stats = renderer_->stats();
        if (document_.loaded() && frame) ImGui::TextDisabled("%zu shapes   |   %llu triangles   |   %zu cached meshes", frame->draws.size(),
            static_cast<unsigned long long>(stats.triangles), stats.cached_meshes);
        else ImGui::TextDisabled("Ready  /  Open or drop a PhysX raw capture");
        ImGui::SameLine(std::max(500.0f, ImGui::GetWindowWidth()-310));
        ImGui::TextDisabled("D3D11 %s", using_warp_ ? "WARP" : "GPU"); ImGui::SameLine();
        ImGui::Checkbox("Diagnostics", &diagnostics_);
        ImGui::End();
    }
    void viewport() {
        ImGui::TextUnformatted("SCENE"); ImGui::SameLine();
        if (ImGui::SmallButton("Fit  F")) document_.fit_scene(camera_, viewport_size_.x / std::max(1.0f,viewport_size_.y));
        ImGui::SameLine(); ImGui::Checkbox("Grid", &render_options_.show_grid);
        ImGui::SameLine(); ImGui::Checkbox("Wire", &render_options_.show_wireframe);
        const auto& style=ImGui::GetStyle();
        const float right=ImGui::GetWindowPos().x+ImGui::GetWindowWidth()-style.WindowPadding.x;
        const float contact_width=ImGui::CalcTextSize("Contacts").x+ImGui::GetFrameHeight()+style.ItemSpacing.x*2;
        if (right-ImGui::GetItemRectMax().x > contact_width) ImGui::SameLine();
        ImGui::Checkbox("Contacts", &render_options_.show_contacts);
        const auto frame = document_.frame();
        if (!document_.loaded() || !frame) {
            ImGui::Dummy({0, ImGui::GetContentRegionAvail().y * 0.32f});
            drawEmptyState("Inspect your simulation", "Open a PhysX 3.4 raw capture to explore geometry, frames and object properties.");
            if (ImGui::Button("Open capture...")) choose_file();
            return;
        }
        viewport_size_ = ImGui::GetContentRegionAvail(); viewport_size_.y = std::max(1.0f,viewport_size_.y-25);
        viewport_size_.x = std::max(1.0f,viewport_size_.x);
        if (displayed_scene_ != document_.scene_id()) { fitted_ = false; displayed_scene_ = document_.scene_id(); }
        if (!fitted_) fitted_ = document_.fit_scene(camera_, viewport_size_.x/viewport_size_.y);
        renderer_->resize(static_cast<UINT>(viewport_size_.x), static_cast<UINT>(viewport_size_.y));
        viewport_origin_ = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("Scene interaction", viewport_size_, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered(); const auto& io = ImGui::GetIO();
        if (hovered && io.MouseWheel != 0) camera_.zoom(io.MouseWheel);
        if (ImGui::IsItemActive()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
                camera_.orbit(-io.MouseDelta.x*0.008, -io.MouseDelta.y*0.008);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) camera_.pan(io.MouseDelta.x,io.MouseDelta.y,viewport_size_.y);
        }
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyAlt && io.MouseDragMaxDistanceSqr[0] < 16) {
            const auto hit = pick(frame->draws, camera_.screen_ray(io.MousePos.x-viewport_origin_.x,io.MousePos.y-viewport_origin_.y,viewport_size_.x,viewport_size_.y));
            if (hit) document_.select(hit->key); else document_.clear_selection();
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) document_.fit_selected(camera_,viewport_size_.x/viewport_size_.y);
        renderer_->render(*document_.frame(), camera_, render_options_);
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(renderer_->shader_resource_view()), viewport_origin_,
            {viewport_origin_.x+viewport_size_.x,viewport_origin_.y+viewport_size_.y});
        // Text stays in the UI layer; the reusable GPU renderer has no font dependency.
        auto* draw_list=ImGui::GetWindowDrawList();
        draw_list->PushClipRect(viewport_origin_,{viewport_origin_.x+viewport_size_.x,viewport_origin_.y+viewport_size_.y},true);
        const auto projection=camera_.view_projection(viewport_size_.x/viewport_size_.y);
        const auto& m=projection.values;
        for (const auto& text : document_.frame()->debug.texts) {
            const auto& p=text.position;
            const double w=m[3]*p.x+m[7]*p.y+m[11]*p.z+m[15];
            if (!(w>0) || !std::isfinite(w)) continue;
            const double x=(m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12])/w;
            const double y=(m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13])/w;
            const double z=(m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14])/w;
            if (!(std::abs(x)<=1 && std::abs(y)<=1 && z>=0 && z<=1)) continue;
            const ImVec2 pos{viewport_origin_.x+float((x+1)*0.5)*viewport_size_.x,viewport_origin_.y+float((1-y)*0.5)*viewport_size_.y};
            const auto color=IM_COL32((text.color>>16)&255,(text.color>>8)&255,text.color&255,(text.color>>24)&255);
            const float size=ImGui::GetFontSize()*(std::isfinite(text.size)?std::clamp(text.size,0.6f,2.0f):1.0f);
            draw_list->AddText(ImGui::GetFont(),size,pos,color,text.text.data(),text.text.data()+std::min<std::size_t>(text.text.size(),4096));
        }
        draw_list->PopClipRect();
        ImGui::TextDisabled("RMB orbit   MMB pan   Wheel zoom   Click select");
    }
    void screenshot() {
        ComPtr<ID3D11Texture2D> buffer;
        check(swapchain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Capture backbuffer");
        if (!options_.screenshot.parent_path().empty()) std::filesystem::create_directories(options_.screenshot.parent_path());
        saveTexturePng(device_.Get(),context_.Get(),buffer.Get(),options_.screenshot);
    }
    void smoke_step() {
        if (!document_.loaded()) throw std::runtime_error("Smoke capture did not load");
        if (host_frames_ == 1) {
            if (document_.scenes().empty() || document_.timeline().size() < 2) throw std::runtime_error("Smoke capture lacks scenes or frames");
            document_.set_frame(document_.timeline().size()-1);
            const auto last = document_.frame_index(); document_.step(-1);
            if (document_.frame_index()+1 != last) throw std::runtime_error("Previous frame navigation failed");
            document_.set_frame(0);
            auto playback = document_.playback(); playback.frames_per_second = 60; playback.playing = true;
            document_.set_playback(playback); document_.advance(1.0/60.0); document_.set_playing(false);
            if (document_.frame_index() != 1) throw std::runtime_error("Playback advance failed");
            if (!document_.set_sample_point(SamplePoint::after_fetch) || !document_.set_frame(3))
                throw std::runtime_error("Explicit after-fetch frame navigation failed");
            smoke_navigation_ = true; fitted_ = false;
        }
        if (host_frames_ == 3) {
            document_.set_frame(0); renderer_->render(*document_.frame(),camera_,render_options_);
            const auto first = readTextureRgba8(device_.Get(),context_.Get(),renderer_->texture());
            document_.set_frame(document_.timeline().size()-1); renderer_->render(*document_.frame(),camera_,render_options_);
            const auto last = readTextureRgba8(device_.Get(),context_.Get(),renderer_->texture());
            for (std::size_t i=0; i<first.pixels.size(); i+=4)
                if (first.pixels[i]!=last.pixels[i] || first.pixels[i+1]!=last.pixels[i+1] || first.pixels[i+2]!=last.pixels[i+2]) ++motion_pixels_;
            if (!motion_pixels_) throw std::runtime_error("Frame navigation did not change the GPU image");
            document_.set_frame(3); renderer_->render(*document_.frame(),camera_,render_options_);
            unselected_ = readTextureRgba8(device_.Get(),context_.Get(),renderer_->texture());
            auto frame = document_.frame();
            // Prefer a visible simulated object, projecting its mesh center and
            // confirming the nearest ray hit. No selection through occluders.
            auto accept_hit = [&](const std::optional<PickHit>& hit) {
                if (!hit || !document_.select(hit->key)) return false;
                const auto key=document_.selected_key(); const auto inspected=document_.inspected_object();
                return key && inspected && *key==hit->key && *inspected==hit->key.shape && document_.inspected_instance();
            };
            for (const auto& shape : frame->render.instances) {
                if (smoke_picking_) break;
                if (shape.actor.kind != pvd::render::ActorKind::rigid_dynamic || shape.flags.trigger) continue;
                for (const auto& item : frame->draws) {
                    if (!(item.key==shape.picking_key()) || item.mesh->positions.empty()) continue;
                    pvd::Vec3 center{};
                    for (const auto& p : item.mesh->positions) { center.x+=p.x; center.y+=p.y; center.z+=p.z; }
                    const float n=static_cast<float>(item.mesh->positions.size()); center.x/=n; center.y/=n; center.z/=n;
                    const auto m=multiply(camera_.view_projection(viewport_size_.x/viewport_size_.y),pose_matrix(item.world_pose));
                    const auto& v=m.values;
                    const double w=v[3]*center.x+v[7]*center.y+v[11]*center.z+v[15];
                    if (w<=0) continue;
                    const double x=(v[0]*center.x+v[4]*center.y+v[8]*center.z+v[12])/w;
                    const double y=(v[1]*center.x+v[5]*center.y+v[9]*center.z+v[13])/w;
                    if (std::abs(x)>=1 || std::abs(y)>=1) continue;
                    auto hit=pick(frame->draws,camera_.screen_ray((x+1)*0.5*viewport_size_.x,(1-y)*0.5*viewport_size_.y,viewport_size_.x,viewport_size_.y));
                    if (hit && hit->key==item.key) smoke_picking_=accept_hit(hit);
                }
            }
            for (double y = 0.18; y < 0.83 && !smoke_picking_; y += 0.04) {
                for (double x = 0.12; x < 0.90 && !smoke_picking_; x += 0.04) {
                    auto hit = pick(frame->draws, camera_.screen_ray(x*viewport_size_.x,y*viewport_size_.y,viewport_size_.x,viewport_size_.y));
                    smoke_picking_ = accept_hit(hit);
                }
            }
            if (!smoke_picking_) throw std::runtime_error("Visible triangle picking or linked inspection failed");
        }
        if (host_frames_ == 6) {
            const auto selected = readTextureRgba8(device_.Get(),context_.Get(),renderer_->texture());
            if (selected.pixels.size() != unselected_.pixels.size()) throw std::runtime_error("Viewport resized during smoke test");
            for (std::size_t i=0; i<selected.pixels.size(); i+=4)
                if (selected.pixels[i]!=unselected_.pixels[i] || selected.pixels[i+1]!=unselected_.pixels[i+1] || selected.pixels[i+2]!=unselected_.pixels[i+2]) ++selection_pixels_;
            const auto stats = renderer_->stats();
            if (!stats.triangles || !stats.draw_calls || selection_pixels_ < 10) throw std::runtime_error("GPU scene or selection highlight is empty");
            if (!options_.screenshot.empty()) screenshot();
            if (!options_.report.empty()) {
                if (!options_.report.parent_path().empty()) std::filesystem::create_directories(options_.report.parent_path());
                std::ofstream report(options_.report);
                report << "{\n  \"passed\": true,\n  \"driver\": \"" << (using_warp_?"WARP":"hardware") << "\",\n"
                    << "  \"navigation\": " << (smoke_navigation_?"true":"false") << ",\n  \"cpu_triangle_picking\": true,\n  \"linked_inspection\": true,\n"
                    << "  \"selection_changed_pixels\": " << selection_pixels_ << ",\n  \"shapes\": " << document_.frame()->draws.size()
                    << ",\n  \"frame_changed_pixels\": " << motion_pixels_
                    << ",\n  \"triangles\": " << stats.triangles << ",\n  \"draw_calls\": " << stats.draw_calls
                    << ",\n  \"screenshot\": \"" << escaped(utf8(options_.screenshot.wstring())) << "\"\n}\n";
                if (!report) throw std::runtime_error("Write smoke report failed");
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
    try { options = arguments(); { App app(options); result = app.run(); } }
    catch (const std::exception& e) {
        OutputDebugStringA(e.what());
        try {
            if (!options.report.empty()) {
                if (!options.report.parent_path().empty()) std::filesystem::create_directories(options.report.parent_path());
                std::ofstream report(options.report);
                report << "{\"passed\":false,\"error\":\"" << escaped(e.what()) << "\"}\n";
            } else {
                std::ofstream log("pvd-viewer-error.log", std::ios::app); log << e.what() << '\n';
            }
        } catch (...) { OutputDebugStringA("Could not write PVD viewer error report"); }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
