#include "repx_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <thread>

using namespace pvd;
using namespace pvd::viewer;
namespace {
void check(bool b, const char *m) { if (!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) {
  try { f(); } catch(const std::exception &) { return; }
  throw std::runtime_error("expected rejection");
}
struct Fixture {
  std::vector<Event> events;
  std::map<std::string,std::uint32_t> strings;
  void emit(Payload p) { events.push_back({1,events.size(),0,std::move(p)}); }
  std::uint32_t str(const std::string &s) {
    auto f=strings.find(s); if(f!=strings.end()) return f->second;
    auto n=static_cast<std::uint32_t>(strings.size()+1); strings[s]=n;
    emit(StringHandleEvent{s,n}); return n;
  }
  WireName type(const char *t) { auto n=str("physx3"),h=str(t);return {n,h}; }
  void prop(const char *c,const char *p,const char *t,bool array=false) {
    auto cls=type(c),dt=type(t);auto n=str(p);
    emit(CreateProperty{cls,n,0,dt,static_cast<std::uint8_t>(array?2:1),{}});
  }
  void value(std::uint64_t id,const char *p,const char *t,Bytes b,unsigned count=1) {
    auto n=str(p);auto dt=type(t);emit(SetPropertyValue{id,n,dt,count,std::move(b)});
  }
  static Bytes u(std::uint64_t v,std::size_t n=4) {
    Bytes b;for(std::size_t i=0;i<n;++i)b.push_back(static_cast<std::uint8_t>(v>>(8*i)));return b;
  }
  static Bytes f(std::initializer_list<float> values) {
    Bytes b;for(float v:values){std::uint32_t n;std::memcpy(&n,&v,4);auto a=u(n);b.insert(b.end(),a.begin(),a.end());}return b;
  }
  Fixture() {
    for(auto c:{"PxScene","PxRigidStatic","PxShape","PxBoxGeometry","PxMaterial","PxFilterData"})emit(CreateClass{type(c)});
    for(auto p:{"ActorFlags","DominanceGroup","OwnerClient","ClientBehaviorFlags"})prop("PxRigidStatic",p,"PvdU32");
    prop("PxRigidStatic","GlobalPose","PxTransform");prop("PxRigidStatic","Scene","ObjectRef");prop("PxRigidStatic","Shapes","ObjectRef",true);
    for(auto p:{"Flags","IsExclusive"})prop("PxShape",p,"PvdU32");
    prop("PxShape","Geometry","ObjectRef");prop("PxShape","Materials","ObjectRef",true);prop("PxShape","Actor","ObjectRef");
    prop("PxShape","LocalPose","PxTransform");
    for(auto p:{"ContactOffset","RestOffset"})prop("PxShape",p,"PvdF32");
    for(auto p:{"SimulationFilterData","QueryFilterData"})prop("PxShape",p,"PxFilterData");
    prop("PxBoxGeometry","HalfExtents","PxVec3");
    for(auto p:{"StaticFriction","DynamicFriction","Restitution"})prop("PxMaterial",p,"PvdF32");
    for(auto p:{"Flags","FrictionCombineMode","RestitutionCombineMode"})prop("PxMaterial",p,"PvdU32");
    for(auto pair:{std::pair<const char*,std::uint64_t>{"PxScene",1},{"PxRigidStatic",2},{"PxShape",3},{"PxBoxGeometry",4},{"PxMaterial",5}})emit(CreateInstance{type(pair.first),pair.second});
    for(auto p:{"ActorFlags","DominanceGroup","OwnerClient","ClientBehaviorFlags"})value(2,p,"PvdU32",u(0));
    value(2,"GlobalPose","PxTransform",f({0,0,0,1,1,2,3}));
    value(2,"Scene","VoidPtr",u(1,8));value(2,"Shapes","VoidPtr",u(3,8));
    value(3,"Geometry","VoidPtr",u(4,8));value(3,"Materials","VoidPtr",u(5,8));value(3,"Actor","VoidPtr",u(2,8));
    value(3,"Flags","PvdU32",u(11));value(3,"IsExclusive","PvdU32",u(1));
    value(3,"LocalPose","PxTransform",f({0,0,0,1,0,0,0}));
    value(3,"ContactOffset","PvdF32",f({.02f}));value(3,"RestOffset","PvdF32",f({0}));
    for(auto p:{"SimulationFilterData","QueryFilterData"})value(3,p,"PxFilterData",Bytes(16));
    value(4,"HalfExtents","PxVec3",f({1,2,3}));
    for(auto p:{"StaticFriction","DynamicFriction","Restitution"})value(5,p,"PvdF32",f({.2f}));
    for(auto p:{"Flags","FrictionCombineMode","RestitutionCombineMode"})value(5,p,"PvdU32",u(0));
    auto frame=str("frame");emit(BeginSection{1,frame,100});emit(EndSection{1,frame,200});
    value(2,"GlobalPose","PxTransform",f({0,0,0,1,8,9,10}));
    emit(BeginSection{1,frame,300});emit(EndSection{1,frame,400});
    for(auto id:{2u,3u,4u,5u,1u})emit(DestroyInstance{id});
  }
  std::unique_ptr<Session> session() {
    StreamHeader header;header.stream_flags=1;
    return std::make_unique<Session>(header,events);
  }
};
std::string read(const std::filesystem::path &p) {std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
RepxStatus finish(RepxJob &job) {
  auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(job.running()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(!job.running(),"worker timed out");job.wait();return job.status();
}
void checks(Document &doc,const std::filesystem::path &dir,bool partial) {
  auto output=dir/std::filesystem::u8path(u8"当前帧-🚀.repx");
  auto request=make_repx_request(doc,RepxScope::current_scene,partial,{output.u8string(),false});
  check(request.event_count==doc.frame()->snapshot.event_count,"export ignored displayed prefix");
  auto expected=repx::export_session(*doc.session(),request.event_count,request.options);
  RepxJob job;job.start(*doc.session(),request);auto saved=finish(job);
  check(saved.stage==RepxStage::completed&&saved.actors==expected.actor_count&&saved.joints==expected.joint_count&&saved.complete==expected.complete,"background export differs");
  check(read(output)==expected.xml,"background file differs from snapshot");
  auto original=read(output);job.start(*doc.session(),request);
  check(finish(job).stage==RepxStage::failed&&read(output)==original,"no-overwrite damaged file");
  request.overwrite=true;job.start(*doc.session(),request);check(finish(job).stage==RepxStage::completed,"overwrite failed");
  request.event_count=doc.session()->event_count()+1;job.start(*doc.session(),request);
  check(finish(job).stage==RepxStage::failed&&read(output)==original,"invalid prefix changed file");
  request.event_count=doc.frame()->snapshot.event_count;
  std::promise<void> entered,release;auto unblock=release.get_future().share();
  request.options.cancelled=[&]{entered.set_value();unblock.wait();return false;};
  job.start(*doc.session(),request);auto signal=entered.get_future();
  if(signal.wait_for(std::chrono::seconds(5))!=std::future_status::ready){release.set_value();throw std::runtime_error("cancel hook not reached");}
  rejects([&]{job.start(*doc.session(),request);});job.cancel();release.set_value();
  check(finish(job).stage==RepxStage::cancelled&&read(output)==original,"cancel published output");
  check(doc.frame()&&!doc.frame()->draws.empty(),"missing source draws");
  const auto chosen=doc.frame()->draws.front().key;doc.select(chosen);
  auto selected=make_repx_request(doc,RepxScope::selection,partial,{});
  check(selected.options.attachments.size()==1&&selected.options.attachments[0].actor.id==chosen.actor.id,"selection lost attachment owner");
  auto one=repx::export_session(*doc.session(),selected.event_count,selected.options);
  check(one.actor_count==1&&one.shape_count==1,"selected shape expanded siblings/owners");
  doc.set_visible(chosen,false);
  if(doc.frame()->draws.size()==1)rejects([&]{make_repx_request(doc,RepxScope::visible_shapes,partial,{});});
  else {
    auto visible=make_repx_request(doc,RepxScope::visible_shapes,partial,{});
    for(const auto &a:visible.options.attachments)check(a.shape.id!=chosen.shape.id||a.actor.id!=chosen.actor.id,"hidden attachment exported");
  }
  doc.show_all();
}
void widgets(Document &doc,const std::filesystem::path &dir,float width) {
  ImGui::CreateContext();auto &io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={width,800};io.DeltaTime=1.f/60;
  unsigned char *pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
  RepxPanelState panel;RepxJob job;int chosen=0;ExportPanelResult result;
  auto draw=[&]{ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({width,800});ImGui::Begin("test",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    result=draw_repx_panel(doc,job,panel,[&](const char*)->std::optional<ExportDestination>{++chosen;return ExportDestination{(dir/("widget"+std::to_string(static_cast<int>(width))+".repx")).u8string(),false};});
    ImGui::End();ImGui::Render();};
  draw();
  check(ImGui::GetDrawData()->TotalVtxCount>100,"panel produced no widgets");
  check(chosen==0&&!result.started,"panel exported without input");
  ImGui::DestroyContext();
}
}
int main(int argc,char **argv) {
  try {
    auto dir=std::filesystem::current_path()/"build"/("repx-job-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    Fixture fixture;Document doc;doc.adopt(fixture.session());
    check(doc.timeline().size()==2,"fixture frames missing");
    check(doc.session()->snapshot_at_event(doc.session()->event_count()).instances.empty(),"fixture EOF not empty");
    checks(doc,dir,false);widgets(doc,dir,420);widgets(doc,dir,1060);
    if(argc>1){Document actual;actual.open_file(argv[1]);auto sub=dir/"sdk";std::filesystem::create_directory(sub);checks(actual,sub,true);}
    std::cout<<"RepX worker, displayed-prefix and exact-attachment tests passed\n";return 0;
  } catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}
