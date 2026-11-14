#include "analysis_panels.hpp"
#include "pvd/profile.hpp"
#include "pvd/memory.hpp"
#include "pvd/errors.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace pvd::viewer {
namespace {
std::string hex(std::uint64_t value) { std::ostringstream s; s<<"0x"<<std::hex<<std::uppercase<<value;return s.str(); }
void text(const std::string& value) { ImGui::TextUnformatted(value.c_str()); if(ImGui::IsItemHovered()&&value.size()>24)ImGui::SetTooltip("%s",value.c_str()); }
std::string duration(std::optional<double> ns) {
  if(!ns)return "Clock unavailable";
  std::ostringstream s;s<<std::fixed<<std::setprecision(3);
  if(*ns>=1e6)s<<*ns/1e6<<" ms";else if(*ns>=1e3)s<<*ns/1e3<<" us";else s<<*ns<<" ns";
  return s.str();
}
std::size_t prefix(const Document& doc,bool current) { return current&&doc.frame()?doc.frame()->snapshot.event_count:doc.session()->event_count(); }
void jump(Document& doc,std::size_t event_index) {
  const auto& frames=doc.session()->frames();
  const pvd::FrameMarker* best=nullptr;
  for(const auto& f:frames) if(f.event_count>event_index&&(!best||f.event_count<best->event_count))best=&f;
  if(!best&&!frames.empty())best=&frames.back();
  if(best) { doc.set_scene(best->scene_id);doc.set_sample_point(SamplePoint::native_frame);doc.set_frame(best->index); }
}
void page_buttons(std::size_t& page,std::size_t total,bool& dirty) {
  const auto last=total?((total-1)/256):0;page=std::min(page,last);
  ImGui::BeginDisabled(page==0);if(ImGui::SmallButton("Previous page")){--page;dirty=true;}ImGui::EndDisabled();ImGui::SameLine();
  ImGui::BeginDisabled(page==last);if(ImGui::SmallButton("Next page")){++page;dirty=true;}ImGui::EndDisabled();ImGui::SameLine();
  ImGui::TextDisabled("Page %zu / %zu  |  %zu matching rows",page+1,last+1,total);
}
template<class Diagnostics>void diagnostics(const Diagnostics& rows) {
  if(rows.empty())return;
  ImGui::TextColored({1,.66f,.35f,1},"%zu analysis messages",rows.size());
  if(ImGui::CollapsingHeader("Analysis diagnostics")) {
    const auto count=std::min<std::size_t>(rows.size(),200);
    for(std::size_t i=0;i<count;++i)ImGui::TextWrapped("Event %zu: %s",rows[i].event_index,rows[i].message.c_str());
    if(count<rows.size())ImGui::TextDisabled("Showing the first %zu messages",count);
  }
}
std::uint64_t tick(long double value) {
  if(!(value>0))return 0;
  if(value>=static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))return std::numeric_limits<std::uint64_t>::max();
  return static_cast<std::uint64_t>(value);
}
using Lane=std::tuple<std::uint32_t,std::uint64_t,std::uint64_t,std::uint64_t>;
Lane lane(const pvd::profile::Interval& e){return {e.thread_id,e.start_source.zone.id,e.start_source.zone.generation,e.context_id};}
} // namespace

struct AnalysisPanels::Impl {
  const Session* source=nullptr;
  std::unique_ptr<pvd::profile::Index> profile;
  std::unique_ptr<pvd::memory::Index> memory;
  std::unique_ptr<pvd::errors::Index> errors;
  std::size_t profile_count=0,memory_count=0,error_count=0;
  bool profile_current=false,memory_current=true,error_current=false;
  bool profile_dirty=true,memory_dirty=true,error_dirty=true,view_manual=false;
  std::optional<std::uint32_t> thread;
  std::optional<std::uint64_t> zone;
  std::optional<std::size_t> selected_interval;
  std::vector<std::uint32_t> threads;
  std::vector<std::size_t> intervals;
  std::vector<pvd::profile::Aggregate> aggregates;
  std::size_t p_begin=0,p_end=0;
  std::uint64_t view_begin=0,view_end=1;
  std::array<char,128> profile_search{},memory_search{},error_search{};
  pvd::memory::Filter memory_filter;
  pvd::errors::Filter error_filter;
  pvd::memory::Snapshot memory_snapshot;
  std::vector<pvd::memory::Allocation> memory_rows;
  std::vector<pvd::errors::Entry> error_rows;
  std::size_t m_prefix=std::numeric_limits<std::size_t>::max(),e_prefix=std::numeric_limits<std::size_t>::max();
  std::size_t memory_page=0,error_page=0,matches_memory=0,matches_errors=0;
  int group_by=3;
  std::string error;

  void bind(const Session* session) {
    if(source==session)return;
    source=session;profile.reset();memory.reset();errors.reset();
    profile_count=memory_count=error_count=0;profile_dirty=memory_dirty=error_dirty=true;
    selected_interval.reset();view_manual=false;memory_page=error_page=0;m_prefix=e_prefix=std::numeric_limits<std::size_t>::max();
    thread.reset();zone.reset();error.clear();
  }
  void refresh(AnalysisPage page) {
    const auto count=source->event_count();
    if(page==AnalysisPage::profile) {
      if(!profile)profile=std::make_unique<pvd::profile::Index>(source->header());
      if(profile_count!=count) {
        profile->update(*source);profile_count=count;profile_dirty=true;
        std::set<std::uint32_t> ids;for(const auto& e:profile->events())if(e.kind==pvd::profile::Kind::start||e.kind==pvd::profile::Kind::stop)ids.insert(e.thread_id);
        threads.assign(ids.begin(),ids.end());
      }
      if(source->finished()&&!profile->statistics().finished)profile->finish();
    } else if(page==AnalysisPage::memory) {
      if(!memory)memory=std::make_unique<pvd::memory::Index>();
      if(memory_count!=count){memory->update(*source);memory_count=count;memory_dirty=true;}
    } else {
      if(!errors)errors=std::make_unique<pvd::errors::Index>(std::vector<pvd::Event>{});
      if(error_count!=count){errors->update(*source);error_count=count;error_dirty=true;}
    }
  }
  void profile_view(Document& doc,const CsvExport& save) {
    ImGui::TextUnformatted("CPU PROFILER");ImGui::SameLine();
    if(ImGui::Checkbox("Current frame",&profile_current)){profile_dirty=true;view_manual=false;}
    ImGui::SameLine();ImGui::SetNextItemWidth(185);
    const auto thread_name=thread?std::to_string(*thread):"All threads";
    if(ImGui::BeginCombo("##thread",thread_name.c_str())) {
      if(ImGui::Selectable("All threads",!thread)){thread.reset();profile_dirty=true;}
      for(auto id:threads)if(ImGui::Selectable(std::to_string(id).c_str(),thread&&*thread==id)){thread=id;profile_dirty=true;}
      ImGui::EndCombo();
    }
    ImGui::SameLine();ImGui::SetNextItemWidth(190);
    std::string zone_name="All zones";for(const auto& z:profile->zones())if(zone&&z.first.id==*zone)zone_name=z.second.name;
    if(ImGui::BeginCombo("##zone",zone_name.c_str())) {
      if(ImGui::Selectable("All zones",!zone)){zone.reset();profile_dirty=true;}
      for(const auto& z:profile->zones()){
        ImGui::PushID(hex(z.first.id).c_str());ImGui::PushID(hex(z.first.generation).c_str());
        if(ImGui::Selectable(z.second.name.c_str(),zone&&*zone==z.first.id)){zone=z.first.id;profile_dirty=true;}
        ImGui::PopID();ImGui::PopID();
      }ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(300);if(ImGui::InputTextWithHint("##profile-search","Filter event names...",profile_search.data(),profile_search.size()))profile_dirty=true;
    ImGui::SameLine();if(ImGui::SmallButton("Fit timeline")){view_manual=false;profile_dirty=true;}
    pvd::profile::Query q;q.thread_id=thread;q.zone_id=zone;q.text=profile_search.data();
    q.end_event_count=prefix(doc,profile_current);
    if(profile_current&&doc.frame()->marker)q.begin_event_count=doc.frame()->marker->begin_event_count;
    if(p_begin!=q.begin_event_count||p_end!=q.end_event_count){p_begin=q.begin_event_count;p_end=q.end_event_count;profile_dirty=true;if(profile_current)view_manual=false;}
    if(profile_dirty) {
      intervals=profile->query(q);
      aggregates=profile->aggregate(q,true);
      std::sort(aggregates.begin(),aggregates.end(),[](const auto& a,const auto& b){return a.total_ticks>b.total_ticks;});
      if(!view_manual&&!intervals.empty()) {
        view_begin=std::numeric_limits<std::uint64_t>::max();view_end=0;
        for(auto i:intervals){view_begin=std::min(view_begin,profile->intervals()[i].begin_ticks);view_end=std::max(view_end,profile->intervals()[i].end_ticks);}
        if(view_end<=view_begin){if(view_begin==std::numeric_limits<std::uint64_t>::max())--view_begin;view_end=view_begin+1;}
      }
      profile_dirty=false;
    }
    ImGui::SameLine();if(ImGui::Button("Export CSV..."))save("profile.csv",[&](std::ostream& out){profile->write_csv(out,q);});
    const auto statistics=profile->statistics();
    ImGui::TextDisabled("%zu packets  |  %zu completed intervals  |  %zu open scopes%s",statistics.profile_packets,intervals.size(),statistics.open_scopes,statistics.complete?"":statistics.malformed_packets||statistics.saturated||statistics.skipped_gpu_events?"  |  INCOMPLETE DATA":"  |  Has diagnostics");
    if(!statistics.profile_packets){ImGui::TextWrapped("This capture has no profile channel. Enable PhysX PVD ePROFILE instrumentation in the producer.");return;}
    if(ImGui::BeginTabBar("profile-pages")) {
      if(ImGui::BeginTabItem("Thread timeline")){timeline(doc);ImGui::EndTabItem();}
      if(ImGui::BeginTabItem("Summary")) {
        if(ImGui::BeginTable("profile-summary",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y-60})){
          for(auto name:{"Event","Zone / thread","Count","Inclusive total","Mean","Max"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(aggregates.size()));
          while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& a=aggregates[std::size_t(row)];ImGui::TableNextRow();ImGui::TableNextColumn();text(a.event_name);ImGui::TableNextColumn();text(a.zone_name+" / "+(a.thread_id?std::to_string(*a.thread_id):"all"));ImGui::TableNextColumn();ImGui::Text("%zu",a.count);ImGui::TableNextColumn();text(duration(a.total_nanoseconds));ImGui::TableNextColumn();text(duration(a.mean_nanoseconds));ImGui::TableNextColumn();text(duration(a.maximum_nanoseconds));}
          ImGui::EndTable();
        }ImGui::EndTabItem();
      }
      if(ImGui::BeginTabItem("Intervals")) {
        if(ImGui::BeginTable("profile-intervals",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y-60})) {
          for(auto name:{"Event","Thread","Context","Duration","Source event"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(intervals.size()));
          while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto id=intervals[std::size_t(row)];const auto& e=profile->intervals()[id];ImGui::PushID(static_cast<int>(id));ImGui::TableNextRow();ImGui::TableNextColumn();if(ImGui::Selectable(profile->event_name(e.start_source.zone,e.event_id).c_str(),selected_interval&&*selected_interval==id,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowDoubleClick)){selected_interval=id;if(ImGui::IsMouseDoubleClicked(0))jump(doc,e.stop_source.event_index);}ImGui::TableNextColumn();ImGui::Text("%u",e.thread_id);ImGui::TableNextColumn();text(hex(e.context_id));ImGui::TableNextColumn();text(duration(e.duration_nanoseconds));ImGui::TableNextColumn();ImGui::Text("%zu",e.stop_source.event_index);ImGui::PopID();}ImGui::EndTable();
        }ImGui::EndTabItem();
      }ImGui::EndTabBar();
    }
    if(selected_interval&&*selected_interval<profile->intervals().size()){
      const auto& e=profile->intervals()[*selected_interval];text(profile->event_name(e.start_source.zone,e.event_id)+"  /  "+duration(e.duration_nanoseconds));
      ImGui::SameLine();ImGui::BeginDisabled(doc.session()->frames().empty());if(ImGui::SmallButton("Jump to received frame"))jump(doc,e.stop_source.event_index);ImGui::EndDisabled();
    }
    if(ImGui::CollapsingHeader("Profile diagnostics"))for(const auto& d:profile->diagnostics())ImGui::TextWrapped("Event %zu: %s",d.source.event_index,d.message.c_str());
  }
  void timeline(Document& doc) {
    std::map<Lane,std::size_t> depths;
    for(auto i:intervals){const auto& e=profile->intervals()[i];depths[lane(e)]=std::max(depths[lane(e)],std::min<std::size_t>(e.depth,15)+1);}
    std::map<Lane,float> lanes;float height=30;
    for(const auto& lane:depths){lanes[lane.first]=height;height+=25+float(lane.second)*19;}
    ImGui::TextDisabled("Wheel to zoom  |  Middle drag to pan  |  Double-click an interval to locate its received frame");
    ImGui::TextDisabled("Visible origin: %llu native ticks; axis is elapsed time from this origin",static_cast<unsigned long long>(view_begin));
    ImGui::BeginChild("thread-timeline",{0,std::max(140.0f,ImGui::GetContentRegionAvail().y-90)},ImGuiChildFlags_Borders);
    const auto origin=ImGui::GetCursorScreenPos();const float width=std::max(300.0f,ImGui::GetContentRegionAvail().x),gutter=240,graph=width-gutter;
    ImGui::InvisibleButton("timeline-canvas",{width,std::max(height,ImGui::GetContentRegionAvail().y)},ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    auto* draw=ImGui::GetWindowDrawList();
    const long double span=std::max(1.0L,static_cast<long double>(view_end)-view_begin);
    auto x=[&](std::uint64_t t){return origin.x+gutter+float((static_cast<long double>(t)-view_begin)/span)*graph;};
    for(int i=0;i<=5;++i){const float at=origin.x+gutter+graph*float(i)/5;draw->AddLine({at,origin.y},{at,origin.y+height},IM_COL32(42,55,70,255));auto label=duration(profile->nanoseconds(tick(span*i/5)));draw->AddText({at+3,origin.y},IM_COL32(115,139,160,255),label.c_str());}
    for(const auto& entry:lanes){const auto label="T"+std::to_string(std::get<0>(entry.first))+"  "+profile->zone_name({std::get<1>(entry.first),std::get<2>(entry.first)})+" / "+hex(std::get<3>(entry.first));draw->PushClipRect(origin,{origin.x+gutter-5,origin.y+height},true);draw->AddText({origin.x,origin.y+entry.second},IM_COL32(195,213,225,255),label.c_str());draw->PopClipRect();}
    std::size_t painted=0;
    for(auto i:intervals){const auto& e=profile->intervals()[i];if(e.end_ticks<view_begin||e.begin_ticks>view_end)continue;if(++painted>100000)break;
      const float y=origin.y+lanes[lane(e)]+float(std::min<std::size_t>(e.depth,15))*19;
      if(y+17<ImGui::GetWindowPos().y||y>ImGui::GetWindowPos().y+ImGui::GetWindowHeight())continue;
      const ImVec2 a{std::clamp(x(e.begin_ticks),origin.x+gutter,origin.x+width),y},b{std::clamp(std::max(x(e.end_ticks),a.x+2),a.x+2,origin.x+width+2),y+17};
      const auto color=ImColor::HSV(float((e.event_id*37u)%255u)/255.0f,.50f,.68f);
      draw->AddRectFilled(a,b,color,2);
      if(selected_interval&&*selected_interval==i)draw->AddRect(a,b,IM_COL32(255,218,90,255),2,0,2);
      if(b.x-a.x>45){const auto label=profile->event_name(e.start_source.zone,e.event_id);draw->PushClipRect(a,b,true);draw->AddText({a.x+3,a.y},IM_COL32(245,248,250,255),label.c_str());draw->PopClipRect();}
      if(ImGui::IsItemHovered()&&ImGui::IsMouseHoveringRect(a,b)){
        ImGui::SetTooltip("%s\n%s\nThread %u / context %s\n%llu native ticks",profile->event_name(e.start_source.zone,e.event_id).c_str(),duration(e.duration_nanoseconds).c_str(),e.thread_id,hex(e.context_id).c_str(),static_cast<unsigned long long>(e.duration_ticks));
        if(ImGui::IsMouseClicked(0))selected_interval=i;
        if(ImGui::IsMouseDoubleClicked(0))jump(doc,e.stop_source.event_index);
      }
    }
    const auto& io=ImGui::GetIO();
    if(ImGui::IsItemHovered()&&io.MouseWheel!=0){const auto anchor=std::clamp((io.MousePos.x-origin.x-gutter)/graph,0.0f,1.0f);const long double next=std::max(1.0L,span*std::exp(-io.MouseWheel*.20));const long double begin=std::max(0.0L,static_cast<long double>(view_begin)+anchor*(span-next));view_begin=tick(begin);view_end=tick(begin+next);view_manual=true;}
    if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){const long double begin=std::max(0.0L,static_cast<long double>(view_begin)-io.MouseDelta.x/graph*span);view_begin=tick(begin);view_end=tick(begin+span);view_manual=true;}
    ImGui::EndChild();
    if(painted>100000)ImGui::TextDisabled("More than 100,000 visible intervals; zoom or filter by thread/zone.");
  }
  void memory_view(Document& doc,const CsvExport& save) {
    ImGui::TextUnformatted("MEMORY");ImGui::SameLine();if(ImGui::Checkbox("At current frame",&memory_current))memory_dirty=true;
    ImGui::SameLine();if(ImGui::Checkbox("Active only",&memory_filter.active_only)){memory_page=0;memory_dirty=true;}
    ImGui::SetNextItemWidth(300);if(ImGui::InputTextWithHint("##memory-search","History filter: type, file, address...",memory_search.data(),memory_search.size())){memory_filter.text=memory_search.data();memory_page=0;memory_dirty=true;}
    ImGui::SameLine();ImGui::SetNextItemWidth(200);const char* groups[]={"Type","File","File and line","Type, file and line"};if(ImGui::Combo("Group by",&group_by,groups,4))memory_dirty=true;
    const auto count=prefix(doc,memory_current);if(m_prefix!=count){m_prefix=count;memory_dirty=true;}
    if(memory_dirty){memory_snapshot=memory->summary_at_event(count,static_cast<pvd::memory::GroupBy>(group_by));std::sort(memory_snapshot.groups.begin(),memory_snapshot.groups.end(),[](const auto& a,const auto& b){return a.stats.current_bytes>b.stats.current_bytes;});matches_memory=memory->matching_count(count,memory_filter);memory_page=std::min(memory_page,matches_memory?(matches_memory-1)/256:0);memory_rows=memory->history_at_event(count,memory_filter,memory_page*256,256);memory_dirty=false;}
    ImGui::SameLine();if(ImGui::Button("Export allocations CSV..."))save("memory.csv",[&](std::ostream& out){memory->write_csv(out,count,memory_filter);});
    ImGui::TextDisabled("Totals and groups cover all allocations before event %zu. History filters apply to allocation rows and their CSV.",count);
    const auto& s=memory_snapshot.totals;
    ImGui::Text("Live %.3f MiB (%zu allocations)  |  Peak %.3f MiB  |  Allocated %.3f MiB  |  Freed %.3f MiB",double(s.current_bytes)/(1024*1024),s.current_allocations,double(s.peak_bytes)/(1024*1024),double(s.allocated_bytes)/(1024*1024),double(s.freed_bytes)/(1024*1024));
    if(!memory_snapshot.complete)ImGui::TextColored({1,.65f,.35f,1},"Incomplete memory data; inspect diagnostics before interpreting totals.");
    if(memory->events().empty()&&memory->diagnostics().empty()){ImGui::TextWrapped("No memory channel was captured. Enable PhysX PVD eMEMORY instrumentation in the producer.");return;}
    if(ImGui::BeginTabBar("memory-pages")){
      if(ImGui::BeginTabItem("Allocation history")){
        page_buttons(memory_page,matches_memory,memory_dirty);
        if(ImGui::BeginTable("allocation-history",7,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,{0,ImGui::GetContentRegionAvail().y-60})){
          for(auto name:{"Address / generation","Bytes","Type","Source","Line","State","Allocated event"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(memory_rows.size()));while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& a=memory_rows[std::size_t(row)];ImGui::PushID(row);ImGui::TableNextRow();ImGui::TableNextColumn();if(ImGui::Selectable((hex(a.address)+" / "+std::to_string(a.generation)).c_str(),false,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowDoubleClick)&&ImGui::IsMouseDoubleClicked(0))jump(doc,a.allocated_event_count? a.allocated_event_count-1:0);ImGui::TableNextColumn();text(std::to_string(a.size));ImGui::TableNextColumn();text(a.type_name.value_or("Unresolved string"));ImGui::TableNextColumn();text(a.file.value_or("Unresolved string"));ImGui::TableNextColumn();ImGui::Text("%u",a.line);ImGui::TableNextColumn();text(!a.ended_event_count||*a.ended_event_count>count?"Active":a.end_reason==pvd::memory::EndReason::freed?"Freed":"Replaced without free");ImGui::TableNextColumn();ImGui::Text("%zu",a.allocated_event_count);ImGui::PopID();}ImGui::EndTable();
        }ImGui::EndTabItem();
      }
      if(ImGui::BeginTabItem("Groups")){
        if(ImGui::Button("Export groups CSV..."))save("memory-groups.csv",[&](std::ostream& out){memory->write_groups_csv(out,count,static_cast<pvd::memory::GroupBy>(group_by));});
        if(ImGui::BeginTable("memory-groups",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,{0,ImGui::GetContentRegionAvail().y-60})){
          for(auto name:{"Type","File / line","Current bytes","Peak bytes","Live count","Total allocated"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(memory_snapshot.groups.size()));while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& a=memory_snapshot.groups[std::size_t(row)];ImGui::TableNextRow();ImGui::TableNextColumn();text(a.type_name.value_or(group_by==0||group_by==3?"Unresolved type":"All types"));ImGui::TableNextColumn();text(a.file.value_or(group_by!=0?"Unresolved file":"All files")+(group_by==2||group_by==3?":"+std::to_string(a.line):""));ImGui::TableNextColumn();text(std::to_string(a.stats.current_bytes));ImGui::TableNextColumn();text(std::to_string(a.stats.peak_bytes));ImGui::TableNextColumn();text(std::to_string(a.stats.current_allocations));ImGui::TableNextColumn();text(std::to_string(a.stats.allocated_bytes));}ImGui::EndTable();
        }ImGui::EndTabItem();
      }ImGui::EndTabBar();
    }
    diagnostics(memory_snapshot.diagnostics);
  }
  void error_view(Document& doc,const CsvExport& save) {
    ImGui::TextUnformatted("ERROR STREAM");ImGui::SameLine();if(ImGui::Checkbox("Up to current frame",&error_current))error_dirty=true;
    ImGui::SetNextItemWidth(350);if(ImGui::InputTextWithHint("##error-search","Filter message or file...",error_search.data(),error_search.size())){error_filter.text=error_search.data();error_page=0;error_dirty=true;}
    ImGui::SameLine();ImGui::SetNextItemWidth(210);
    auto code_label=[](std::uint32_t code){return std::string(pvd::errors::code_name(code))+" ("+std::to_string(code)+")";};
    if(ImGui::BeginCombo("##error-code",error_filter.code?code_label(*error_filter.code).c_str():"All codes")){
      if(ImGui::Selectable("All codes",!error_filter.code)){error_filter.code.reset();error_dirty=true;}
      std::set<std::uint32_t> codes;for(const auto& e:errors->entries())codes.insert(e.code);
      for(auto code:codes)if(ImGui::Selectable(code_label(code).c_str(),error_filter.code&&*error_filter.code==code)){error_filter.code=code;error_dirty=true;}
      ImGui::EndCombo();
    }
    error_filter.end_event_count=prefix(doc,error_current);if(e_prefix!=error_filter.end_event_count){e_prefix=error_filter.end_event_count;error_dirty=true;}
    if(error_dirty){matches_errors=errors->matching_count(error_filter);error_page=std::min(error_page,matches_errors?(matches_errors-1)/256:0);error_rows=errors->select(error_filter,error_page*256,256);error_dirty=false;}
    ImGui::SameLine();if(ImGui::Button("Export CSV..."))save("errors.csv",[&](std::ostream& out){errors->write_csv(out,error_filter);});
    page_buttons(error_page,matches_errors,error_dirty);
    if(errors->entries().empty())ImGui::TextDisabled("No errors were reported in this capture.");
    if(ImGui::BeginTable("error-stream",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,{0,ImGui::GetContentRegionAvail().y})){
      for(auto name:{"Code","Message","File","Line","Source event"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
      ImGuiListClipper clipper;clipper.Begin(static_cast<int>(error_rows.size()));while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& e=error_rows[std::size_t(row)];ImGui::PushID(row);ImGui::TableNextRow();ImGui::TableNextColumn();if(ImGui::Selectable(code_label(e.code).c_str(),false,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowDoubleClick)&&ImGui::IsMouseDoubleClicked(0))jump(doc,e.event_index);ImGui::TableNextColumn();text(e.message);ImGui::TableNextColumn();text(e.file);ImGui::TableNextColumn();ImGui::Text("%u",e.line);ImGui::TableNextColumn();ImGui::Text("%zu",e.event_index);ImGui::PopID();}ImGui::EndTable();
    }
  }
};
AnalysisPanels::AnalysisPanels():impl_(std::make_unique<Impl>()){}
AnalysisPanels::~AnalysisPanels()=default;
void AnalysisPanels::reset(){impl_=std::make_unique<Impl>();}
void AnalysisPanels::draw(Document& doc,AnalysisPage page,const CsvExport& save){
  if(!doc.loaded()||!doc.session()){ImGui::TextWrapped("Open a capture or receive a live session to inspect analysis data.");return;}
  try{impl_->bind(doc.session());impl_->refresh(page);if(page==AnalysisPage::profile)impl_->profile_view(doc,save);else if(page==AnalysisPage::memory)impl_->memory_view(doc,save);else impl_->error_view(doc,save);impl_->error.clear();}
  catch(const std::exception& e){impl_->error=e.what();}
  if(!impl_->error.empty())ImGui::TextWrapped("Analysis error: %s",impl_->error.c_str());
}
std::size_t AnalysisPanels::row_count(AnalysisPage page)const noexcept{
  if(page==AnalysisPage::profile)return impl_->profile?impl_->profile->intervals().size():0;
  if(page==AnalysisPage::memory)return impl_->memory?impl_->memory->allocations().size():0;
  return impl_->errors?impl_->errors->entries().size():0;
}
std::size_t AnalysisPanels::visible_row_count(AnalysisPage page)const noexcept{
  if(page==AnalysisPage::profile)return impl_->intervals.size();
  if(page==AnalysisPage::memory)return impl_->matches_memory;
  return impl_->matches_errors;
}
} // namespace pvd::viewer
