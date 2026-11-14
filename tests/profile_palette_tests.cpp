// Real ImGui color inputs and submitted timeline vertex colors, no mocks/GPU.
#include "analysis_panels.hpp"
#include "ui_scale.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
namespace {
using namespace pvd;
using namespace pvd::viewer;
void check(bool value,const char *message) {if(!value)throw std::runtime_error(message);}
struct Fixture {
  StreamHeader header;
  std::vector<Event> events;
  std::map<std::string,std::uint32_t> strings;
  void emit(Payload p) {events.push_back({1,events.size(),events.size()*4,std::move(p)});}
  std::uint32_t str(const std::string &text) {
    const auto at=strings.find(text);if(at!=strings.end())return at->second;
    const auto id=static_cast<std::uint32_t>(strings.size()+1);strings[text]=id;emit(StringHandleEvent{text,id});return id;
  }
  WireName name(const char *space,const char *text) {const auto a=str(space),b=str(text);return {a,b};}
  explicit Fixture(ByteOrder order) {
    header.byte_order=order;header.timestamp_numerator=1;header.timestamp_denominator=1;
    const auto cls=name("_debugger_","PxProfileZone"),dt=name("physx3","PvdU8");
    const auto property=str("events"),semantic=str("profile event stream");
    emit(CreateClass{cls});emit(CreateProperty{cls,property,semantic,dt,2,{}});
    const auto scene=name("physx3","PxScene");emit(CreateClass{scene});emit(CreateInstance{scene,1});const auto marker=str("frame");
    for(unsigned generation=0;generation<2;++generation) {
      if(generation)emit(DestroyInstance{10});
      emit(AddProfileZone{10,"Same zone"});emit(CreateInstance{cls,10});
      emit(AddProfileZoneEvent{10,"Work one",1,true});emit(AddProfileZoneEvent{10,"Work two",2,true});
      Bytes bytes;
      const auto u=[&](std::uint64_t value,unsigned width) {
        for(unsigned i=0;i<width;++i)bytes.push_back(static_cast<std::uint8_t>(value>>((order==ByteOrder::little?i:width-1-i)*8)));
      };
      for(unsigned event=1;event<=2-generation;++event)for(unsigned tag:{1u,2u}) {
        u(tag,1);u(15,1);u(event,2);u(7,4);u(99,8);u(0,1);u(0,1);
        u(1000+event*100+(tag==2?80:0),8);
      }
      emit(BeginSection{1,marker,events.size()});
      const auto count=static_cast<std::uint32_t>(bytes.size());
      emit(SetPropertyValue{10,property,dt,count,std::move(bytes)});emit(EndSection{1,marker,events.size()});
    }
  }
  void load(Document &doc) const {doc.adopt(std::make_unique<Session>(header,events),"Profile palette fixture");}
};
ImU32 packed(const PaletteRgba &color) {return ImGui::ColorConvertFloat4ToU32({color[0],color[1],color[2],color[3]});}
struct Harness {
  Document doc;AnalysisPanels panels;ImGuiWindow *window=nullptr;
  float scale,width;std::string exported,log;unsigned export_count=0;
  explicit Harness(float s,float w):scale(s),width(w*s) {}
  void frame(std::optional<ImVec2> mouse={},std::optional<bool> down={}) {
    UiPixelScope pixels(scale);auto &io=ImGui::GetIO();io.DisplaySize={width,1000*scale};io.DeltaTime=1.f/60;
    if(mouse)io.AddMousePosEvent(mouse->x,mouse->y);if(down)io.AddMouseButtonEvent(0,*down);
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Profile palette",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
    ImGui::LogToBuffer();panels.draw(doc,AnalysisPage::profile,[&](const char *,const auto &writer){std::ostringstream out;writer(out);exported=out.str();++export_count;});
    log=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();window=ImGui::GetCurrentWindow();
    const auto controls=panels.profile_controls();
    for(const auto &r:{controls.palette_header,controls.palette_by_event,controls.palette_rgba,controls.palette_restore})
      if(r.width>0)check(r.x>=window->Pos.x&&r.x+r.width<=window->InnerRect.Max.x+2*scale,"Profile palette controls fit available logical width");
    ImGui::End();ImGui::Render();
    check(log.find("Analysis error:")==std::string::npos,"Profile palette edit candidate validates");
  }
  enum class Control { header,by_event,rgba,restore,csv,zone };
  ProfileControlRect rect(Control value) const {
    const auto controls=panels.profile_controls();
    switch(value) {
      case Control::header:return controls.palette_header;
      case Control::by_event:return controls.palette_by_event;
      case Control::rgba:return controls.palette_rgba;
      case Control::restore:return controls.palette_restore;
      case Control::csv:return controls.csv;
      case Control::zone:return controls.zone_combo;
    }
    throw std::runtime_error("Unknown actual profiler control");
  }
  void reveal(Control value) {
    frame();for(unsigned i=0;i<8;++i) {
      const auto r=rect(value);check(r.width>0&&r.height>0,"actual palette control submitted");
      const auto y=r.y+r.height*.5f;
      if(y>=window->InnerClipRect.Min.y&&y<window->InnerClipRect.Max.y)return;
      ImGui::SetScrollY(window,std::max(0.f,window->Scroll.y+y-(window->InnerClipRect.Min.y+window->InnerClipRect.Max.y)*.5f));frame();frame();
    }
    throw std::runtime_error("Cannot reveal actual profiler palette control");
  }
  void click(ImVec2 p) {frame(p,false);frame(p,true);frame(p,false);frame(p,false);}
  void click(Control value) {reveal(value);const auto r=rect(value);click({r.x+r.width*.5f,r.y+r.height*.5f});}
  void choose_zone(unsigned row) {
    click(Control::zone);frame();const auto &stack=ImGui::GetCurrentContext()->OpenPopupStack;
    check(stack.Size&&stack.back().Window,"actual source generation combo opens");const auto p=stack.back().Window->DC.CursorStartPos;
    click({p.x+10*scale,p.y+ImGui::GetTextLineHeight()*.5f+row*ImGui::GetTextLineHeightWithSpacing()});
  }
  void drag_component(unsigned component,float amount) {
    reveal(Control::rgba);const auto r=rect(Control::rgba);const auto &style=ImGui::GetStyle();
    const auto inputs=r.width-ImGui::GetFrameHeight()-style.ItemInnerSpacing.x;
    const auto field=std::floor((inputs-style.ItemInnerSpacing.x*3)/4);
    const ImVec2 start{r.x+component*(field+style.ItemInnerSpacing.x)+field*.5f,r.y+r.height*.5f};
    frame(start,false);frame(start,true);frame(ImVec2{start.x+amount*scale,start.y},true);
    frame(ImVec2{start.x+(amount+amount*.25f)*scale,start.y},true);frame(ImVec2{start.x+(amount+amount*.25f)*scale,start.y},false);frame();
  }
  std::size_t bar_vertices(ImU32 color) const {
    const auto view=panels.profile_timeline_view();std::size_t count=0;
    for(const auto *child:ImGui::GetCurrentContext()->Windows) {
      if(child->ParentWindow!=window||!std::strstr(child->Name,"thread-timeline"))continue;
      // Only the actual timeline child. Editor preview/picker vertices cannot
      // satisfy this evidence check, even when they use the same RGBA color.
      for(const auto &v:child->DrawList->VtxBuffer)
        if(v.col==color&&v.pos.x>=view.left&&v.pos.x<=view.left+view.width&&
            v.pos.y>=view.top+25*scale&&v.pos.y<=view.top+view.height)++count;
    }
    return count;
  }
  void automatic_bars() const {
    for(unsigned event:{1u,2u}) {
      const auto original=ImColor::HSV(float((event*37u)%255u)/255.f,.50f,.68f);
      check(bar_vertices(ImGui::ColorConvertFloat4ToU32(original.Value))>=4,"actual default timeline fill preserves original event-ID HSV vertices");
    }
  }
};
void same_objects(const ColorPalette &a,const ColorPalette &b) {
  for(const auto role:color_palette_roles)if(role!=PaletteRole::profile_bar)
    check(palette_color(a,role)==palette_color(b,role),"Profile edit/restore preserves every object palette role");
}
void run(ByteOrder order,float scale,float logical_width) {
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=io.LogFilename=nullptr;
  UiScale fonts(ImGui::GetStyle());fonts.reload_fonts(scale);Fixture source(order);Harness h(scale,logical_width);source.load(h.doc);
  auto view=h.doc.view_options();view.palette.active={.12f,.34f,.56f,.78f};view.palette.cloth={.7f,.2f,.4f,.6f};h.doc.set_view_options(view);
  const auto objects=h.doc.view_options().palette;
  std::vector<Bytes> raw;for(const auto &packet:h.doc.session()->channels())raw.push_back(packet.value.raw_bytes());
  const auto count=h.doc.session()->event_count();h.frame();h.frame();h.automatic_bars();h.choose_zone(1);
  check(h.panels.visible_row_count(AnalysisPage::profile)==2,"source generation filter baseline");
  h.click(Harness::Control::csv);const auto original_csv=h.exported;
  h.click(Harness::Control::header);check(h.rect(Harness::Control::rgba).width>0,"actual Profile colors section opens");
  const auto original=h.doc.view_options().palette.profile_bar;
  h.drag_component(0,30);
  auto custom=h.doc.view_options().palette;
  check(!custom.profile_by_event&&custom.profile_bar!=original&&custom.profile_bar[0]!=original[0],"actual RGBA drag selects custom bars");
  h.drag_component(3,-35);custom=h.doc.view_options().palette;
  check(custom.profile_bar[3]>0&&custom.profile_bar[3]<1&&valid_color_palette(custom),"actual alpha field edits custom RGBA");
  same_objects(custom,objects);h.frame();h.frame();
  check(h.bar_vertices(packed(custom.profile_bar))>=8,"actual timeline vertices contain the edited custom RGBA including alpha");
  const auto legacy=ImColor::HSV(float(37u)/255.f,.50f,.68f);
  check(h.bar_vertices(ImGui::ColorConvertFloat4ToU32(legacy.Value))==0,"custom mode replaces the old event fill, not only the preview swatch");
  h.click(Harness::Control::csv);check(h.exported==original_csv&&h.panels.visible_row_count(AnalysisPage::profile)==2,"palette does not alter CSV or source generation filter");
  h.click(Harness::Control::by_event);check(h.doc.view_options().palette.profile_by_event&&h.doc.view_options().palette.profile_bar==custom.profile_bar,"event-ID mode preserves custom RGBA for later reuse");h.automatic_bars();
  h.click(Harness::Control::by_event);h.frame();check(h.bar_vertices(packed(custom.profile_bar))>=8,"actual uniform mode reuses retained custom bar color");
  // One panel can bind a different Session without carrying a color override.
  Document other;source.load(other);auto other_view=other.view_options();other_view.palette.profile_by_event=false;other_view.palette.profile_bar={.91f,.17f,.72f,.42f};other.set_view_options(other_view);
  std::swap(h.doc,other);h.frame();h.frame();check(h.bar_vertices(packed(other_view.palette.profile_bar))>=8,"source/document switch reads that document's own custom palette");
  std::swap(h.doc,other);h.frame();h.frame();check(h.bar_vertices(packed(custom.profile_bar))>=8,"switching back restores original document palette without global state");
  h.click(Harness::Control::restore);const auto defaults=default_color_palette();const auto restored=h.doc.view_options().palette;
  check(restored.profile_by_event&&restored.profile_bar==defaults.profile_bar,"actual Restore Profile colors resets only the two Profile fields");same_objects(restored,objects);h.automatic_bars();
  check(h.doc.session()->event_count()==count,"Profile display edit does not change raw event prefix");
  std::size_t packet=0;for(const auto &channel:h.doc.session()->channels())check(channel.value.raw_bytes()==raw.at(packet++),"Profile display edit preserves raw channel bytes");
  check(packet==raw.size()&&h.doc.frame_index()==0,"Profile display edit does not seek or change captured channels");
  ImGui::DestroyContext();
}
}
int main() {
  try {
    for(const auto order:{ByteOrder::little,ByteOrder::big})for(float scale:{1.f,1.5f,2.f})for(float width:{420.f,1060.f})run(order,scale,width);
    std::cout<<"Actual Profile palette RGBA/alpha/modes/restore, timeline vertex colors, CSV/generation/raw isolation: LE/BE, DPI 1/1.5/2, width 420/1060 passed\n";return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}
