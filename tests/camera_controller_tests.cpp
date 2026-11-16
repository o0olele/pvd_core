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
void acceleration(){
  CameraNavigationPreferences prefs;CameraMovement input;
  input.forward=1;input.elapsed_seconds=.1;
  OrbitCamera camera;CameraMovementState state;
  const auto origin=camera.position();
  check(move_camera(camera,input,prefs,state),"accelerated camera move ignored");
  const auto first_step=length(origin,camera.position());
  check(std::abs(first_step-10*(.1+7*.1*.1/6))<2e-4,
        "initial acceleration did not integrate speed over the frame");
  for(int i=0;i<40;++i)move_camera(camera,input,prefs,state);
  check(std::abs(state.held_seconds-3)<1e-9,"held movement timer did not stop at its cap");
  const auto capped=camera.position();move_camera(camera,input,prefs,state);
  check(std::abs(length(capped,camera.position())-8)<2e-4,"held movement did not cap at eight times base speed");
  const auto stopped=camera.position();CameraMovement idle;idle.elapsed_seconds=.1;
  check(!move_camera(camera,idle,prefs,state),"idle input moved camera");
  near(camera.position(),stopped,"idle movement changed position");
  check(state.held_seconds==0,"releasing movement did not reset acceleration");
  move_camera(camera,input,prefs,state);
  check(std::abs(length(stopped,camera.position())-first_step)<2e-4,"movement resumed at an accelerated speed");
  const auto before_invalid=camera.view_projection(1).values;
  const auto held_before_invalid=state.held_seconds;
  auto invalid=input;invalid.forward=2;
  rejects([&]{move_camera(camera,invalid,prefs,state);});
  check(state.held_seconds==held_before_invalid&&camera.view_projection(1).values==before_invalid,
        "invalid accelerated movement changed camera or timer");
  prefs.movement_speed=0;
  check(!move_camera(camera,input,prefs,state)&&state.held_seconds==0,"disabled movement retained acceleration");
  prefs.movement_speed=10;
  const auto travel=[&](int frames,double dt,bool boost){
    OrbitCamera c;CameraMovementState s;auto step=input;step.elapsed_seconds=dt;step.boost=boost;
    const auto start=c.position();for(int i=0;i<frames;++i)move_camera(c,step,prefs,s);
    return length(start,c.position());
  };
  const auto at20=travel(80,.05,false),at60=travel(240,1./60,false);
  check(std::abs(at20-215)<.002&&std::abs(at20-at60)<.002,
        "held movement acceleration depends on rendering frame rate");
  check(std::abs(travel(80,.05,true)-2*at20)<.003,"Shift did not double accelerated movement");
}
void widgets(float scale){
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
  UiScale fonts(ImGui::GetStyle());fonts.reload_fonts(scale);
  io.DisplaySize={1060*scale,900*scale};io.DeltaTime=.05f;
  Document doc;OrbitCamera c;CameraNavigationPreferences prefs;CameraPanelState state;CameraInputState input_state;
  bool hovered=true,moved=false;bool focus_editor=false;char text[32]{};
  const auto draw=[&]{ImGui::NewFrame();UiPixelScope pixels(scale);
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({420*scale,850*scale});
    ImGui::Begin("Camera",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    drawCameraControls(doc,c,prefs,state);
    if(focus_editor)ImGui::SetKeyboardFocusHere();
    ImGui::InputText("Editor",text,sizeof(text));
    moved=navigateCameraFromImGui(c,prefs,input_state,hovered);
    ImGui::End();ImGui::Render();};
  draw();draw();
  check(ImGui::GetDrawData()->TotalVtxCount>100,"camera controls did not render");
  const auto origin=c.position();io.AddKeyEvent(ImGuiKey_W,true);draw();
  const auto first_step=length(origin,c.position());
  check(moved&&first_step>.5&&first_step<.55,"actual ImGui W input did not begin moving at base speed");
  for(int i=0;i<20;++i)draw();
  auto old=c.position();draw();
  check(length(old,c.position())>2*first_step,"actual held ImGui W input did not accelerate");
  old=c.position();hovered=false;draw();near(c.position(),old,"unhovered viewport captured keyboard");
  check(input_state.movement.held_seconds==0,"unhovered viewport retained acceleration");
  hovered=true;io.AddKeyEvent(ImGuiMod_Ctrl,true);draw();near(c.position(),old,"Ctrl shortcut moved camera");
  io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddKeyEvent(ImGuiKey_W,false);draw();
  io.AddKeyEvent(ImGuiKey_W,true);draw();
  check(std::abs(length(old,c.position())-first_step)<2e-4,"keyboard gate did not reset acceleration");
  io.AddKeyEvent(ImGuiKey_W,false);draw();old=c.position();
  focus_editor=true;draw();focus_editor=false;draw();
  io.AddKeyEvent(ImGuiKey_W,true);io.AddInputCharacter('w');draw();
  check(io.WantTextInput&&!moved,"camera stole typing from actual active text editor");
  near(c.position(),old,"text editing moved the viewport");
  check(input_state.movement.held_seconds==0,"text editing retained movement acceleration");
  ImGui::DestroyContext();
}
void mouse(float scale){
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
  UiScale fonts(ImGui::GetStyle());fonts.reload_fonts(scale);
  io.DisplaySize={1060*scale,900*scale};io.DeltaTime=.05f;
  OrbitCamera camera;CameraNavigationPreferences prefs;bool active=true,rotated=false;
  const auto draw=[&]{ImGui::NewFrame();UiPixelScope pixels(scale);
    ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Viewport",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    rotated=rotateCameraFromImGui(camera,prefs,active,scale);
    ImGui::End();ImGui::Render();};
  io.AddMousePosEvent(400*scale,400*scale);draw();draw();
  for(auto mode:{CameraNavigationMode::orbit,CameraNavigationMode::first_person}){
    camera={};prefs.mode=mode;
    const auto eye=camera.position();const auto yaw=camera.yaw;
    io.AddMouseButtonEvent(ImGuiMouseButton_Right,true);draw();
    io.AddMousePosEvent(420*scale,392*scale);draw();
    check(rotated,"actual ImGui right drag did not rotate camera");
    near(camera.position(),eye,"right drag orbited instead of retaining camera position");
    check(std::abs(camera.yaw-yaw+.16)<1e-6,"right drag sensitivity changed with screen scale");
    const auto unchanged=camera.view_projection(1).values;
    active=false;io.AddMousePosEvent(430*scale,390*scale);draw();
    check(!rotated&&camera.view_projection(1).values==unchanged,"inactive viewport captured mouse look");
    io.AddMouseButtonEvent(ImGuiMouseButton_Right,false);draw();active=true;
    io.AddMousePosEvent(400*scale,400*scale);draw();
  }
  camera={};prefs.mode=CameraNavigationMode::first_person;
  const auto pivot=camera.target;const auto eye=camera.position();
  io.AddKeyEvent(ImGuiMod_Alt,true);draw();
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);draw();
  io.AddMousePosEvent(420*scale,392*scale);draw();
  check(rotated,"actual Alt-left drag did not orbit camera");
  near(camera.target,pivot,"Alt-left drag did not retain orbit pivot");
  check(length(camera.position(),eye)>.1,"Alt-left drag unexpectedly retained camera position");
  io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);draw();
  const auto alt_eye=camera.position();
  io.AddMouseButtonEvent(ImGuiMouseButton_Right,true);draw();
  io.AddMousePosEvent(440*scale,384*scale);draw();
  check(rotated,"Alt-right drag did not look around");
  near(camera.position(),alt_eye,"Alt changed right drag into orbit");
  ImGui::DestroyContext();
}
}
int main(){try{mathematics();acceleration();for(float scale:{1.f,1.5f,2.f}){widgets(scale);mouse(scale);}
  std::cout<<"Camera movement, eye/pivot, clipping and real ImGui input tests passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}}
