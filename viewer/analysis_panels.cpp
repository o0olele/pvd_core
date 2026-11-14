#include "analysis_panels.hpp"
#include "ui_scale.hpp"
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
std::string zone_label(const pvd::profile::Zone &zone) {
  return zone.name+" ["+hex(zone.key.id)+" / generation "+
      std::to_string(zone.key.generation)+"]";
}
void same_line_if_fits(float width) {
  const auto right=ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x;
  if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+width<=right)
    ImGui::SameLine();
}
float button_width(const char *label) {
  return ImGui::CalcTextSize(label).x+ImGui::GetStyle().FramePadding.x*2;
}
ProfileControlRect control_rect() {
  const auto p=ImGui::GetItemRectMin(),size=ImGui::GetItemRectSize();
  return {p.x,p.y,size.x,size.y};
}
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
  std::optional<pvd::profile::ZoneKey> zone;
  std::optional<std::size_t> selected_interval;
  std::vector<std::uint32_t> threads;
  std::vector<std::size_t> intervals;
  std::vector<pvd::profile::Aggregate> aggregates;
  std::size_t p_begin=0,p_end=0;
  std::uint64_t view_begin=0,view_end=1;
  std::optional<ProfileTimeSelection> time_selection;
  ProfileTimelineView timeline_view;
  ProfilePanelControls controls;
  std::optional<std::uint64_t> range_anchor;
  std::array<char,128> profile_search{},memory_search{},error_search{};
  pvd::memory::Filter memory_filter;
  pvd::errors::Filter error_filter;
  pvd::memory::Snapshot memory_snapshot;
  std::optional<pvd::memory::WindowStats> memory_window;
  std::vector<pvd::memory::Allocation> memory_rows;
  std::vector<pvd::errors::Entry> error_rows;
  std::size_t m_prefix=std::numeric_limits<std::size_t>::max(),e_prefix=std::numeric_limits<std::size_t>::max();
  std::size_t memory_page=0,error_page=0,matches_memory=0,matches_errors=0;
  int group_by=3;
  std::string error;

  void bind(const Session* session) {
    if(source==session)return;
    source=session;profile.reset();memory.reset();errors.reset();
    memory_window.reset();
    time_selection.reset();range_anchor.reset();timeline_view={};controls={};
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
    controls={};
    ImGui::TextUnformatted("CPU PROFILER");ImGui::SameLine();
    if(ImGui::Checkbox("Current frame",&profile_current)){profile_dirty=true;view_manual=false;}
    same_line_if_fits(ui_pixels(185));ImGui::SetNextItemWidth(ui_pixels(185));
    const auto thread_name=thread?std::to_string(*thread):"All threads";
    if(ImGui::BeginCombo("##thread",thread_name.c_str())) {
      if(ImGui::Selectable("All threads",!thread)){thread.reset();profile_dirty=true;}
      for(auto id:threads)if(ImGui::Selectable(std::to_string(id).c_str(),thread&&*thread==id)){thread=id;profile_dirty=true;}
      ImGui::EndCombo();
    }
    same_line_if_fits(ui_pixels(190));ImGui::SetNextItemWidth(ui_pixels(190));
    std::string zone_name="All zones";for(const auto& z:profile->zones())if(zone&&z.first==*zone)zone_name=zone_label(z.second);
    const bool zone_open=ImGui::BeginCombo("##zone",zone_name.c_str());
    controls.zone_combo=control_rect();
    if(zone_open) {
      if(ImGui::Selectable("All zones",!zone)){zone.reset();profile_dirty=true;}
      for(const auto& z:profile->zones()){
        ImGui::PushID(hex(z.first.id).c_str());ImGui::PushID(hex(z.first.generation).c_str());
        if(ImGui::Selectable(zone_label(z.second).c_str(),zone&&*zone==z.first)){zone=z.first;profile_dirty=true;}
        ImGui::PopID();ImGui::PopID();
      }ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(ui_pixels(300));if(ImGui::InputTextWithHint("##profile-search","Filter event names...",profile_search.data(),profile_search.size()))profile_dirty=true;
    same_line_if_fits(button_width("Fit timeline"));if(ImGui::SmallButton("Fit timeline")){view_manual=false;profile_dirty=true;}
    pvd::profile::Query q;q.thread_id=thread;q.text=profile_search.data();
    if(zone){q.zone_id=zone->id;q.zone_generation=zone->generation;}
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
    same_line_if_fits(button_width("Export CSV..."));const bool export_csv=ImGui::Button("Export CSV...");controls.csv=control_rect();
    if(export_csv)save("profile.csv",[&](std::ostream& out){profile->write_csv(out,q);});
    const auto statistics=profile->statistics();
    ImGui::TextDisabled("%zu packets  |  %zu completed intervals  |  %zu open scopes%s",statistics.profile_packets,intervals.size(),statistics.open_scopes,statistics.complete?"":statistics.malformed_packets||statistics.saturated||statistics.skipped_gpu_events?"  |  INCOMPLETE DATA":"  |  Has diagnostics");
    if(!statistics.profile_packets){ImGui::TextWrapped("This capture has no profile channel. Enable PhysX PVD ePROFILE instrumentation in the producer.");return;}
    if(ImGui::BeginTabBar("profile-pages")) {
      if(ImGui::BeginTabItem("Thread timeline")){timeline(doc);ImGui::EndTabItem();}
      if(ImGui::BeginTabItem("Summary")) {
        if(ImGui::BeginTable("profile-summary",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y-ui_pixels(60)})){
          for(auto name:{"Event","Zone / thread","Count","Inclusive total","Mean","Max"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(aggregates.size()));
          while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& a=aggregates[std::size_t(row)];ImGui::TableNextRow();ImGui::TableNextColumn();text(a.event_name);ImGui::TableNextColumn();text(a.zone_name+" / "+(a.thread_id?std::to_string(*a.thread_id):"all"));ImGui::TableNextColumn();ImGui::Text("%zu",a.count);ImGui::TableNextColumn();text(duration(a.total_nanoseconds));ImGui::TableNextColumn();text(duration(a.mean_nanoseconds));ImGui::TableNextColumn();text(duration(a.maximum_nanoseconds));}
          ImGui::EndTable();
        }ImGui::EndTabItem();
      }
      if(ImGui::BeginTabItem("Intervals")) {
        if(ImGui::BeginTable("profile-intervals",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,ImGui::GetContentRegionAvail().y-ui_pixels(60)})) {
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
    if(time_selection) {
      const auto range=*time_selection;
      ImGui::TextWrapped("Measured range: %llu .. %llu native ticks | %llu ticks | %s",
          static_cast<unsigned long long>(range.begin_ticks),
          static_cast<unsigned long long>(range.end_ticks),
          static_cast<unsigned long long>(range.duration_ticks),duration(range.duration_nanoseconds).c_str());
      if(ImGui::SmallButton("Clear measured range")){time_selection.reset();range_anchor.reset();}
      same_line_if_fits(button_width("Export measured intervals CSV..."));
      const bool export_measured=ImGui::SmallButton("Export measured intervals CSV...");
      controls.measured_csv=control_rect();
      if(export_measured) {
        q.begin_ticks=range.begin_ticks;q.end_ticks=range.end_ticks;
        save("profile-range.csv",[&](std::ostream &out){profile->write_csv(out,q);});
      }
    }
    if(ImGui::CollapsingHeader("Profile diagnostics"))for(const auto& d:profile->diagnostics())ImGui::TextWrapped("Event %zu: %s",d.source.event_index,d.message.c_str());
  }
  void timeline(Document& doc) {
    std::map<Lane,std::size_t> depths;
    for(auto i:intervals){const auto& e=profile->intervals()[i];depths[lane(e)]=std::max(depths[lane(e)],std::min<std::size_t>(e.depth,15)+1);}
    std::map<Lane,float> lanes;float height=ui_pixels(30);
    for(const auto& lane:depths){lanes[lane.first]=height;height+=ui_pixels(25)+float(lane.second)*ui_pixels(19);}
    ImGui::TextWrapped("Wheel to zoom | Middle drag to pan | Shift+left drag to measure time across lanes | Double-click an interval to locate its received frame");
    ImGui::TextDisabled("Visible origin: %llu native ticks; axis is elapsed time from this origin",static_cast<unsigned long long>(view_begin));
    ImGui::BeginChild("thread-timeline",{0,std::max(ui_pixels(140),ImGui::GetContentRegionAvail().y-ui_pixels(90))},ImGuiChildFlags_Borders);
    const auto origin=ImGui::GetCursorScreenPos();const float width=std::max(ui_pixels(300),ImGui::GetContentRegionAvail().x),gutter=ui_pixels(240),graph=width-gutter;
    ImGui::InvisibleButton("timeline-canvas",{width,std::max(height,ImGui::GetContentRegionAvail().y)},ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    const auto hovered=ImGui::IsItemHovered(),active=ImGui::IsItemActive();
    const auto &io=ImGui::GetIO();
    auto* draw=ImGui::GetWindowDrawList();
    const long double span=std::max(1.0L,static_cast<long double>(view_end-view_begin));
    timeline_view={origin.x+gutter,origin.y,graph,ImGui::GetItemRectSize().y,view_begin,view_end};
    auto x=[&](std::uint64_t t){const auto delta=t>=view_begin?static_cast<long double>(t-view_begin):-static_cast<long double>(view_begin-t);return origin.x+gutter+float(delta/span)*graph;};
    const auto mouse_tick=[&] {
      const auto fraction=std::clamp((io.MousePos.x-origin.x-gutter)/graph,0.f,1.f);
      const auto offset=std::min(view_end-view_begin,tick(span*fraction));
      return view_begin+offset;
    };
    if(hovered&&io.KeyShift&&ImGui::IsMouseClicked(0)&&io.MousePos.x>=origin.x+gutter)
      range_anchor=mouse_tick();
    const bool measuring=range_anchor.has_value();
    if(range_anchor) {
      const auto at=mouse_tick(),begin=std::min(*range_anchor,at),end=std::max(*range_anchor,at);
      time_selection=ProfileTimeSelection{begin,end,end-begin,profile->nanoseconds(end-begin)};
      if(!ImGui::IsMouseDown(0))range_anchor.reset();
    }
    for(int i=0;i<=5;++i){const float at=origin.x+gutter+graph*float(i)/5;draw->AddLine({at,origin.y},{at,origin.y+height},IM_COL32(42,55,70,255));auto label=duration(profile->nanoseconds(tick(span*i/5)));draw->AddText({at+ui_pixels(3),origin.y},IM_COL32(115,139,160,255),label.c_str());}
    for(const auto& entry:lanes){const auto label="T"+std::to_string(std::get<0>(entry.first))+"  "+profile->zone_name({std::get<1>(entry.first),std::get<2>(entry.first)})+" / "+hex(std::get<3>(entry.first));draw->PushClipRect(origin,{origin.x+gutter-ui_pixels(5),origin.y+height},true);draw->AddText({origin.x,origin.y+entry.second},IM_COL32(195,213,225,255),label.c_str());draw->PopClipRect();}
    std::size_t painted=0;
    for(auto i:intervals){const auto& e=profile->intervals()[i];if(e.end_ticks<view_begin||e.begin_ticks>view_end)continue;if(++painted>100000)break;
      const float y=origin.y+lanes[lane(e)]+float(std::min<std::size_t>(e.depth,15))*ui_pixels(19);
      if(y+ui_pixels(17)<ImGui::GetWindowPos().y||y>ImGui::GetWindowPos().y+ImGui::GetWindowHeight())continue;
      const ImVec2 a{std::clamp(x(e.begin_ticks),origin.x+gutter,origin.x+width),y},b{std::clamp(std::max(x(e.end_ticks),a.x+ui_pixels(2)),a.x+ui_pixels(2),origin.x+width+ui_pixels(2)),y+ui_pixels(17)};
      const auto color=ImColor::HSV(float((e.event_id*37u)%255u)/255.0f,.50f,.68f);
      draw->AddRectFilled(a,b,color,ui_pixels(2));
      if(selected_interval&&*selected_interval==i)draw->AddRect(a,b,IM_COL32(255,218,90,255),ui_pixels(2),0,ui_pixels(2));
      if(b.x-a.x>ui_pixels(45)){const auto label=profile->event_name(e.start_source.zone,e.event_id);draw->PushClipRect(a,b,true);draw->AddText({a.x+ui_pixels(3),a.y},IM_COL32(245,248,250,255),label.c_str());draw->PopClipRect();}
      if(hovered&&!measuring&&!io.KeyShift&&ImGui::IsMouseHoveringRect(a,b)){
        ImGui::SetTooltip("%s\n%s\nThread %u / context %s\n%llu native ticks",profile->event_name(e.start_source.zone,e.event_id).c_str(),duration(e.duration_nanoseconds).c_str(),e.thread_id,hex(e.context_id).c_str(),static_cast<unsigned long long>(e.duration_ticks));
        if(ImGui::IsMouseClicked(0))selected_interval=i;
        if(ImGui::IsMouseDoubleClicked(0))jump(doc,e.stop_source.event_index);
      }
    }
    if(time_selection) {
      const auto left=std::clamp(x(time_selection->begin_ticks),origin.x+gutter,origin.x+width);
      const auto right=std::clamp(x(time_selection->end_ticks),origin.x+gutter,origin.x+width);
      draw->AddRectFilled({left,origin.y},{right,origin.y+timeline_view.height},IM_COL32(255,213,85,38));
      draw->AddLine({left,origin.y},{left,origin.y+timeline_view.height},IM_COL32(255,213,85,220));
      draw->AddLine({right,origin.y},{right,origin.y+timeline_view.height},IM_COL32(255,213,85,220));
    }
    if(hovered&&!measuring&&io.MouseWheel!=0) {
      const auto anchor=std::clamp((io.MousePos.x-origin.x-gutter)/graph,0.f,1.f);
      const auto next=std::max<std::uint64_t>(1,tick(span*std::exp(-io.MouseWheel*.20)));
      const auto pivot=view_begin+std::min(view_end-view_begin,tick(span*anchor));
      const auto left=tick(static_cast<long double>(next)*anchor);
      const auto maximum=std::numeric_limits<std::uint64_t>::max();
      view_begin=std::min(pivot>=left?pivot-left:0,maximum-next);
      view_end=view_begin+next;view_manual=true;
    }
    if(active&&!measuring&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
      const auto delta=-io.MouseDelta.x/graph*span;
      if(delta>=0) {
        const auto shift=std::min(std::numeric_limits<std::uint64_t>::max()-view_end,tick(delta));
        view_begin+=shift;view_end+=shift;
      }else {
        const auto shift=std::min(view_begin,tick(-delta));view_begin-=shift;view_end-=shift;
      }
      view_manual=true;
    }
    ImGui::EndChild();
    if(painted>100000)ImGui::TextDisabled("More than 100,000 visible intervals; zoom or filter by thread/zone.");
  }
  void memory_view(Document& doc,const CsvExport& save) {
    ImGui::TextUnformatted("MEMORY");ImGui::SameLine();if(ImGui::Checkbox("At current frame",&memory_current))memory_dirty=true;
    ImGui::SameLine();if(ImGui::Checkbox("Active only",&memory_filter.active_only)){memory_page=0;memory_dirty=true;}
    ImGui::SetNextItemWidth(ui_pixels(300));if(ImGui::InputTextWithHint("##memory-search","History filter: type, file, address...",memory_search.data(),memory_search.size())){memory_filter.text=memory_search.data();memory_page=0;memory_dirty=true;}
    ImGui::SameLine();ImGui::SetNextItemWidth(ui_pixels(200));const char* groups[]={"Type","File","File and line","Type, file and line"};if(ImGui::Combo("Group by",&group_by,groups,4))memory_dirty=true;
    const auto count=prefix(doc,memory_current);if(m_prefix!=count){m_prefix=count;memory_dirty=true;}
    if(memory_dirty){memory_snapshot=memory->summary_at_event(count,static_cast<pvd::memory::GroupBy>(group_by));std::sort(memory_snapshot.groups.begin(),memory_snapshot.groups.end(),[](const auto& a,const auto& b){return a.stats.current_bytes>b.stats.current_bytes;});matches_memory=memory->matching_count(count,memory_filter);memory_page=std::min(memory_page,matches_memory?(matches_memory-1)/256:0);memory_rows=memory->history_at_event(count,memory_filter,memory_page*256,256);memory_dirty=false;}
    ImGui::SameLine();if(ImGui::Button("Export allocations CSV..."))save("memory.csv",[&](std::ostream& out){memory->write_csv(out,count,memory_filter);});
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Totals and groups cover all allocations before event %zu. History filters apply to allocation rows and their CSV.",count);
    const auto& s=memory_snapshot.totals;
    ImGui::Text("Live %.3f MiB (%zu allocations)  |  Peak %.3f MiB  |  Allocated %.3f MiB  |  Freed %.3f MiB",double(s.current_bytes)/(1024*1024),s.current_allocations,double(s.peak_bytes)/(1024*1024),double(s.allocated_bytes)/(1024*1024),double(s.freed_bytes)/(1024*1024));
    if(!memory_snapshot.complete)ImGui::TextColored({1,.65f,.35f,1},"Incomplete memory data; inspect diagnostics before interpreting totals.");
    ImGui::PopTextWrapPos();
    std::optional<pvd::FrameMarker> native;
    if(doc.scene_id()&&doc.frame())for(const auto& frame:source->frames())
      if(frame.scene_id==*doc.scene_id()&&frame.event_count<=doc.frame()->snapshot.event_count&&
         (!native||frame.event_count>native->event_count))native=frame;
    if(native) {
      const auto window=pvd::memory::native_frame_event_window(*source,*native);
      if(!memory_window||memory_window->window.begin_event_count!=window.begin_event_count||
         memory_window->window.end_event_count!=window.end_event_count||memory_dirty)
        memory_window=memory->window_statistics(window);
      const auto& stats=*memory_window;
      const auto& flow=stats.activity;
      ImGui::Separator();
      ImGui::TextUnformatted("RECEIVED IN SELECTED NATIVE FRAME");
      ImGui::TextWrapped("Global memory events received in [%zu, %zu), native scene %llu / stream %llu. Memory buffers have no scene ownership or allocation timestamps; this is not memory used by that scene or time spent inside simulation.",window.begin_event_count,window.end_event_count,static_cast<unsigned long long>(native->scene_id),static_cast<unsigned long long>(native->stream_id));
      if(doc.sample_point()==SamplePoint::after_fetch)
        ImGui::TextWrapped("After-fetch view: activity ends at the preceding native EndSection. Later flushed packets belong to a later receipt window.");
      ImGui::TextWrapped("Outstanding(native end): %llu B / %zu allocations | Peak(as of native end): %llu B",static_cast<unsigned long long>(stats.as_of_end.current_bytes),stats.as_of_end.current_allocations,static_cast<unsigned long long>(stats.as_of_end.peak_bytes));
      ImGui::TextWrapped("Allocated(frame): %llu B / %zu events | Freed(frame): %llu known B / %zu events (%zu unknown sizes)",static_cast<unsigned long long>(flow.allocated_bytes),flow.allocation_count,static_cast<unsigned long long>(flow.freed_bytes),flow.free_count,flow.unknown_free_count);
      ImGui::TextWrapped("Churn(frame, allocated + known freed): %s B | Net(frame, allocated - known freed): %s B",flow.churn_bytes().c_str(),flow.net_bytes().c_str());
      if(flow.replacement_count)
        ImGui::TextWrapped("Replaced without observed free: %llu B / %zu events (separate from freed and net)",static_cast<unsigned long long>(flow.replaced_bytes),flow.replacement_count);
      if(!stats.has_memory_channel)
        ImGui::TextWrapped("No memory channel observed before this boundary; zero received events do not prove zero allocations.");
      else if(!stats.complete)
        ImGui::TextColored({1,.65f,.35f,1},"Partial observed accounting; unknown free sizes and quarantined data are not reconstructed.");
    } else {
      memory_window.reset();
      ImGui::TextWrapped("Per-frame activity unavailable: no preceding native frame boundary for the selected scene.");
    }
    if(memory->events().empty()&&memory->diagnostics().empty()){ImGui::TextWrapped("No memory channel was captured. Enable PhysX PVD eMEMORY instrumentation in the producer.");return;}
    if(ImGui::BeginTabBar("memory-pages")){
      if(ImGui::BeginTabItem("Allocation history")){
        page_buttons(memory_page,matches_memory,memory_dirty);
        if(ImGui::BeginTable("allocation-history",7,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,{0,ImGui::GetContentRegionAvail().y-ui_pixels(60)})){
          for(auto name:{"Address / generation","Bytes","Type","Source","Line","State","Allocated event"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(memory_rows.size()));while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& a=memory_rows[std::size_t(row)];ImGui::PushID(row);ImGui::TableNextRow();ImGui::TableNextColumn();if(ImGui::Selectable((hex(a.address)+" / "+std::to_string(a.generation)).c_str(),false,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowDoubleClick)&&ImGui::IsMouseDoubleClicked(0))jump(doc,a.allocated_event_count? a.allocated_event_count-1:0);ImGui::TableNextColumn();text(std::to_string(a.size));ImGui::TableNextColumn();text(a.type_name.value_or("Unresolved string"));ImGui::TableNextColumn();text(a.file.value_or("Unresolved string"));ImGui::TableNextColumn();ImGui::Text("%u",a.line);ImGui::TableNextColumn();text(!a.ended_event_count||*a.ended_event_count>count?"Active":a.end_reason==pvd::memory::EndReason::freed?"Freed":"Replaced without free");ImGui::TableNextColumn();ImGui::Text("%zu",a.allocated_event_count);ImGui::PopID();}ImGui::EndTable();
        }ImGui::EndTabItem();
      }
      if(ImGui::BeginTabItem("Groups")){
        if(ImGui::Button("Export groups CSV..."))save("memory-groups.csv",[&](std::ostream& out){memory->write_groups_csv(out,count,static_cast<pvd::memory::GroupBy>(group_by));});
        if(ImGui::BeginTable("memory-groups",6,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,{0,ImGui::GetContentRegionAvail().y-ui_pixels(60)})){
          for(auto name:{"Type","File / line","Current bytes","Peak bytes","Live count","Total allocated"})ImGui::TableSetupColumn(name);ImGui::TableHeadersRow();
          ImGuiListClipper clipper;clipper.Begin(static_cast<int>(memory_snapshot.groups.size()));while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const auto& a=memory_snapshot.groups[std::size_t(row)];ImGui::TableNextRow();ImGui::TableNextColumn();text(a.type_name.value_or(group_by==0||group_by==3?"Unresolved type":"All types"));ImGui::TableNextColumn();text(a.file.value_or(group_by!=0?"Unresolved file":"All files")+(group_by==2||group_by==3?":"+std::to_string(a.line):""));ImGui::TableNextColumn();text(std::to_string(a.stats.current_bytes));ImGui::TableNextColumn();text(std::to_string(a.stats.peak_bytes));ImGui::TableNextColumn();text(std::to_string(a.stats.current_allocations));ImGui::TableNextColumn();text(std::to_string(a.stats.allocated_bytes));}ImGui::EndTable();
        }ImGui::EndTabItem();
      }ImGui::EndTabBar();
    }
    diagnostics(memory_snapshot.diagnostics);
  }
  void error_view(Document& doc,const CsvExport& save) {
    ImGui::TextUnformatted("ERROR STREAM");ImGui::SameLine();if(ImGui::Checkbox("Up to current frame",&error_current))error_dirty=true;
    ImGui::SetNextItemWidth(ui_pixels(350));if(ImGui::InputTextWithHint("##error-search","Filter message or file...",error_search.data(),error_search.size())){error_filter.text=error_search.data();error_page=0;error_dirty=true;}
    ImGui::SameLine();ImGui::SetNextItemWidth(ui_pixels(210));
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
std::optional<memory::WindowStats> AnalysisPanels::memory_window_statistics()const {
  return impl_->memory_window;
}
std::optional<ProfileTimeSelection> AnalysisPanels::profile_time_selection()const {return impl_->time_selection;}
ProfileTimelineView AnalysisPanels::profile_timeline_view()const noexcept {return impl_->timeline_view;}
ProfilePanelControls AnalysisPanels::profile_controls()const noexcept {return impl_->controls;}
} // namespace pvd::viewer
