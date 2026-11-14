// Generation identity, actual ImGui controls and synchronous CSV callbacks.
#include "analysis_panels.hpp"
#include "ui_scale.hpp"
#include "imgui_internal.h"
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
namespace {
using namespace pvd;
using namespace pvd::viewer;
constexpr std::uint64_t base=(std::uint64_t(1)<<60)+3;
void check(bool ok,const char *message) {if(!ok)throw std::runtime_error(message);}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string,std::uint32_t> strings;
  std::size_t first_prefix=0;
  void emit(Payload p) {events.push_back({1,events.size(),events.size()*4,std::move(p)});}
  std::uint32_t str(const std::string &text) {
    const auto at=strings.find(text);if(at!=strings.end())return at->second;
    const auto id=static_cast<std::uint32_t>(strings.size()+1);strings[text]=id;emit(StringHandleEvent{text,id});return id;
  }
  WireName name(const char *space,const char *text) {const auto a=str(space),b=str(text);return {a,b};}
  explicit Fixture(ByteOrder order) {
    header.byte_order=order;header.timestamp_numerator=10;header.timestamp_denominator=2;
    const auto cls=name("_debugger_","PxProfileZone"),dt=name("physx3","PvdU8");
    const auto property=str("events"),semantic=str("profile event stream");
    emit(CreateClass{cls});emit(CreateProperty{cls,property,semantic,dt,2,{}});
    const auto scene=name("physx3","PxScene");emit(CreateClass{scene});emit(CreateInstance{scene,1});
    const auto frame=str("frame");
    for(unsigned generation=0;generation<2;++generation) {
      if(generation)emit(DestroyInstance{10});
      emit(AddProfileZone{10,"Repeated zone"});emit(CreateInstance{cls,10});
      for(unsigned id=1;id<=3;++id)emit(AddProfileZoneEvent{10,"Work"+std::to_string(id),static_cast<std::uint16_t>(id),true});
      Bytes bytes;
      const auto u=[&](std::uint64_t v,unsigned width) {
        for(unsigned i=0;i<width;++i)bytes.push_back(static_cast<std::uint8_t>(v>>((order==ByteOrder::little?i:width-1-i)*8)));
      };
      const auto interval=[&](unsigned id,std::uint64_t begin,std::uint64_t end) {
        for(unsigned tag:{1u,2u}) {u(tag,1);u(15,1);u(id,2);u(7,4);u(99,8);u(0,1);u(0,1);u(tag==1?begin:end,8);}
      };
      if(!generation) {
        interval(1,base,base+1024); // Outside the measured receipt-time window.
        interval(2,base,base+8192); // Overlaps range: full duration retained.
        interval(3,base+7168,base+8192); // Outside the measured window.
      } else {
        interval(1,base+2048,base+6144);
        interval(2,base+1024,base+7168);
      }
      emit(BeginSection{1,frame,events.size()});
      emit(SetPropertyValue{10,property,dt,static_cast<std::uint32_t>(bytes.size()),std::move(bytes)});
      emit(EndSection{1,frame,events.size()});
      if(!generation)first_prefix=events.size();
    }
  }
  std::vector<Event> boundary_packet(std::uint64_t begin,std::uint64_t end) {
    Bytes bytes;
    const auto u=[&](std::uint64_t value,unsigned width) {
      for(unsigned i=0;i<width;++i)bytes.push_back(static_cast<std::uint8_t>(value>>((header.byte_order==ByteOrder::little?i:width-1-i)*8)));
    };
    const auto interval=[&](unsigned id,std::uint64_t a,std::uint64_t b) {
      for(unsigned tag:{1u,2u}) {u(tag,1);u(15,1);u(id,2);u(7,4);u(99,8);u(0,1);u(0,1);u(tag==1?a:b,8);}
    };
    interval(1,begin-100,begin);interval(2,end,end+100);
    interval(3,begin,end);
    const auto count=static_cast<std::uint32_t>(bytes.size());
    emit(SetPropertyValue{10,strings.at("events"),{strings.at("physx3"),strings.at("PvdU8")},count,std::move(bytes)});
    return {events.back()};
  }
};
std::vector<std::vector<std::string>> csv(const std::string &text) {
  std::vector<std::vector<std::string>> result;std::istringstream input(text);std::string line;
  check(bool(std::getline(input,line))&&line.find("zone_generation")!=std::string::npos,"actual profile CSV header");
  while(std::getline(input,line)) {
    std::vector<std::string> row;std::size_t begin=0;
    for(;;) {const auto end=line.find(',',begin);row.push_back(line.substr(begin,end-begin));if(end==std::string::npos)break;begin=end+1;}
    check(row.size()==16,"actual profile CSV columns");result.push_back(std::move(row));
  }
  return result;
}
struct Harness {
  AnalysisPanels panels;Document doc;ImGuiWindow *window=nullptr;
  float scale,width;std::string exported,export_name,logged;unsigned exports=0;
  Harness(float s,float w):scale(s),width(w*s) {}
  void frame(std::optional<ImVec2> mouse={},std::optional<bool> down={}) {
    UiPixelScope pixels(scale);auto &io=ImGui::GetIO();io.DisplaySize={width,900*scale};io.DeltaTime=1.f/60;
    if(mouse)io.AddMousePosEvent(mouse->x,mouse->y);if(down)io.AddMouseButtonEvent(0,*down);
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Zone lifetime filter",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
    ImGui::LogToBuffer();panels.draw(doc,AnalysisPage::profile,[&](const char *name,const auto &writer) {
      std::ostringstream out;writer(out);exported=out.str();export_name=name;++exports;
    });
    logged=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();window=ImGui::GetCurrentWindow();
    ImGui::End();ImGui::Render();
  }
  ImVec2 focus(const char *label) {
    frame();
    for(unsigned i=0;i<8;++i) {
      const auto controls=panels.profile_controls();
      const auto rect=std::string(label)=="##zone"?controls.zone_combo:
          std::string(label)=="Export CSV..."?controls.csv:controls.measured_csv;
      check(rect.width>0&&rect.height>0,"actual requested profiler control is submitted");
      const ImVec2 center{rect.x+rect.width*.5f,rect.y+rect.height*.5f};
      if(center.y>=window->InnerClipRect.Min.y&&center.y<window->InnerClipRect.Max.y&&
          center.x>=window->InnerClipRect.Min.x&&center.x<window->InnerClipRect.Max.x)return center;
      // Position the actual ImGui scroll view before the mouse action. No
      // synthetic button activation or direct filter mutation is used.
      ImGui::SetScrollY(window,std::max(0.f,window->Scroll.y+center.y-
          (window->InnerClipRect.Min.y+window->InnerClipRect.Max.y)*.5f));
      frame();frame();
    }
    throw std::runtime_error(std::string("Cannot reveal actual profiler control: ")+label);
  }
  void click(ImVec2 p) {frame(p,false);frame(p,true);frame(p,false);frame(p,false);}
  void click(const char *label) {click(focus(label));}
  void choose(unsigned row) {
    click("##zone");frame();const auto &stack=ImGui::GetCurrentContext()->OpenPopupStack;
    check(stack.Size&&stack.back().Window,"actual zone combo opened");
    const auto *popup=stack.back().Window;
    check(popup->DC.CursorMaxPos.x<=ImGui::GetIO().DisplaySize.x+2*scale,"zone lifetime popup fits screen");
    const auto p=popup->DC.CursorStartPos;
    click({p.x+10*scale,p.y+ImGui::GetTextLineHeight()*.5f+row*ImGui::GetTextLineHeightWithSpacing()});
  }
  void measure() {
    // Restore top scroll before using the real canvas coordinates.
    ImGui::SetScrollY(window,0);frame();frame();
    const auto view=panels.profile_timeline_view();auto &io=ImGui::GetIO();
    check(view.end_ticks>view.begin_ticks&&view.begin_ticks>(std::uint64_t(1)<<53),"native high-u64 measurement extent");
    const auto point=[&](const ProfileTimelineView &at,float fraction) {
      const auto x=static_cast<std::uint64_t>(at.left+at.width*fraction);
      io.AddMousePosEvent(static_cast<float>(x),at.top+at.height*.55f);
      const auto local=x-static_cast<std::uint64_t>(at.left);
      return at.begin_ticks+local*(at.end_ticks-at.begin_ticks)/static_cast<std::uint64_t>(at.width);
    };
    const auto expected_begin=point(view,.25f);io.AddKeyEvent(ImGuiMod_Shift,true);frame();io.AddMouseButtonEvent(0,true);frame();frame();frame();
    // New footer text can admit a vertical scrollbar. Use the newly laid-out
    // actual canvas, as a user sees it, for the final drag point.
    const auto expected_end=point(panels.profile_timeline_view(),.75f);frame();io.AddMouseButtonEvent(0,false);frame();io.AddKeyEvent(ImGuiMod_Shift,false);frame();
    const auto selection=panels.profile_time_selection();
    check(selection&&selection->begin_ticks>=expected_begin-1&&selection->begin_ticks<=expected_begin+1&&selection->end_ticks>=expected_end-1&&selection->end_ticks<=expected_end+1,"actual high-u64 measured endpoints follow physical pixels without rounding absolute ticks");
  }
};
void run(ByteOrder order,float scale,float width) {
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
  UiScale fonts(ImGui::GetStyle());fonts.reload_fonts(scale);Fixture source(order);Harness h(scale,width);
  h.doc.adopt(std::make_unique<Session>(source.header,source.events));h.frame();h.frame();
  check(h.panels.visible_row_count(AnalysisPage::profile)==5,"both source zone generations indexed independently");
  h.choose(1);
  check(h.panels.visible_row_count(AnalysisPage::profile)==3&&h.logged.find("generation 0")!=std::string::npos,"old zone lifetime remains individually selectable");
  h.click("Export CSV...");auto rows=csv(h.exported);check(rows.size()==3,"actual export button uses generation filter");
  for(const auto &row:rows)check(row[0]=="10"&&row[1]=="0","old generation CSV source identity");
  h.measure();const auto before=h.exports;h.click("Export measured intervals CSV...");
  check(h.exports==before+1&&h.export_name=="profile-range.csv","actual measured export button invokes writer");
  rows=csv(h.exported);check(rows.size()==1&&rows[0][1]=="0"&&rows[0][3]=="2","measured CSV excludes other generation and intervals outside the selected window");
  check(std::stoull(rows[0][9])==base&&std::stoull(rows[0][10])==base+8192&&rows[0][11]=="8192","range export retains original full interval endpoints/duration");
  h.choose(2);check(h.panels.visible_row_count(AnalysisPage::profile)==2&&h.logged.find("generation 1")!=std::string::npos,"reused zone lifetime selected independently");
  h.click("Export measured intervals CSV...");rows=csv(h.exported);check(rows.size()==2,"measured filter follows selected zone generation");
  for(const auto &row:rows)check(row[1]=="1","new generation CSV never includes old zone");
  h.choose(0);check(h.panels.visible_row_count(AnalysisPage::profile)==5,"All zones resets both id and generation filters");
  h.click("Export CSV...");check(csv(h.exported).size()==5,"All zones CSV contains both generations");
  check(h.doc.frame_index()==0&&h.doc.session()->event_count()==source.events.size(),"filter/export never mutates or seeks source");
  // Incremental update must retain an explicitly selected old generation.
  h.panels.reset();h.doc.begin_live(source.header,"live reused profile zone");
  h.doc.append_live(std::vector<Event>(source.events.begin(),source.events.begin()+static_cast<std::ptrdiff_t>(source.first_prefix)));h.frame();h.frame();h.choose(1);
  h.doc.append_live(std::vector<Event>(source.events.begin()+static_cast<std::ptrdiff_t>(source.first_prefix),source.events.end()));h.frame();h.frame();
  check(h.panels.visible_row_count(AnalysisPage::profile)==3,"live zone reuse does not retarget existing filter");
  h.click("Export CSV...");for(const auto &row:csv(h.exported))check(row[1]=="0","live export retains selected generation");
  h.choose(2);h.measure();const auto boundary=*h.panels.profile_time_selection();
  h.doc.append_live(source.boundary_packet(boundary.begin_ticks,boundary.end_ticks));h.frame();h.frame();
  check(h.panels.visible_row_count(AnalysisPage::profile)==5,"live exact-boundary oracle intervals received");
  h.click("Export measured intervals CSV...");rows=csv(h.exported);check(rows.size()==3,"actual range CSV uses half-open overlap endpoints");
  unsigned exact_inside=0;
  for(const auto &row:rows) {
    const auto begin=std::stoull(row[9]),end=std::stoull(row[10]);
    check(row[1]=="1"&&end>boundary.begin_ticks&&begin<boundary.end_ticks,"actual CSV excludes intervals ending at begin or starting at end");
    exact_inside+=begin==boundary.begin_ticks&&end==boundary.end_ticks;
  }
  check(exact_inside==1,"interval exactly matching measured bounds is included");
  h.doc.finish_live();ImGui::DestroyContext();
}
}
int main() {
  try {
    for(const auto order:{ByteOrder::little,ByteOrder::big})for(float scale:{1.f,1.5f,2.f})for(float width:{420.f,1060.f})run(order,scale,width);
    std::cout<<"Actual profiler zone ID reuse/filter/CSV/range boundaries/live: LE/BE, DPI 1/1.5/2, width 420/1060 passed\n";return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}
