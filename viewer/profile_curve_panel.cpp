#include "profile_curve_panel.hpp"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace pvd::viewer {
namespace {
namespace a = pvd::analysis;
constexpr std::size_t curve_limit = 16, sample_limit = 2000;
std::string hex(std::uint64_t n) {
  std::ostringstream s;
  s << "0x" << std::hex << std::uppercase << n;
  return s.str();
}
std::string clipped(const std::string &s, std::size_t size = 200) {
  if (s.size() <= size)
    return s;
  while (size && (static_cast<unsigned char>(s[size]) & 0xc0) == 0x80)
    --size;
  return s.substr(0, size) + "...";
}
void text(const std::string &s) { ImGui::TextUnformatted(s.c_str()); }
int unit(a::ProfileMetric metric) {
  return metric == a::ProfileMetric::count               ? 0
         : metric >= a::ProfileMetric::total_nanoseconds ? 2
                                                         : 1;
}
const char *unit_name(a::ProfileMetric metric) {
  return unit(metric) == 0 ? "count" : unit(metric) == 1 ? "ticks" : "ns";
}
std::optional<double> sample_x(const a::ProfileCurveSample &s, bool time) {
  return time ? s.elapsed_seconds
              : std::make_optional(static_cast<double>(s.source.frame_index) +
                                   1);
}
void pad_range(double &lo, double &hi) {
  const auto margin =
      lo == hi ? std::max(1.0, std::abs(lo) * .05) : (hi - lo) * .08;
  if (std::isfinite(margin)) {
    lo -= margin;
    hi += margin;
  }
}
void source_for(a::ProfileCurveRequest &r, const Document &d) {
  r.source.scene_id = d.scene_id().value_or(0);
  r.source.sampling = a::Sampling::native_frame;
}
std::string label_for(const a::ProfileCurveRequest &r,
                      const profile::Index *index) {
  const auto &f = r.filter;
  std::string label =
      f.zone ? (index ? clipped(index->zone_name(*f.zone), 60) + " " : "") +
                   hex(f.zone->id) + "/" + std::to_string(f.zone->generation)
             : "All zones";
  if (f.event_id)
    label += (f.zone && index
                  ? " " + clipped(index->event_name(*f.zone, *f.event_id), 60)
                  : " event") +
             " #" + std::to_string(*f.event_id);
  else
    label += " all events";
  if (f.thread_id)
    label += " thread " + std::to_string(*f.thread_id);
  if (f.context_id)
    label += " context " + hex(*f.context_id);
  if (!f.text.empty())
    label += " name~" + clipped(f.text, 24);
  return label + " | " + a::profile_metric_name(r.metric);
}

} // namespace
struct ProfileCurvePanel::Impl {
  struct Entry {
    std::size_t serial = 0;
    std::string label;
    bool auto_label = true, visible = true;
    std::array<float, 4> color{.3f, .8f, 1, 1};
    a::ProfileCurveRequest request;
    a::ProfileCurveSeries data;
  };
  struct ZoneChoice {
    profile::ZoneKey key;
    std::string name;
  };
  struct EventChoice {
    profile::ZoneKey zone;
    std::uint16_t id;
    std::string name;
  };
  const Session *source = nullptr;
  std::unique_ptr<a::ProfileCurves> index;
  std::optional<std::uint64_t> scene;
  std::vector<Entry> entries;
  std::size_t serial = 0, selected = 0, rows = 0, curve_visible = 0,
              seen_events = 0;
  std::vector<ZoneChoice> zones;
  std::vector<EventChoice> events;
  std::set<std::uint32_t> thread_set;
  std::set<std::uint64_t> context_set;
  std::vector<std::uint32_t> threads;
  std::vector<std::uint64_t> contexts;
  bool ids_truncated = false;
  bool choices_truncated = false, context_dirty = false, auto_refresh = false,
       time_axis = false, manual_x = false, auto_y = true;
  double xlo = 1, xhi = 2, ylo = 0, yhi = 1;
  ProfileCurvePlotView plot;
  std::optional<std::pair<std::size_t, std::size_t>> selected_sample, click;
  std::chrono::steady_clock::time_point last_refresh{};
  std::string error;
  a::ProfileCurveRequest editor;
  std::array<char, 192> filter_text{};
  bool editing = false;
  void count_rows() {
    rows = 0;
    for (const auto &e : entries)
      rows += e.data.samples.size();
  }
  bool plotted(const Entry &e) const {
    return e.visible && !entries.empty() &&
           unit(e.request.metric) == unit(entries[selected].request.metric);
  }
  void choose_series(std::size_t i) {
    if (i >= entries.size())
      throw std::out_of_range("Profile curve index");
    if (unit(entries[i].request.metric) !=
        unit(entries[selected].request.metric))
      auto_y = true;
    selected = i;
    selected_sample.reset();
  }
  void choices() {
    zones.clear();
    events.clear();
    choices_truncated = ids_truncated;
    const auto &profile = index->profile_index();
    for (const auto &z : profile.zones()) {
      zones.push_back({z.first, z.second.name});
      for (const auto &e : z.second.event_names) {
        if (events.size() >= 8192) {
          choices_truncated = true;
          break;
        }
        events.push_back({z.first, e.first, e.second.name});
      }
    }
    const auto &raw = profile.events();
    for (; seen_events < raw.size(); ++seen_events) {
      const auto &e = raw[seen_events];
      if (e.kind != profile::Kind::start && e.kind != profile::Kind::stop)
        continue;
      if (thread_set.size() < 4096 || thread_set.count(e.thread_id))
        thread_set.insert(e.thread_id);
      else
        choices_truncated = ids_truncated = true;
      if (context_set.size() < 8192 || context_set.count(e.context_id))
        context_set.insert(e.context_id);
      else
        choices_truncated = ids_truncated = true;
    }
    threads.assign(thread_set.begin(), thread_set.end());
    contexts.assign(context_set.begin(), context_set.end());
  }
  void update_index(Document &doc) {
    if (!source || !index)
      return;
    if (!source->at_transaction_boundary())
      throw std::logic_error(
          "Refresh waits for a complete property transaction");
    index->update(*source);
    choices();
    (void)doc;
  }
  void resample(Document &doc) {
    update_index(doc);
    a::ProfileCurveOptions limits;
    limits.max_samples = sample_limit;
    for (auto &e : entries) {
      auto data = index->sample(e.request, limits);
      e.data = std::move(data);
    }
    last_refresh = std::chrono::steady_clock::now();
    context_dirty = false;
    selected_sample.reset();
    count_rows();
    error.clear();
  }
  void bind(Document &doc) {
    if (source != doc.session()) {
      source = doc.session();
      index = source ? std::make_unique<a::ProfileCurves>() : nullptr;
      entries.clear();
      zones.clear();
      events.clear();
      threads.clear();
      contexts.clear();
      thread_set.clear();
      context_set.clear();
      serial = selected = rows = curve_visible = seen_events = 0;
      ids_truncated = false;
      selected_sample.reset();
      plot = {};
      manual_x = false;
      auto_y = true;
      error.clear();
      editor = {};
      filter_text[0] = 0;
      editing = false;
      scene = doc.scene_id();
      context_dirty = true;
    }
    if (scene != doc.scene_id()) {
      scene = doc.scene_id();
      for (auto &e : entries)
        source_for(e.request, doc);
      context_dirty = true;
      manual_x = false;
    }
    if (context_dirty && source && source->at_transaction_boundary())
      resample(doc);
  }
  void legend() {
    if (!ImGui::BeginTable("Profile curve list", 4,
                           ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_BordersInnerH))
      return;
    ImGui::TableSetupColumn("Show", 0, 35);
    ImGui::TableSetupColumn("Color", 0, 30);
    ImGui::TableSetupColumn("Curve", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Remove", 0, 22);
    std::optional<std::size_t> remove;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      auto &e = entries[i];
      ImGui::PushID(static_cast<int>(e.serial));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Checkbox("##show", &e.visible);
      ImGui::TableNextColumn();
      ImGui::ColorEdit4("##color", e.color.data(),
                        ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_NoLabel |
                            ImGuiColorEditFlags_AlphaBar);
      ImGui::TableNextColumn();
      if (ImGui::Selectable(e.label.c_str(), selected == i))
        choose_series(i);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%zu samples / %zu native windows",
                          e.label.c_str(), e.data.samples.size(),
                          e.data.candidate_samples);
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("X"))
        remove = i;
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove) {
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*remove));
      selected = entries.empty() ? 0 : std::min(selected, entries.size() - 1);
      selected_sample.reset();
      auto_y = true;
      count_rows();
    }
  }
  bool plot_pressed = false, plot_dragged = false;
  ImVec2 press_pos{};
  void draw_plot() {
    curve_visible = 0;
    plot = {};
    click.reset();
    bool any = false;
    double xmin = 0, xmax = 1, ymin = 0, ymax = 1;
    for (const auto &e : entries)
      if (plotted(e))
        for (const auto &s : e.data.samples)
          if (s.value) {
            const auto x = sample_x(s, time_axis);
            const auto y = a::plot_value(*s.value);
            if (!x || !std::isfinite(*x) || !std::isfinite(y))
              continue;
            if (!any) {
              xmin = xmax = *x;
              ymin = ymax = y;
              any = true;
            } else {
              xmin = std::min(xmin, *x);
              xmax = std::max(xmax, *x);
              ymin = std::min(ymin, y);
              ymax = std::max(ymax, y);
            }
          }
    if (!any) {
      ImGui::TextWrapped(
          time_axis
              ? "No timed numeric samples. Clock calibration or numeric "
                "values are unavailable."
              : "No numeric samples in the selected native frame windows. "
                " Inspect coverage and profile diagnostics below.");
      return;
    }
    if (!manual_x) {
      xlo = xmin;
      xhi = xmax;
      pad_range(xlo, xhi);
      if (!time_axis)
        xlo = std::max(1.0, xlo);
    }
    if (auto_y) {
      bool y_any = false;
      for (const auto &e : entries)
        if (plotted(e))
          for (const auto &s : e.data.samples)
            if (s.value) {
              auto x = sample_x(s, time_axis);
              const auto y = a::plot_value(*s.value);
              if (x && *x >= xlo && *x <= xhi && std::isfinite(y)) {
                if (!y_any) {
                  ymin = ymax = y;
                  y_any = true;
                } else {
                  ymin = std::min(ymin, y);
                  ymax = std::max(ymax, y);
                }
              }
            }
      ylo = ymin;
      yhi = ymax;
      pad_range(ylo, yhi);
    }
    if (!std::isfinite(xlo) || !std::isfinite(xhi) || !std::isfinite(ylo) ||
        !std::isfinite(yhi) || !(xhi > xlo) || !(yhi > ylo) ||
        !std::isfinite(xhi - xlo) || !std::isfinite(yhi - ylo)) {
      ImGui::TextWrapped("Plot range is invalid or exceeds double precision. "
                         "Use Fit view or a finite manual range.");
      return;
    }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(160.f, ImGui::GetContentRegionAvail().x),
                height = 260;
    const ImVec2 lo{start.x + 55, start.y + 12},
        hi{start.x + width - 12, start.y + height - 30};
    plot = {lo.x,
            lo.y,
            hi.x - lo.x,
            hi.y - lo.y,
            xlo,
            xhi,
            ylo,
            yhi,
            time_axis,
            true,
            unit_name(entries[selected].request.metric)};
    ImGui::InvisibleButton("Profile curve canvas", {width, height},
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonMiddle);
    plot.unit = unit_name(entries[selected].request.metric);
    const bool hovered = ImGui::IsItemHovered();
    auto &io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
      const double anchor =
          xlo + std::clamp(
                    static_cast<double>((io.MousePos.x - lo.x) / (hi.x - lo.x)),
                    0.0, 1.0) *
                    (xhi - xlo);
      const double factor = std::pow(
          1.25, -std::clamp(static_cast<double>(io.MouseWheel), -10.0, 10.0));
      const double a0 = anchor + (xlo - anchor) * factor,
                   b0 = anchor + (xhi - anchor) * factor;
      if (std::isfinite(a0) && std::isfinite(b0) && b0 > a0) {
        xlo = a0;
        xhi = b0;
        manual_x = true;
      }
    }
    if (ImGui::IsItemActivated()) {
      plot_pressed = true;
      plot_dragged = false;
      press_pos = io.MousePos;
    }
    if (ImGui::IsItemActive() && plot_pressed) {
      const float dx = io.MousePos.x - press_pos.x,
                  dy = io.MousePos.y - press_pos.y;
      if (dx * dx + dy * dy > 16)
        plot_dragged = true;
      if (plot_dragged) {
        const double shift =
            -static_cast<double>(io.MouseDelta.x) / (hi.x - lo.x) * (xhi - xlo);
        if (std::isfinite(xlo + shift) && std::isfinite(xhi + shift)) {
          xlo += shift;
          xhi += shift;
          manual_x = true;
        }
        if (!auto_y) {
          const double sy = static_cast<double>(io.MouseDelta.y) /
                            (hi.y - lo.y) * (yhi - ylo);
          if (std::isfinite(ylo + sy) && std::isfinite(yhi + sy)) {
            ylo += sy;
            yhi += sy;
          }
        }
      }
    }
    // Frame numbers are one-based in the UI, including zoom/pan ranges.
    // The sample retains its zero-based source index for exact seeks.
    if (!time_axis && xlo < 1.0) {
      xhi += 1.0 - xlo;
      xlo = 1.0;
    }
    plot.x_min = xlo;
    plot.x_max = xhi;
    plot.y_min = ylo;
    plot.y_max = yhi;
    const bool released =
        plot_pressed && (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseReleased(ImGuiMouseButton_Middle));
    const bool choose = released &&
                        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                        hovered && !plot_dragged;
    if (released)
      plot_pressed = false;
    auto *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, IM_COL32(18, 23, 31, 255));
    for (int i = 0; i <= 4; ++i) {
      const float t = static_cast<float>(i) / 4;
      dl->AddLine({lo.x, lo.y + (hi.y - lo.y) * t},
                  {hi.x, lo.y + (hi.y - lo.y) * t}, IM_COL32(53, 61, 70, 255));
      char label[48];
      std::snprintf(label, sizeof label, "%.4g", yhi - (yhi - ylo) * t);
      dl->AddText({start.x, lo.y + (hi.y - lo.y) * t - 6},
                  IM_COL32(180, 185, 195, 255), label);
      std::snprintf(label, sizeof label, "%.4g", xlo + (xhi - xlo) * t);
      dl->AddText({lo.x + (hi.x - lo.x) * t - 10, hi.y + 4},
                  IM_COL32(180, 185, 195, 255), label);
    }
    dl->AddRect(lo, hi, IM_COL32(100, 111, 125, 255));
    dl->PushClipRect(lo, hi, true);
    float nearest = 100;
    std::optional<std::pair<std::size_t, std::size_t>> nearest_sample;
    for (std::size_t ei = 0; ei < entries.size(); ++ei) {
      const auto &e = entries[ei];
      if (!plotted(e))
        continue;
      const auto color = ImGui::ColorConvertFloat4ToU32(
          {e.color[0], e.color[1], e.color[2], e.color[3]});
      std::optional<ImVec2> previous;
      for (std::size_t si = 0; si < e.data.samples.size(); ++si) {
        const auto &s = e.data.samples[si];
        const auto x = sample_x(s, time_axis);
        if (!s.value || !x) {
          previous.reset();
          continue;
        }
        const double y = a::plot_value(*s.value);
        if (!std::isfinite(*x) || !std::isfinite(y)) {
          previous.reset();
          continue;
        }
        // Bound screen coordinates before narrowing to float; huge offscreen
        // values cannot overflow ImDrawList.
        const double px = std::clamp((*x - xlo) / (xhi - xlo), -4.0, 5.0),
                     py = std::clamp((y - ylo) / (yhi - ylo), -4.0, 5.0);
        ImVec2 p{lo.x + static_cast<float>(px) * (hi.x - lo.x),
                 hi.y - static_cast<float>(py) * (hi.y - lo.y)};
        if (previous && !s.break_before)
          dl->AddLine(*previous, p, color, 1.8f);
        previous = p;
        if (*x >= xlo && *x <= xhi && y >= ylo && y <= yhi) {
          ++curve_visible;
          if (e.data.samples.size() <= 200)
            dl->AddCircleFilled(p, 3, color);
          const float dx = p.x - io.MousePos.x, dy = p.y - io.MousePos.y,
                      dist = dx * dx + dy * dy;
          if (dist < nearest) {
            nearest = dist;
            nearest_sample = {{ei, si}};
          }
          if (selected_sample == std::make_optional(std::make_pair(ei, si)))
            dl->AddCircle(p, 6, IM_COL32_WHITE, 0, 1.5f);
        }
      }
    }
    dl->PopClipRect();
    if (hovered && nearest_sample) {
      const auto &e = entries[nearest_sample->first];
      const auto &s = e.data.samples[nearest_sample->second];
      ImGui::BeginTooltip();
      text(e.label);
      ImGui::Text("Frame %zu | prefix %zu | ticks %llu",
                  s.source.frame_index + 1, s.source.event_count,
                  static_cast<unsigned long long>(s.source.timestamp_ticks));
      text("Exact value: " + s.exact_value);
      ImGui::Text("%s%s", a::profile_state_name(s.state),
                  s.provisional ? " (provisional)" : "");
      ImGui::EndTooltip();
      if (choose) {
        click = nearest_sample;
        selected_sample = nearest_sample;
      }
    }
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "%s | Wheel: zoom  Drag: pan  Click a point: seek original frame",
        time_axis ? "Elapsed native seconds" : "Native scene frame");
    ImGui::PopStyleColor();
  }

  bool open_editor = false;
  void load_editor() {
    if (entries.empty())
      return;
    editor = entries[selected].request;
    std::snprintf(filter_text.data(), filter_text.size(), "%s",
                  editor.filter.text.c_str());
    editing = true;
    open_editor = true;
  }
  void draw_editor(Document &doc, ProfileCurvePanel &owner) {
    if (open_editor) {
      ImGui::SetNextItemOpen(true);
      open_editor = false;
    }
    if (!ImGui::CollapsingHeader(
            "Choose CPU scope / metric",
            entries.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
      return;
    auto &f = editor.filter;
    std::string zone_label = "All advertised zones";
    if (f.zone)
      zone_label = index ? index->profile_index().zone_name(*f.zone) +
                               " / generation " +
                               std::to_string(f.zone->generation)
                         : hex(f.zone->id);
    ImGui::SetNextItemWidth(
        std::max(100.f, ImGui::GetContentRegionAvail().x - 85));
    if (ImGui::BeginCombo("Zone", clipped(zone_label).c_str())) {
      if (ImGui::Selectable("All zones", !f.zone)) {
        f.zone.reset();
        f.event_id.reset();
      }
      ImGuiListClipper list;
      list.Begin(static_cast<int>(zones.size()));
      while (list.Step())
        for (int n = list.DisplayStart; n < list.DisplayEnd; ++n) {
          const auto &z = zones[static_cast<std::size_t>(n)];
          ImGui::PushID(n);
          const auto label = clipped(z.name) + " " + hex(z.key.id) + " / " +
                             std::to_string(z.key.generation);
          if (ImGui::Selectable(label.c_str(), f.zone && *f.zone == z.key)) {
            f.zone = z.key;
            f.event_id.reset();
          }
          ImGui::PopID();
        }
      ImGui::EndCombo();
    }
    std::string event_label = "All CPU events";
    if (f.event_id)
      event_label =
          f.zone && index
              ? index->profile_index().event_name(*f.zone, *f.event_id) +
                    " (#" + std::to_string(*f.event_id) + ")"
              : "Event #" + std::to_string(*f.event_id);
    ImGui::SetNextItemWidth(
        std::max(100.f, ImGui::GetContentRegionAvail().x - 85));
    if (ImGui::BeginCombo("Event", clipped(event_label).c_str())) {
      if (ImGui::Selectable("All events", !f.event_id))
        f.event_id.reset();
      for (std::size_t n = 0; n < events.size(); ++n) {
        const auto &e = events[n];
        if (f.zone && !(*f.zone == e.zone))
          continue;
        ImGui::PushID(static_cast<int>(n));
        const auto label = clipped(e.name) + " (#" + std::to_string(e.id) +
                           ")" +
                           (f.zone ? ""
                                   : " " + hex(e.zone.id) + "/" +
                                         std::to_string(e.zone.generation));
        if (ImGui::Selectable(label.c_str(), f.event_id &&
                                                 *f.event_id == e.id &&
                                                 f.zone && *f.zone == e.zone)) {
          f.zone = e.zone;
          f.event_id = e.id;
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(
        std::max(100.f, ImGui::GetContentRegionAvail().x - 85));
    if (ImGui::BeginCombo("Thread", f.thread_id
                                        ? std::to_string(*f.thread_id).c_str()
                                        : "All observed CPU threads")) {
      if (ImGui::Selectable("All threads", !f.thread_id))
        f.thread_id.reset();
      ImGuiListClipper list;
      list.Begin(static_cast<int>(threads.size()));
      while (list.Step())
        for (int n = list.DisplayStart; n < list.DisplayEnd; ++n)
          if (ImGui::Selectable(
                  std::to_string(threads[static_cast<std::size_t>(n)]).c_str(),
                  f.thread_id &&
                      *f.thread_id == threads[static_cast<std::size_t>(n)]))
            f.thread_id = threads[static_cast<std::size_t>(n)];
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(
        std::max(100.f, ImGui::GetContentRegionAvail().x - 85));
    if (ImGui::BeginCombo("Context", f.context_id ? hex(*f.context_id).c_str()
                                                  : "All observed contexts")) {
      if (ImGui::Selectable("All contexts", !f.context_id))
        f.context_id.reset();
      ImGuiListClipper list;
      list.Begin(static_cast<int>(contexts.size()));
      while (list.Step())
        for (int n = list.DisplayStart; n < list.DisplayEnd; ++n)
          if (ImGui::Selectable(
                  hex(contexts[static_cast<std::size_t>(n)]).c_str(),
                  f.context_id &&
                      *f.context_id == contexts[static_cast<std::size_t>(n)]))
            f.context_id = contexts[static_cast<std::size_t>(n)];
      ImGui::EndCombo();
    }
    const char *metrics[] = {"Completed count", "Total ticks (inclusive)",
                             "Mean ticks",      "Minimum ticks",
                             "Maximum ticks",   "Total ns (inclusive)",
                             "Mean ns",         "Minimum ns",
                             "Maximum ns"};
    int metric = static_cast<int>(editor.metric);
    ImGui::SetNextItemWidth(
        std::max(100.f, ImGui::GetContentRegionAvail().x - 85));
    if (ImGui::Combo("Metric", &metric, metrics, 9))
      editor.metric = static_cast<a::ProfileMetric>(metric);
    bool covered = editor.coverage == a::ProfileCoverage::assume_continuous;
    if (ImGui::Checkbox("Assume continuous collection", &covered))
      editor.coverage = covered ? a::ProfileCoverage::assume_continuous
                                : a::ProfileCoverage::observed_only;
    ImGui::TextWrapped(
        "The SDK has no coverage / final-flush watermark. Default empty "
        "windows are not_collected gaps. This opt-in asserts collection and "
        "permits no_matches zeros.");
    bool subset = editor.partial == a::ProfilePartial::observed_subset;
    if (ImGui::Checkbox("Allow provisional / incomplete subset", &subset))
      editor.partial =
          subset ? a::ProfilePartial::observed_subset : a::ProfilePartial::gap;
    ImGui::TextWrapped(
        "Default incomplete points are gaps. An allowed subset remains labeled "
        "incomplete; later flushes may revise earlier frames.");
    if (ImGui::TreeNode("Manual IDs / text filter")) {
      bool zone = f.zone.has_value();
      if (ImGui::Checkbox("Filter zone ID", &zone)) {
        if (zone)
          f.zone = profile::ZoneKey{};
        else
          f.zone.reset();
      }
      auto z = f.zone.value_or(profile::ZoneKey{});
      ImGui::BeginDisabled(!zone);
      ImGui::SetNextItemWidth(130);
      if (ImGui::InputScalar("Zone ID (hex)", ImGuiDataType_U64, &z.id, nullptr,
                             nullptr, "%llX",
                             ImGuiInputTextFlags_CharsHexadecimal))
        f.zone = z;
      ImGui::SetNextItemWidth(130);
      if (ImGui::InputScalar("Generation", ImGuiDataType_U64, &z.generation))
        f.zone = z;
      ImGui::EndDisabled();
      bool event = f.event_id.has_value();
      if (ImGui::Checkbox("Filter event ID", &event)) {
        if (event)
          f.event_id = std::uint16_t{0};
        else
          f.event_id.reset();
      }
      auto id = f.event_id.value_or(0);
      ImGui::BeginDisabled(!event);
      ImGui::SetNextItemWidth(130);
      if (ImGui::InputScalar("Event ID", ImGuiDataType_U16, &id))
        f.event_id = id;
      ImGui::EndDisabled();
      bool thread = f.thread_id.has_value();
      if (ImGui::Checkbox("Filter thread ID", &thread)) {
        if (thread)
          f.thread_id = 0;
        else
          f.thread_id.reset();
      }
      auto tid = f.thread_id.value_or(0);
      ImGui::BeginDisabled(!thread);
      ImGui::SetNextItemWidth(130);
      if (ImGui::InputScalar("Thread ID", ImGuiDataType_U32, &tid))
        f.thread_id = tid;
      ImGui::EndDisabled();
      bool context = f.context_id.has_value();
      if (ImGui::Checkbox("Filter context ID", &context)) {
        if (context)
          f.context_id = 0;
        else
          f.context_id.reset();
      }
      auto cid = f.context_id.value_or(0);
      ImGui::BeginDisabled(!context);
      ImGui::SetNextItemWidth(130);
      if (ImGui::InputScalar("Context ID (hex)", ImGuiDataType_U64, &cid,
                             nullptr, nullptr, "%llX",
                             ImGuiInputTextFlags_CharsHexadecimal))
        f.context_id = cid;
      ImGui::EndDisabled();
      ImGui::SetNextItemWidth(
          std::max(100.f, ImGui::GetContentRegionAvail().x - 85));
      if (ImGui::InputText("Name text", filter_text.data(), filter_text.size()))
        f.text = filter_text.data();
      ImGui::TextWrapped(
          "Name text is a case-sensitive zone/event substring. Selecting an "
          "advertised event also chooses its exact zone lifetime.");
      ImGui::TreePop();
    }
    ImGui::BeginDisabled(entries.size() >= curve_limit);
    if (ImGui::Button("Add profile curve")) {
      owner.add_series(doc, editor);
      editing = false;
    }
    ImGui::EndDisabled();
    if (editing && !entries.empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Update selected"))
        owner.set_request(doc, selected, editor);
    }
    if (choices_truncated)
      ImGui::TextWrapped(
          "Selector lists reached their display budget. Manual IDs remain "
          "available; series filters still use exact IDs.");
    ImGui::TextDisabled(
        "Up to 16 curves; at most 2000 original rows per curve.");
  }
  void selected_details() {
    if (entries.empty())
      return;
    const auto &series = entries[selected].data;
    std::map<a::ProfileSampleState, std::size_t> states;
    for (const auto &s : series.samples)
      ++states[s.state];
    std::string line = "Samples:";
    for (const auto &item : states)
      line += " " + std::to_string(item.second) + " " +
              a::profile_state_name(item.first) + ";";
    ImGui::TextWrapped("%s", line.c_str());
    if (series.provisional)
      ImGui::TextWrapped(
          "PROVISIONAL: late flushes can change historical buckets.");
    if (!series.complete)
      ImGui::TextWrapped("Partial query%s%s",
                         series.cancelled ? " (cancelled)" : "",
                         series.work_limited ? " (work budget reached)" : "");
    if (series.resampled)
      ImGui::TextWrapped("Uniform original-row selection: %zu / %zu windows; "
                         "skipped rows break the line.",
                         series.samples.size(), series.candidate_samples);
    if (selected_sample && selected_sample->first < entries.size()) {
      const auto &data = entries[selected_sample->first].data;
      if (selected_sample->second < data.samples.size()) {
        const auto &s = data.samples[selected_sample->second];
        ImGui::Text("Selected native frame %zu | prefix %zu",
                    s.source.frame_index + 1, s.source.event_count);
        ImGui::TextWrapped("Exact value: %s", s.exact_value.empty()
                                                  ? "(gap)"
                                                  : s.exact_value.c_str());
        ImGui::TextWrapped("Observed inclusive total ticks: %s",
                           s.total_ticks.c_str());
      }
    }
    if (!series.diagnostics.empty() &&
        ImGui::TreeNode("Selected curve diagnostics")) {
      for (std::size_t i = 0;
           i < std::min<std::size_t>(16, series.diagnostics.size()); ++i)
        ImGui::TextWrapped("%s", series.diagnostics[i].c_str());
      ImGui::TreePop();
    }
  }
};

ProfileCurvePanel::ProfileCurvePanel() : impl_(std::make_unique<Impl>()) {}
ProfileCurvePanel::~ProfileCurvePanel() = default;
void ProfileCurvePanel::reset() { impl_ = std::make_unique<Impl>(); }
std::size_t ProfileCurvePanel::add_series(Document &doc,
                                          a::ProfileCurveRequest request,
                                          std::string label) {
  impl_->bind(doc);
  if (!doc.loaded())
    throw std::logic_error("Open a capture before adding a profile curve");
  if (impl_->entries.size() >= curve_limit)
    throw std::length_error("Profile curve limit is 16");
  impl_->update_index(doc);
  source_for(request, doc);
  a::ProfileCurveOptions options;
  options.max_samples = sample_limit;
  auto data = impl_->index->sample(request, options);
  Impl::Entry entry;
  entry.serial = impl_->serial++;
  entry.auto_label = label.empty();
  entry.label = label.empty()
                    ? label_for(request, &impl_->index->profile_index())
                    : std::move(label);
  entry.request = std::move(request);
  entry.data = std::move(data);
  const ImVec4 colors[] = {{.3f, .8f, 1, 1},   {1, .65f, .3f, 1},
                           {.6f, .9f, .4f, 1}, {.9f, .4f, .8f, 1},
                           {.8f, .8f, .3f, 1}, {.6f, .6f, 1, 1}};
  const auto c = colors[entry.serial % 6];
  entry.color = {c.x, c.y, c.z, c.w};
  impl_->entries.push_back(std::move(entry));
  impl_->selected = impl_->entries.size() - 1;
  impl_->selected_sample.reset();
  impl_->auto_y = true;
  impl_->manual_x = false;
  impl_->count_rows();
  impl_->last_refresh = std::chrono::steady_clock::now();
  impl_->error.clear();
  return impl_->selected;
}
void ProfileCurvePanel::set_request(Document &doc, std::size_t i,
                                    a::ProfileCurveRequest request) {
  impl_->bind(doc);
  if (i >= impl_->entries.size())
    throw std::out_of_range("Profile curve index");
  impl_->update_index(doc);
  source_for(request, doc);
  a::ProfileCurveOptions options;
  options.max_samples = sample_limit;
  auto data = impl_->index->sample(request, options);
  auto &entry = impl_->entries[i];
  entry.request = std::move(request);
  entry.data = std::move(data);
  if (entry.auto_label)
    entry.label = label_for(entry.request, &impl_->index->profile_index());
  impl_->selected_sample.reset();
  impl_->auto_y = true;
  impl_->count_rows();
  impl_->last_refresh = std::chrono::steady_clock::now();
  impl_->error.clear();
}
bool ProfileCurvePanel::erase_series(std::size_t i) {
  if (i >= impl_->entries.size())
    return false;
  impl_->entries.erase(impl_->entries.begin() + static_cast<std::ptrdiff_t>(i));
  impl_->selected = impl_->entries.empty()
                        ? 0
                        : std::min(impl_->selected, impl_->entries.size() - 1);
  impl_->selected_sample.reset();
  impl_->auto_y = true;
  impl_->count_rows();
  return true;
}
void ProfileCurvePanel::select_series(std::size_t i) {
  impl_->choose_series(i);
}
void ProfileCurvePanel::set_series_visible(std::size_t i, bool visible) {
  if (i >= impl_->entries.size())
    throw std::out_of_range("Profile curve index");
  impl_->entries[i].visible = visible;
}
void ProfileCurvePanel::set_auto_refresh(bool enabled) noexcept {
  impl_->auto_refresh = enabled;
}
void ProfileCurvePanel::set_time_axis(bool enabled) noexcept {
  impl_->time_axis = enabled;
  impl_->manual_x = false;
}
void ProfileCurvePanel::refresh(Document &doc) {
  impl_->bind(doc);
  if (!doc.loaded())
    throw std::logic_error("Open a capture before refreshing profile curves");
  impl_->resample(doc);
}
std::size_t ProfileCurvePanel::series_count() const noexcept {
  return impl_->entries.size();
}
const a::ProfileCurveSeries *
ProfileCurvePanel::series(std::size_t i) const noexcept {
  return i < impl_->entries.size() ? &impl_->entries[i].data : nullptr;
}
const a::ProfileCurveRequest *
ProfileCurvePanel::request(std::size_t i) const noexcept {
  return i < impl_->entries.size() ? &impl_->entries[i].request : nullptr;
}
const profile::Index *ProfileCurvePanel::profile_index() const noexcept {
  return impl_->index ? &impl_->index->profile_index() : nullptr;
}
std::size_t ProfileCurvePanel::indexed_event_count() const noexcept {
  return impl_->index ? impl_->index->event_count() : 0;
}
std::size_t ProfileCurvePanel::row_count() const noexcept {
  return impl_->rows;
}
std::size_t ProfileCurvePanel::visible_row_count() const noexcept {
  return impl_->curve_visible;
}
ProfileCurvePlotView ProfileCurvePanel::plot_view() const {
  return impl_->plot;
}
const std::string &ProfileCurvePanel::error() const noexcept {
  return impl_->error;
}
bool ProfileCurvePanel::seek_sample(Document &doc, std::size_t entry,
                                    std::size_t sample) {
  if (doc.session() != impl_->source || entry >= impl_->entries.size() ||
      sample >= impl_->entries[entry].data.samples.size())
    return false;
  const auto point = impl_->entries[entry].data.samples[sample].source;
  if (!doc.set_scene(point.scene_id) ||
      !doc.set_sample_point(SamplePoint::native_frame) ||
      !doc.set_frame(point.frame_index))
    return false;
  impl_->choose_series(entry);
  impl_->selected_sample = {{entry, sample}};
  return true;
}
void ProfileCurvePanel::draw(Document &doc, const CsvExport &export_csv) {
  try {
    impl_->bind(doc);
    impl_->curve_visible = 0;
    impl_->plot = {};
    if (!doc.loaded()) {
      ImGui::TextWrapped(
          "Open a capture or connect live to view recorded CPU scope curves.");
      return;
    }
    const bool sealed = doc.session()->finished() || doc.session()->failed();
    bool stale =
        impl_->index &&
        (impl_->index->event_count() != doc.session()->event_count() ||
         impl_->index->profile_index().statistics().finished != sealed);
    for (const auto &e : impl_->entries)
      stale |= e.data.source_event_count != doc.session()->event_count() ||
               e.data.provisional != !sealed;
    if (impl_->auto_refresh && stale &&
        doc.session()->at_transaction_boundary() &&
        std::chrono::steady_clock::now() - impl_->last_refresh >=
            std::chrono::seconds(1))
      impl_->resample(doc);
    ImGui::TextWrapped(
        "Native scene frame windows: completed scopes are assigned by original "
        "end tick, never by the packet receipt frame. Inclusive totals include "
        "nested / overlapping threads; this is not CPU utilization.");
    if (doc.sample_point() == SamplePoint::after_fetch)
      ImGui::TextWrapped(
          "The viewer currently shows after-fetch data. These curves use "
          "native windows; clicking a point switches to its native frame.");
    if (impl_->context_dirty) {
      ImGui::TextWrapped("Waiting for a complete property transaction before "
                         "changing curve source.");
      return;
    }
    if (ImGui::Button("Refresh"))
      refresh(doc);
    ImGui::SameLine();
    ImGui::Checkbox("Auto (1 Hz)", &impl_->auto_refresh);
    ImGui::SameLine();
    ImGui::BeginDisabled(impl_->entries.empty() || !export_csv);
    if (ImGui::Button("Export selected CSV") && export_csv &&
        !impl_->entries.empty()) {
      auto snapshot = impl_->entries[impl_->selected].data;
      export_csv("profile-curve.csv",
                 [snapshot = std::move(snapshot)](std::ostream &out) {
                   a::write_profile_curve_csv(out, snapshot);
                 });
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Time axis", &impl_->time_axis))
      impl_->manual_x = false;
    ImGui::SameLine();
    ImGui::Checkbox("Auto Y", &impl_->auto_y);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit view"))
      impl_->manual_x = false;
    if (!impl_->entries.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Edit selected"))
        impl_->load_editor();
    }
    if (!impl_->auto_y) {
      ImGui::SetNextItemWidth(105);
      ImGui::InputDouble("Y min", &impl_->ylo, 0, 0, "%.6g");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(105);
      ImGui::InputDouble("Y max", &impl_->yhi, 0, 0, "%.6g");
    }
    impl_->legend();
    ImGui::Text("%zu curves | %zu sampled rows | received prefix %zu",
                impl_->entries.size(), impl_->rows, indexed_event_count());
    if (stale && !impl_->auto_refresh)
      ImGui::TextWrapped("New received data or capture finalization is "
                         "available. Refresh updates historical buckets.");
    if (!impl_->entries.empty()) {
      ImGui::TextWrapped(
          "Y: %s. Select a legend row to switch unit groups; count, ticks and "
          "ns are not overlaid on one scale.",
          unit_name(impl_->entries[impl_->selected].request.metric));
      impl_->draw_plot();
      if (impl_->click)
        seek_sample(doc, impl_->click->first, impl_->click->second);
      impl_->selected_details();
    } else
      ImGui::TextWrapped(
          "Choose an advertised CPU zone/event or enter exact IDs, then add a "
          "curve. No sample data is fabricated.");
    impl_->draw_editor(doc, *this);
  } catch (const std::exception &e) {
    impl_->error = e.what();
  }
  if (!impl_->error.empty())
    ImGui::TextWrapped("Profile curves: %s", impl_->error.c_str());
}
} // namespace pvd::viewer
