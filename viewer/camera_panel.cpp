#include "camera_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include <algorithm>
#include <exception>
#include <stdexcept>
namespace pvd::viewer {
bool navigateCameraFromImGui(OrbitCamera &camera,
                            const CameraNavigationPreferences &prefs,bool hovered) {
  const auto &io=ImGui::GetIO();
  if(!hovered||io.WantTextInput||io.KeyCtrl||io.KeyAlt||io.KeySuper||
      ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))return false;
  const auto axis=[](ImGuiKey positive,ImGuiKey negative) {
    return double(ImGui::IsKeyDown(positive))-double(ImGui::IsKeyDown(negative));
  };
  CameraMovement input;
  input.forward=axis(ImGuiKey_W,ImGuiKey_S);
  input.right=axis(ImGuiKey_D,ImGuiKey_A);
  input.up=axis(ImGuiKey_E,ImGuiKey_Q);
  input.boost=io.KeyShift;
  input.elapsed_seconds=std::clamp(double(io.DeltaTime),0.,.1);
  return move_camera(camera,input,prefs);
}
bool drawCameraControls(const Document &doc,OrbitCamera &camera,
                         CameraNavigationPreferences &prefs,CameraPanelState &state) {
  if(!state.initialized||state.source!=doc.session()) {
    state={};state.source=doc.session();state.initialized=true;
  }
  if(!state.target_dirty)state.target=camera.target;
  bool changed=false;
  const auto act=[&](const auto &fn) {
    try{fn();state.error.clear();changed=true;}
    catch(const std::exception &e){state.error=e.what();}
  };
  ImGui::TextUnformatted("CAMERA NAVIGATION");
  int mode=static_cast<int>(prefs.mode);
  ImGui::SetNextItemWidth(ui_pixels(180));
  if(ImGui::Combo("Controller",&mode,"Orbit (pivot)\0First person (eye)\0"))
    prefs.mode=static_cast<CameraNavigationMode>(mode);
  ImGui::SetNextItemWidth(ui_pixels(180));
  auto speed=prefs.movement_speed;
  if(ImGui::InputDouble("Speed (units/s)",&speed,1,10,"%.6g")) {
    const auto old=prefs.movement_speed;prefs.movement_speed=speed;
    if(!valid_camera_navigation(prefs)) {
      prefs.movement_speed=old;state.error="Camera speed must be finite in [0,1000000].";
    }else state.error.clear();
  }
  ImGui::SetNextItemWidth(ui_pixels(180));
  auto sensitivity=prefs.look_sensitivity;
  if(ImGui::InputDouble("Look (rad/pixel)",&sensitivity,.001,.01,"%.6g")) {
    const auto old=prefs.look_sensitivity;prefs.look_sensitivity=sensitivity;
    if(!valid_camera_navigation(prefs)) {
      prefs.look_sensitivity=old;state.error="Look sensitivity must be finite in [0.000001,0.1].";
    }else state.error.clear();
  }
  ImGui::TextWrapped("Hover the viewport: WASD moves, Q/E moves along display up, "
                     "Shift is 2x speed. Right-drag looks around the eye in first-person mode "
                     "or rotates around the pivot in orbit mode.");
  ImGui::SetNextItemWidth(ui_pixels(240));
  if(ImGui::InputFloat3("Look at point",&state.target.x,"%.6g"))state.target_dirty=true;
  if(ImGui::Button("Apply look-at"))act([&] {
    point_camera_at(camera,state.target);state.target_dirty=false;
  });
  ImGui::SameLine();
  if(ImGui::Button("Use current target"))state.target_dirty=false;
  ImGui::BeginDisabled(!doc.loaded());
  if(ImGui::Button("Center on visible"))act([&] {
    auto fitted=camera;
    if(!doc.fit_scene(fitted))throw std::runtime_error("No visible scene bounds");
    point_camera_at(camera,fitted.target);
  });
  ImGui::SameLine();
  if(ImGui::Button("Center on selected"))act([&] {
    auto fitted=camera;
    if(!doc.fit_selected(fitted))throw std::runtime_error("Selection has no visible bounds");
    point_camera_at(camera,fitted.target);
  });
  ImGui::EndDisabled();
  auto near_plane=camera.near_plane,far_plane=camera.far_plane;
  double fov=camera.vertical_fov*180/3.14159265358979323846;
  ImGui::SetNextItemWidth(ui_pixels(180));
  if(ImGui::InputDouble("Near clip",&near_plane,0,0,"%.8g"))
    act([&]{set_camera_projection(camera,near_plane,far_plane,camera.vertical_fov);});
  ImGui::SetNextItemWidth(ui_pixels(180));
  if(ImGui::InputDouble("Far clip",&far_plane,0,0,"%.8g"))
    act([&]{set_camera_projection(camera,camera.near_plane,far_plane,camera.vertical_fov);});
  ImGui::SetNextItemWidth(ui_pixels(180));
  if(ImGui::InputDouble("Vertical FOV (deg)",&fov,1,5,"%.5g"))
    act([&]{set_camera_projection(camera,camera.near_plane,camera.far_plane,
                                 fov*3.14159265358979323846/180);});
  ImGui::TextWrapped("Projection settings affect this view only. Fit and wheel zoom "
                     "recompute clipping for their new scene distance.");
  if(!state.error.empty())ImGui::TextWrapped("Camera: %s",state.error.c_str());
  return changed;
}
} // namespace pvd::viewer
