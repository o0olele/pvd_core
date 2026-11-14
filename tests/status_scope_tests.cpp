// Actual Session/Document extraction and Dear ImGui input; no SDK/GPU needed.
#include "scene_tools.hpp"
#include "ui_scale.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace pvd;
using namespace pvd::viewer;
namespace {
void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
Bytes words(std::initializer_list<std::uint64_t> values) {
  Bytes out;
  for (auto n : values) for (unsigned j=0;j<8;++j)
    out.push_back(static_cast<std::uint8_t>(n>>(j*8)));
  return out;
}
Bytes floats(std::initializer_list<float> values) {
  Bytes out;
  for (auto f : values) {
    std::uint32_t n; std::memcpy(&n,&f,4);
    for (unsigned j=0;j<4;++j) out.push_back(static_cast<std::uint8_t>(n>>(j*8)));
  }
  return out;
}
struct Fixture {
  std::vector<Event> events;
  std::vector<std::size_t> ends;
  std::map<std::string,std::uint32_t> strings;
  void emit(Payload p) { events.push_back({1,events.size(),events.size()*10,std::move(p)}); }
  std::uint32_t str(const std::string &s) {
    auto found=strings.find(s); if(found!=strings.end()) return found->second;
    const auto h=static_cast<std::uint32_t>(strings.size()+1); strings[s]=h;
    emit(StringHandleEvent{s,h}); return h;
  }
  WireName type(const char *s) { const auto ns=str("physx3"); return {ns,str(s)}; }
  void prop(const char *c,const char *p,const char *t,bool array=false) {
    const auto cls=type(c),dt=type(t); const auto h=str(p);
    emit(CreateProperty{cls,h,0,dt,static_cast<std::uint8_t>(array?2:1),{}});
  }
  void create(const char *c,std::uint64_t id) { emit(CreateInstance{type(c),id}); }
  void value(std::uint64_t id,const char *p,const char *t,Bytes b,std::uint32_t count=1) {
    const auto h=str(p),ns=str("physx3"),dt=str(t);
    emit(SetPropertyValue{id,h,{ns,dt},count,std::move(b)});
  }
  void refs(std::uint64_t id,const char *p,std::initializer_list<std::uint64_t> ids) {
    value(id,p,"VoidPtr",words(ids),static_cast<std::uint32_t>(ids.size()));
  }
  void pose(std::uint64_t id,const char *p="GlobalPose") {
    value(id,p,"PxTransform",floats({0,0,0,1,static_cast<float>(id),0,0}));
  }
  void actor(std::uint64_t id,std::initializer_list<std::uint64_t> shapes,
             std::optional<bool> sleep,std::uint64_t scene=1,const char *cls="PxRigidDynamic") {
    create(cls,id); refs(id,"Scene",{scene}); refs(id,"Shapes",shapes); pose(id);
    value(id,"RigidBodyFlags","PvdU8",Bytes{0});
    if(sleep) value(id,"IsSleeping","PvdBool",Bytes{static_cast<std::uint8_t>(*sleep)});
  }
  void mark(std::uint64_t scene=1) {
    const auto label=str("frame"); emit(BeginSection{scene,label,events.size()});
    emit(EndSection{scene,label,events.size()});
    if(scene==1) ends.push_back(events.size());
  }
  Fixture() {
    for(const auto *c:{"PxScene","PxRigidDynamic","PxArticulationLink","PxArticulation","PxAggregate","PxShape","PxBoxGeometry"}) emit(CreateClass{type(c)});
    for(const auto *c:{"PxRigidDynamic","PxArticulationLink"}) {
      prop(c,"Scene","ObjectRef"); prop(c,"Shapes","ObjectRef",true);
      prop(c,"GlobalPose","PxTransform"); prop(c,"IsSleeping","PvdBool");
      prop(c,"RigidBodyFlags","PvdU8");
    }
    prop("PxArticulationLink","Parent","ObjectRef");
    prop("PxArticulation","Scene","ObjectRef"); prop("PxArticulation","Links","ObjectRef",true);
    prop("PxArticulation","IsSleeping","PvdBool");
    prop("PxAggregate","Actors","ObjectRef",true); prop("PxAggregate","Articulations","ObjectRef",true);
    prop("PxShape","Geometry","ObjectRef"); prop("PxShape","LocalPose","PxTransform"); prop("PxShape","Flags","PvdU8");
    prop("PxBoxGeometry","HalfExtents","PxVec3");
    create("PxScene",1); create("PxScene",2); create("PxBoxGeometry",200);
    value(200,"HalfExtents","PxVec3",floats({.5f,.5f,.5f}));
    for(std::uint64_t id:{100,101,102}) {
      create("PxShape",id); refs(id,"Geometry",{200}); pose(id,"LocalPose");
      value(id,"Flags","PvdU8",Bytes{static_cast<std::uint8_t>(id==101?3:6)});
    }
    actor(10,{100,101},true); actor(11,{100},true); actor(12,{101},false);
    actor(13,{101},{}); actor(14,{102},false);
    create("PxArticulation",60); refs(60,"Scene",{1}); refs(60,"Links",{30});
    value(60,"IsSleeping","PvdBool",Bytes{1});
    actor(30,{101},{},0,"PxArticulationLink"); refs(30,"Parent",{60});
    actor(31,{100},{},0,"PxArticulationLink"); refs(31,"Parent",{30});
    create("PxAggregate",70); refs(70,"Actors",{11}); refs(70,"Articulations",{60});
    actor(21,{100},true,2); mark(2); mark();
    value(11,"IsSleeping","PvdBool",Bytes{0}); value(100,"Flags","PvdU8",Bytes{3}); mark();
    emit(DestroyInstance{10}); actor(10,{100,101},true);
    value(11,"IsSleeping","PvdBool",Bytes{1}); value(100,"Flags","PvdU8",Bytes{6}); mark();
  }
  static StreamHeader header() { StreamHeader h; h.stream_flags=1; return h; }
  void load(Document &doc) const {
    doc.adopt(std::make_unique<Session>(header(),events),"status scope fixture");
    check(doc.set_scene(1)&&doc.set_frame(0),"initial native frame");
  }
};
PickingKey key(const Document &doc,std::uint64_t actor,std::uint64_t shape=100) {
  for(const auto &s:doc.frame()->render.instances)
    if(s.actor_id==actor&&s.shape_id==shape) return s.picking_key();
  std::string message="expected attachment missing actor="+std::to_string(actor)+" shape="+std::to_string(shape)+" frame="+std::to_string(doc.frame()->frame_index);
  for(const auto &error:doc.frame()->errors) message+="; "+error;
  throw std::runtime_error(message);
}
std::size_t visible(const Document &doc) {
  return static_cast<std::size_t>(std::count_if(doc.frame()->draws.begin(),doc.frame()->draws.end(),[](const auto &d){return d.visible;}));
}
void model() {
  Fixture f; Document doc; f.load(doc);
  const auto a=key(doc,10),other=key(doc,10,101),b=key(doc,11);
  check(doc.frame()->draws.size()==8&&visible(doc)==8,"source scene extraction");
  auto options=doc.view_options(); options.sleeping_scope=VisualizationScope::selected;
  doc.set_view_options(options); check(visible(doc)==3,"no selected sleepers; awake and unknown remain");
  check(doc.select(a)&&visible(doc)==4&&doc.visible(a)&&!doc.visible(other)&&!doc.visible(b),"shared shape selection remains attachment-specific");
  const auto published=doc.frame(); const auto bytes=published->snapshot.instance(10)->property("GlobalPose")->raw_bytes();
  doc.select(b); check(visible(doc)==4&&!doc.visible(a)&&doc.visible(b)&&published->draws[0].visible,"reselection updates filters and preserves published frame");
  doc.inspect(10); check(visible(doc)==5&&doc.visible(a)&&doc.visible(other),"actor selection includes every owned attachment");
  doc.inspect(60); check(visible(doc)==5&&!doc.visible(a)&&doc.visible(key(doc,30,101))&&doc.visible(key(doc,31)),"articulation Parent-chain selection");
  doc.inspect(70); check(visible(doc)==6&&doc.visible(b),"aggregate actor and articulation members");
  doc.inspect(1); check(visible(doc)==8,"scene owner membership");
  doc.clear_selection(); check(visible(doc)==3,"clearing selection never uses stale draw flags");
  options.sleeping_scope=VisualizationScope::unselected; doc.set_view_options(options); doc.select(a);
  check(visible(doc)==7&&!doc.visible(a)&&doc.visible(other),"Unselected status selection");
  options.sleeping_scope=VisualizationScope::none; doc.set_view_options(options); check(visible(doc)==3,"None status selection");
  options.sleeping_scope=VisualizationScope::all; options.trigger_scope=VisualizationScope::selected;
  doc.set_view_options(options); check(visible(doc)==5&&doc.visible(a)&&!doc.visible(b)&&!doc.visible(key(doc,14,102)),"trigger selected retains non-triggers");
  doc.select(b); check(visible(doc)==5&&!doc.visible(a)&&doc.visible(b),"trigger reselect precise shared attachment");
  options.trigger_scope=VisualizationScope::unselected; doc.set_view_options(options);
  check(visible(doc)==7&&!doc.visible(b)&&doc.visible(a),"trigger Unselected");
  options.trigger_scope=VisualizationScope::none; doc.set_view_options(options); check(visible(doc)==4,"trigger None");
  options.trigger_scope=VisualizationScope::all; options.show_trigger=false; doc.set_view_options(options); check(visible(doc)==4,"legacy trigger master overrides All");
  options.show_trigger=true; options.show_sleeping=false; doc.set_view_options(options); check(visible(doc)==3,"legacy sleep master overrides All");
  doc.show_all(); options=doc.view_options(); options.sleeping_scope=VisualizationScope::selected; options.trigger_scope=VisualizationScope::selected;
  doc.set_view_options(options); doc.select(a); doc.set_visible(a,false); check(!doc.visible(a),"hide wins over scope");
  doc.set_visible(a,true); check(doc.visible(a),"unhide retains active scopes");
  doc.set_frame(1); check(doc.visible(a)&&doc.visible(b),"wake and trigger flag changes affect scope at current prefix");
  doc.set_frame(0); check(!doc.visible(b)&&doc.visible(a),"reverse seek recomputes status");
  doc.set_frame(2); check(!doc.selected_key()&&!doc.inspected_object()&&!doc.visible(key(doc,10)),"reused actor generation does not inherit selection");
  doc.set_frame(0); check(!doc.visible(a),"reverse seek does not resurrect retired selection");
  check(doc.frame()->snapshot.instance(10)->property("GlobalPose")->raw_bytes()==bytes,"view commands preserve source fields");
  doc.select(a); doc.isolate(a); check(visible(doc)==1&&doc.visible(a),"isolate restores category scopes without exposing other attachments");
  doc.show_all(); check(visible(doc)==8&&doc.view_options().sleeping_scope==VisualizationScope::all&&doc.view_options().trigger_scope==VisualizationScope::all,"Restore all resets status scopes");
  options=doc.view_options(); options.sleeping_scope=static_cast<VisualizationScope>(99);
  bool rejected=false; try {doc.set_view_options(options);} catch(const std::invalid_argument &) {rejected=true;}
  check(rejected&&doc.view_options().sleeping_scope==VisualizationScope::all,"invalid enum rejected before view mutation");
  doc.set_scene(2); options=doc.view_options(); options.sleeping_scope=VisualizationScope::selected; doc.set_view_options(options);
  check(visible(doc)==0&&doc.inspect(21)&&visible(doc)==1,"other scene membership isolated");
  std::cout<<"Dynamic sleep/trigger scopes, owners, lifecycle, COW and source immutability passed\n";
}
void live() {
  Fixture f; Document doc; doc.begin_live(Fixture::header(),"live status scopes");
  check(doc.append_live(std::vector<Event>(f.events.begin(),f.events.begin()+static_cast<std::ptrdiff_t>(f.ends[0]))),"live initial prefix");
  doc.set_scene(1); const auto a=key(doc,10),b=key(doc,11);
  auto options=doc.view_options(); options.sleeping_scope=VisualizationScope::selected; doc.set_view_options(options); doc.select(a);
  check(!doc.visible(b),"live sleeping unselected hidden");
  doc.append_live(std::vector<Event>(f.events.begin()+static_cast<std::ptrdiff_t>(f.ends[0]),f.events.begin()+static_cast<std::ptrdiff_t>(f.ends[1])));
  check(doc.visible(b)&&doc.selected_key()==std::optional<PickingKey>(a),"live wake updates current frame retaining valid selection");
  doc.append_live(std::vector<Event>(f.events.begin()+static_cast<std::ptrdiff_t>(f.ends[1]),f.events.end()));
  check(!doc.selected_key()&&!doc.visible(key(doc,10)),"live recycled owner generation clears selection");
  doc.finish_live();
}
struct Ui {
  Document doc; OrbitCamera camera; SceneToolState state; ImGuiWindow *window=nullptr; float scale;
  explicit Ui(float s):scale(s) {Fixture{}.load(doc);}
  void frame(std::optional<ImVec2> mouse={},std::optional<bool> down={}) {
    UiPixelScope pixels(scale); auto &io=ImGui::GetIO(); io.DisplaySize={420*scale,900*scale}; io.DeltaTime=1.f/60;
    if(mouse) io.AddMousePosEvent(mouse->x,mouse->y); if(down)io.AddMouseButtonEvent(0,*down);
    ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Status scopes",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
    drawSceneTools(doc,camera,state); window=ImGui::GetCurrentWindow();
    check(window->DC.CursorMaxPos.x<=window->InnerRect.Max.x+2*scale,"Scene tools fit logical narrow width");
    ImGui::End(); ImGui::Render(); check(state.error.empty(),"Scene tools action succeeded");
  }
  void key(ImGuiKey k) {ImGui::GetIO().AddKeyEvent(k,true);frame();ImGui::GetIO().AddKeyEvent(k,false);frame();}
  ImVec2 focus(const char *label) {
    frame();const auto id=window->GetID(label);
    for(unsigned i=0;i<120;++i) {
      auto &g=*ImGui::GetCurrentContext();
      if(g.NavWindow==window&&g.NavId==id) {frame();frame();return ImGui::WindowRectRelToAbs(window,window->NavRectRel[0]).GetCenter();}
      key(ImGuiKey_Tab);
    }
    throw std::runtime_error(std::string("Cannot navigate actual control: ")+label);
  }
  void click(ImVec2 p) {frame(p,false);frame(p,true);frame(p,false);frame(p,false);}
  void choose(const char *label,int row) {
    click(focus(label));frame(); const auto &stack=ImGui::GetCurrentContext()->OpenPopupStack;
    check(stack.Size&&stack.back().Window,"actual status combo opened");
    const auto p=stack.back().Window->DC.CursorStartPos;
    click({p.x+10*scale,p.y+ImGui::GetTextLineHeight()*.5f+row*ImGui::GetTextLineHeightWithSpacing()});
  }
};
void widgets(float scale) {
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
  UiScale fonts(ImGui::GetStyle());fonts.reload_fonts(scale);Ui ui(scale);ui.frame();ui.frame();
  ui.choose("Sleeping scope",2);ui.choose("Trigger scope",3);
  check(ui.doc.view_options().sleeping_scope==VisualizationScope::selected&&ui.doc.view_options().trigger_scope==VisualizationScope::unselected,"actual independent status scope controls");
  ui.doc.inspect(10);check(!ui.doc.visible(key(ui.doc,10))&&ui.doc.visible(key(ui.doc,10,101)),"actual UI options affect model visibility");
  ui.choose("Sleeping scope",0);ui.choose("Trigger scope",1);
  check(visible(ui.doc)==3,"actual None / All controls");
  ImGui::DestroyContext();std::cout<<"Actual status scope controls DPI "<<scale<<" passed\n";
}
void real(const char *path) {
  Document doc;doc.open_file(path);check(doc.loaded(),"real SDK source opened");
  std::size_t checks=0;
  for(const auto &scene:doc.scenes()) {
    doc.set_scene(scene.key.id);
    for(const auto sample:{SamplePoint::native_frame,SamplePoint::after_fetch}) {
      if(!doc.set_sample_point(sample))continue;
      for(std::size_t frame=0;frame<doc.frame()->frame_count;++frame) {
        doc.set_frame(frame);doc.show_all();if(doc.frame()->draws.empty())continue;
        const auto selection=doc.frame()->draws.front().key;doc.select(selection);
        auto options=doc.view_options();options.sleeping_scope=VisualizationScope::selected;options.trigger_scope=VisualizationScope::unselected;doc.set_view_options(options);
        for(const auto &shape:doc.frame()->render.instances) {
          const bool picked=shape.picking_key()==selection;
          const bool expected=(shape.actor.sleeping!=true||picked)&&(!shape.flags.trigger||!picked);
          check(doc.visible(shape.picking_key())==expected,"actual SDK status availability/flags obey requested scopes");++checks;
        }
      }
    }
  }
  check(checks>0,"real SDK attachment scope comparisons");
  std::cout<<"Real SDK status scope comparisons "<<checks<<" passed\n";
}
}
int main(int argc,char **argv) {
  try {model();live();for(float scale:{1.f,1.5f,2.f})widgets(scale);if(argc==2)real(argv[1]);else check(argc==1,"usage: status_scope_tests [capture.pxd2]");return 0;}
  catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}
