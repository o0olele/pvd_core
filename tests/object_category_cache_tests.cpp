#include "panels.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
namespace {
using namespace pvd;
using namespace pvd::viewer;
constexpr std::uint64_t material_base = 0x20000000000001ull;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
  std::map<std::string, std::uint32_t> strings;
  std::vector<Event> events;
  void emit(Payload value) { events.push_back({1, events.size(), events.size(), std::move(value)}); }
  std::uint32_t text(const std::string &s) {
    const auto found = strings.find(s); if (found != strings.end()) return found->second;
    const auto id = static_cast<std::uint32_t>(strings.size() + 1); strings[s] = id;
    emit(StringHandleEvent{s, id}); return id;
  }
  WireName type(const char *name) { const auto ns = text("physx3"); return {ns, text(name)}; }
  void prop(const char *cls, const char *name, const char *datatype, bool array = false) {
    const auto c = type(cls), t = type(datatype); const auto n = text(name);
    emit(CreateProperty{c, n, 0, t, static_cast<std::uint8_t>(array ? 2 : 1), {}});
  }
  void value(std::uint64_t id, const char *name, const char *datatype, Bytes data, std::uint32_t count=1) {
    const auto n = text(name); const auto t = type(datatype); emit(SetPropertyValue{id,n,t,count,std::move(data)});
  }
  void name(std::uint64_t id, const char *key, const std::string &label) {
    Bytes data(8); data.insert(data.end(), label.begin(), label.end()); data.push_back(0);
    value(id,key,"String",std::move(data));
  }
  void pose(float x) {
    float values[] = {0,0,0,1,x,0,0}; Bytes bytes(sizeof(values)); std::memcpy(bytes.data(),values,sizeof(values));
    value(10,"GlobalPose","PxTransform",std::move(bytes));
  }
  void frame() { const auto id=text("frame"); emit(BeginSection{1,id,events.size()}); emit(EndSection{1,id,events.size()}); }
  Fixture() {
    for(const auto *c:{"PxScene","PxMaterial","PxRigidStatic","CustomMaterial"}) emit(CreateClass{type(c)});
    for(const auto *c:{"PxMaterial","CustomMaterial","PxRigidStatic"})prop(c,"Name","StringHandle");
    prop("PxMaterial","DynamicFriction","PvdF32"); prop("PxRigidStatic","name","StringHandle");
    prop("PxRigidStatic","GlobalPose","PxTransform"); prop("PxRigidStatic","Scene","ObjectRef"); prop("PxRigidStatic","Shapes","ObjectRef",true);
    emit(CreateInstance{type("PxScene"),1}); emit(CreateInstance{type("PxRigidStatic"),10});
    Bytes scene(8);scene[0]=1;value(10,"Scene","VoidPtr",std::move(scene));value(10,"Shapes","VoidPtr",{},0);pose(0);
    name(10,"Name","upper");name(10,"name","lower");
    for(unsigned i=0;i<300;++i){emit(CreateInstance{type("PxMaterial"),material_base+i});name(material_base+i,"Name","synthetic material "+std::to_string(i));}
    frame();
    pose(3);value(material_base,"DynamicFriction","PvdF32",Bytes{0,0,0,0});frame();
    name(10,"name","lower changed");frame();
    name(10,"Name","");frame();
    emit(DestroyInstance{material_base});emit(CreateInstance{type("PxMaterial"),material_base});name(material_base,"Name","same ID new lifetime");frame();
    emit(DestroyInstance{material_base+299});frame();
    emit(DestroyInstance{material_base});emit(CreateInstance{type("CustomMaterial"),material_base});name(material_base,"Name","new class");frame();
  }
  std::unique_ptr<Session> session() const {
    StreamHeader header;header.stream_flags=1;SessionOptions options;options.max_checkpoints=0;
    return std::make_unique<Session>(header,events,options);
  }
};
void paint(Document &doc, PanelState &state) {
  auto &io=ImGui::GetIO();io.DisplaySize={1060,700};io.DeltaTime=1.f/60;
  ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({1060,700});
  ImGui::Begin("Object cache",nullptr,ImGuiWindowFlags_NoSavedSettings);drawHierarchy(doc,state);ImGui::End();ImGui::Render();
  check(state.error.empty(),"hierarchy command failed");
}
ObjectNode node(const PanelState &state, std::uint64_t id) {
  for(const auto &group:state.categories)for(const auto &object:group.objects)if(object.key.id==id)return object;
  throw std::runtime_error("cached live object missing");
}
std::size_t group_index(const PanelState &state,const char *name) {
  for(std::size_t i=0;i<state.categories.size();++i)if(state.categories[i].class_name.name==name)return i;
  throw std::runtime_error("cached class missing");
}
void run() {
  Fixture fixture;Document doc;doc.adopt(fixture.session());PanelState panel;
  std::copy_n("physx3",7,panel.hierarchy_search.data());paint(doc,panel);
  check(panel.object_count==302 && node(panel,material_base).key.id==material_base,"complete classes retain exact large IDs and all materials");
  const auto *groups=panel.categories.data();const auto *matches=panel.category_matches.data();
  const auto material_group=group_index(panel,"PxMaterial");
  const auto *material_rows=panel.categories[material_group].objects.data();
  const auto *material_matches=panel.category_matches[material_group].data();
  panel.category_object_pages["physx3::PxMaterial"]=1;
  doc.set_frame(1);paint(doc,panel);
  check(panel.categories.data()==groups && panel.category_matches.data()==matches && panel.category_object_pages.at("physx3::PxMaterial")==1 && panel.category_prefix==doc.frame()->snapshot.event_count,
        "pose/numeric-only playback retains category allocation, search matches and paging");
  doc.set_frame(2);paint(doc,panel);
  check(panel.categories.data()==groups && node(panel,10).name=="upper","lowercase name changes cannot invalidate an effective uppercase Name");
  doc.set_frame(3);paint(doc,panel);
  check(node(panel,10).name=="lower changed","empty Name reveals updated lowercase name");
  check(panel.categories[group_index(panel,"PxMaterial")].objects.data()==material_rows &&
            panel.category_matches[group_index(panel,"PxMaterial")].data()==material_matches &&
            panel.category_object_pages.at("physx3::PxMaterial")==1,
        "a genuine name change rebuilds only its own class and preserves material rows/matches/page");
  const auto *actor_rows=panel.categories[group_index(panel,"PxRigidStatic")].objects.data();
  const auto *actor_matches=panel.category_matches[group_index(panel,"PxRigidStatic")].data();
  doc.set_frame(4);paint(doc,panel);
  check(node(panel,material_base).key.generation==1 && node(panel,material_base).name=="same ID new lifetime","ID reuse updates name and generation");
  doc.set_frame(5);paint(doc,panel);check(panel.object_count==301,"destroyed live object is removed");
  doc.set_frame(6);paint(doc,panel);check(node(panel,material_base).class_name=="physx3::CustomMaterial","class changes reclassify the object");
  check(panel.categories[group_index(panel,"PxRigidStatic")].objects.data()==actor_rows &&
            panel.category_matches[group_index(panel,"PxRigidStatic")].data()==actor_matches,
        "adding a class moves unaffected groups without copying their rows or matches");
  doc.set_frame(5);paint(doc,panel);
  check(panel.categories[group_index(panel,"PxRigidStatic")].objects.data()==actor_rows &&
            panel.category_matches[group_index(panel,"PxRigidStatic")].data()==actor_matches,
        "removing a class keeps unaffected group storage stable");
  doc.set_frame(0);paint(doc,panel);
  check(panel.object_count==302 && node(panel,material_base).key.generation==0 && node(panel,10).name=="upper","reverse seek restores identities, names and class lists");
  const auto retained=panel.categories.data();doc.inspect(material_base);paint(doc,panel);
  check(panel.categories.data()==retained,"same-prefix selection does not rebuild complete class lists");
  Fixture replacement;const auto previous=doc.session();doc.adopt(replacement.session());paint(doc,panel);
  check(doc.session()!=previous && panel.category_source==doc.session() && node(panel,material_base).key.generation==0,
        "new Session rebinds the category source");
  std::copy_n("upper",6,panel.hierarchy_search.data());paint(doc,panel);
  check(panel.object_count==1 && node(panel,10).name=="upper","search cache still responds to changed queries");
}
}
int main(){try{ImGui::CreateContext();auto&io=ImGui::GetIO();io.IniFilename=nullptr;io.Fonts->AddFontDefault();unsigned char*p;int w,h;io.Fonts->GetTexDataAsRGBA32(&p,&w,&h);run();ImGui::DestroyContext();std::cout<<"Object category cache: complete source/pose-only paging/name precedence/generation/class/deletion/reverse/search passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}}
