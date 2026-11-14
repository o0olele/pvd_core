#include "camera_panel.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace pvd;
using namespace pvd::viewer;
namespace {
void check(bool b,const char *m){if(!b)throw std::runtime_error(m);}
double length(Vec3 a,Vec3 b){return std::sqrt(double(a.x-b.x)*(a.x-b.x)+double(a.y-b.y)*(a.y-b.y)+double(a.z-b.z)*(a.z-b.z));}
void near(Vec3 a,Vec3 b,const char *m){check(length(a,b)<2e-4,m);}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"invalid camera operation accepted");}
void mathematics(){
  for(int up=0;up<6;++up)for(auto hand:{DisplayHandedness::right_handed,DisplayHandedness::left_handed})
    for(auto mode:{CameraNavigationMode::orbit,CameraNavigationMode::first_person}){
      OrbitCamera c;set_display_up(c,static_cast<DisplayUp>(up));c.handedness=hand;c.target={3,4,5};
      CameraNavigationPreferences p;p.mode=mode;p.movement_speed=12;
      const auto eye=c.position(),pivot=c.target;
      check(rotate_camera(c,22,-13,p),"camera look ignored");
      if(mode==CameraNavigationMode::first_person)near(c.position(),eye,"first-person look moved eye");
      else near(c.target,pivot,"orbit look moved pivot");
      const auto before=c.position();
      CameraMovement input;input.forward=1;input.elapsed_seconds=.25;
      check(move_camera(c,input,p),"forward camera move ignored");
      check(std::abs(length(before,c.position())-3)<2e-4,"movement speed is not world units per second");
      auto boosted=c;input.boost=true;const auto boosted_start=boosted.position();move_camera(boosted,input,p);
      check(std::abs(length(boosted_start,boosted.position())-6)<2e-4,"Shift did not double speed");
      auto diagonal=c;input.boost=false;input.right=1;const auto diagonal_start=diagonal.position();move_camera(diagonal,input,p);
      check(std::abs(length(diagonal_start,diagonal.position())-3)<2e-4,"diagonal movement was faster");
      input={};input.right=1;input.elapsed_seconds=.25;const auto lateral=c.target;
      const auto m=c.view_matrix();move_camera(c,input,p);
      const Vec3 expected{lateral.x+m.values[0]*3,lateral.y+m.values[4]*3,lateral.z+m.values[8]*3};
      near(c.target,expected,"screen-right movement disagrees with display handedness");
      auto stopped=c;p.movement_speed=0;check(!move_camera(stopped,input,p),"zero speed moved camera");
      near(stopped.target,c.target,"zero speed changed target");
    }
  OrbitCamera c;const auto before=c.view_projection(1).values;
  CameraNavigationPreferences p;CameraMovement input;input.forward=2;input.elapsed_seconds=.1;
  rejects([&]{move_camera(c,input,p);});
  rejects([&]{rotate_camera(c,std::numeric_limits<double>::infinity(),0,p);});
  rejects([&]{set_camera_projection(c,0,100,1);});
  rejects([&]{set_camera_projection(c,10,1,1);});
  rejects([&]{point_camera_at(c,c.position());});
  check(c.view_projection(1).values==before,"failed camera operation changed state");
  const auto eye=c.position();point_camera_at(c,{2,1,0});near(c.position(),eye,"look-at editor moved eye");
  near(c.target,{2,1,0},"look-at target not applied");
  set_camera_projection(c,.05,500,1.2);check(c.near_plane==.05&&c.far_plane==500&&c.vertical_fov==1.2,"projection fields not retained");
}
void widgets(float scale){
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
  UiScale fonts(ImGui::GetStyle());fonts.reload_fonts(scale);
  io.DisplaySize={1060*scale,900*scale};io.DeltaTime=.05f;
  Document doc;OrbitCamera c;CameraNavigationPreferences prefs;CameraPanelState state;
  bool hovered=true,moved=false;bool focus_editor=false;char text[32]{};
  const auto draw=[&]{ImGui::NewFrame();UiPixelScope pixels(scale);
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({420*scale,850*scale});
    ImGui::Begin("Camera",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    drawCameraControls(doc,c,prefs,state);
    if(focus_editor)ImGui::SetKeyboardFocusHere();
    ImGui::InputText("Editor",text,sizeof(text));
    moved=navigateCameraFromImGui(c,prefs,hovered);
    ImGui::End();ImGui::Render();};
  draw();draw();
  check(ImGui::GetDrawData()->TotalVtxCount>100,"camera controls did not render");
  const auto click=[&](ImVec2 position){io.AddMousePosEvent(position.x,position.y);draw();
    io.AddMouseButtonEvent(0,true);draw();io.AddMouseButtonEvent(0,false);draw();draw();};
  const auto *window=ImGui::FindWindowByName("Camera");
  const auto origin_ui=window->DC.CursorStartPos;
  click({origin_ui.x+30*scale,origin_ui.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetFrameHeight()*.5f});
  const auto *popup=ImGui::FindWindowByName("##Combo_00");
  check(popup&&ImGui::GetCurrentContext()->OpenPopupStack.Size,"actual camera controller menu did not open");
  click({popup->DC.CursorStartPos.x+30*scale,
         popup->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*.5f});
  check(prefs.mode==CameraNavigationMode::first_person,"actual controller selection was not applied");
  const auto origin=c.position();io.AddKeyEvent(ImGuiKey_W,true);draw();
  check(moved&&std::abs(length(origin,c.position())-.5)<2e-4,"actual ImGui W input did not move camera");
  auto old=c.position();hovered=false;draw();near(c.position(),old,"unhovered viewport captured keyboard");
  hovered=true;io.AddKeyEvent(ImGuiMod_Ctrl,true);draw();near(c.position(),old,"Ctrl shortcut moved camera");
  io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddKeyEvent(ImGuiKey_W,false);draw();
  focus_editor=true;draw();focus_editor=false;draw();
  io.AddKeyEvent(ImGuiKey_W,true);io.AddInputCharacter('w');draw();
  check(io.WantTextInput&&!moved,"camera stole typing from actual active text editor");
  near(c.position(),old,"text editing moved the viewport");
  ImGui::DestroyContext();
}
}
int main(){try{mathematics();for(float scale:{1.f,1.5f,2.f})widgets(scale);
  std::cout<<"Camera movement, eye/pivot, clipping and real ImGui input tests passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}}
