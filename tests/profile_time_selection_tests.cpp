#include "analysis_panels.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
constexpr std::uint64_t base=(std::uint64_t(1)<<60)+3;
void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string,std::uint32_t> strings;
  void emit(Payload value){events.push_back({1,events.size(),events.size(),std::move(value)});}
  std::uint32_t str(const std::string &text) {
    if(strings.count(text))return strings.at(text);
    const auto id=static_cast<std::uint32_t>(strings.size()+1);
    strings[text]=id;emit(StringHandleEvent{text,id});return id;
  }
  WireName name(const char *space,const char *text){const auto a=str(space),b=str(text);return {a,b};}
  explicit Fixture(ByteOrder order,bool clock) {
    header.byte_order=order;header.timestamp_numerator=clock?10:0;header.timestamp_denominator=2;
    const auto cls=name("_debugger_","PxProfileZone");emit(CreateClass{cls});
    const auto prop=str("events"),semantic=str("profile event stream");
    const auto dt=name("physx3","PvdU8");emit(CreateProperty{cls,prop,semantic,dt,2,{}});
    emit(AddProfileZone{10,"Measurement"});emit(CreateInstance{cls,10});
    emit(AddProfileZoneEvent{10,"Native workload",1,true});
    Bytes wire;
    const auto u=[&](std::uint64_t value,unsigned width) {
      for(unsigned i=0;i<width;++i)wire.push_back(static_cast<std::uint8_t>(value>>((order==ByteOrder::little?i:width-1-i)*8)));
    };
    const auto endpoint=[&](unsigned tag,std::uint64_t tick,std::uint32_t thread) {
      u(tag,1);u(15,1);u(1,2);u(thread,4);u(99,8);u(0,1);u(0,1);u(tick,8);
    };
    endpoint(1,base,7);endpoint(2,base+2048,7);
    endpoint(1,base+2048,8);endpoint(2,base+6144,8);
    endpoint(1,base+6144,7);endpoint(2,base+8192,7);
    const auto scene=name("physx3","PxScene");emit(CreateClass{scene});emit(CreateInstance{scene,1});
    const auto frame=str("frame");emit(BeginSection{1,frame,base});
    emit(SetPropertyValue{10,prop,dt,static_cast<std::uint32_t>(wire.size()),wire});
    emit(EndSection{1,frame,base+8192});
  }
};
std::string draw(AnalysisPanels &panels,Document &doc,float width,float scale) {
  UiPixelScope pixels(scale);
  auto &io=ImGui::GetIO();io.DisplaySize={std::max(1440.f,width),1000*scale};io.DeltaTime=1.f/60;
  ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({width,800*scale});
  ImGui::Begin("Time selection",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove);
  ImGui::LogToBuffer();panels.draw(doc,AnalysisPage::profile,[](const auto*,const auto&){});
  const std::string text=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();
  ImGui::End();ImGui::Render();return text;
}
void run(ByteOrder order,bool clock,float scale,float logical_width) {
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;
  io.Fonts->AddFontDefault();unsigned char *data;int w,h;io.Fonts->GetTexDataAsRGBA32(&data,&w,&h);
  Fixture source(order,clock);Document doc;doc.adopt(std::make_unique<Session>(source.header,source.events));
  const auto event_count=doc.session()->event_count();
  AnalysisPanels panels;const auto width=logical_width*scale;
  draw(panels,doc,width,scale);draw(panels,doc,width,scale);
  const auto view=panels.profile_timeline_view();
  check(view.width>20&&view.end_ticks-view.begin_ticks==8192,"actual native high-bit timeline extent");
  // ImGui consumes integer physical mouse pixels. Derive the independent
  // expected native tick from that exact pixel ratio, not absolute doubles.
  const auto offset=[&](float fraction) {
    const auto local=static_cast<std::uint64_t>(std::floor(view.left+view.width*fraction))-
                     static_cast<std::uint64_t>(view.left);
    return local*8192/static_cast<std::uint64_t>(view.width);
  };
  const auto expected_begin=base+offset(.25f),expected_end=base+offset(.75f);
  const auto point=[&](float fraction,float row){io.AddMousePosEvent(view.left+view.width*fraction,view.top+view.height*row);};
  point(.25f,.25f);io.AddKeyEvent(ImGuiMod_Shift,true);draw(panels,doc,width,scale);
  io.AddMouseButtonEvent(0,true);draw(panels,doc,width,scale);
  point(.75f,.75f);draw(panels,doc,width,scale);io.AddMouseButtonEvent(0,false);draw(panels,doc,width,scale);
  io.AddKeyEvent(ImGuiMod_Shift,false);const auto text=draw(panels,doc,width,scale);
  const auto selection=panels.profile_time_selection();
  if(!selection||selection->begin_ticks!=expected_begin||selection->end_ticks!=expected_end)
    std::cerr<<"range debug width="<<logical_width<<" scale="<<scale<<" clock="<<clock
             <<" graph="<<view.left<<","<<view.width<<" expected="<<expected_begin<<".."<<expected_end
             <<" actual="<<(selection?std::to_string(selection->begin_ticks)+".."+std::to_string(selection->end_ticks):"none")<<'\n';
  check(selection&&selection->begin_ticks==expected_begin&&selection->end_ticks==expected_end&&selection->duration_ticks==4096,
        "Shift drag across lanes preserves exact uint64 endpoints and delta");
  check(bool(selection->duration_nanoseconds)==clock,"missing clock is unavailable, not zero time");
  if(clock)check(*selection->duration_nanoseconds==20480,"SDK counter ratio converts only the exact delta");
  check(text.find("4096 ticks")!=std::string::npos,"visible arbitrary range duration");
  point(.75f,.85f);io.AddKeyEvent(ImGuiMod_Shift,true);draw(panels,doc,width,scale);
  io.AddMouseButtonEvent(0,true);draw(panels,doc,width,scale);point(.25f,.9f);draw(panels,doc,width,scale);
  io.AddMouseButtonEvent(0,false);draw(panels,doc,width,scale);io.AddKeyEvent(ImGuiMod_Shift,false);draw(panels,doc,width,scale);
  check(panels.profile_time_selection()->begin_ticks==expected_begin&&panels.profile_time_selection()->duration_ticks==4096,
        "right-to-left empty-space range selection");
  check(doc.session()->event_count()==event_count&&doc.frame_index()==0,"measurement never seeks or edits source data");
  Document replacement;replacement.adopt(std::make_unique<Session>(source.header,source.events));
  draw(panels,replacement,width,scale);check(!panels.profile_time_selection(),"source replacement clears measurement");
  ImGui::DestroyContext();
}
}
int main() {
  try {
    for(auto order:{ByteOrder::little,ByteOrder::big})for(bool clock:{true,false})
      for(float scale:{1.f,1.5f,2.f})for(float width:{420.f,1060.f})run(order,clock,scale,width);
    std::cout<<"Profiler actual Shift drag: LE/BE, high uint64, two lanes, blank space, reverse, clock and source reset passed\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}
