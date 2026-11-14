#include "curve_panels.hpp"
#include "ui_scale.hpp"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace pvd::viewer {
namespace {
namespace a = pvd::analysis;
constexpr std::size_t series_limit = 16;
constexpr std::size_t page_size = 128;
std::string hex(std::uint64_t v) {
  std::ostringstream s;
  s << "0x" << std::hex << std::uppercase << v;
  return s.str();
}
a::FrameSource source_for(const Document &d) {
  a::FrameSource s;
  s.scene_id = d.scene_id().value_or(0);
  s.sampling = d.sample_point() == SamplePoint::after_fetch
                   ? a::Sampling::after_fetch
                   : a::Sampling::native_frame;
  return s;
}
bool same_source(const a::FrameSource &x, const a::FrameSource &y) {
  return x.scene_id == y.scene_id && x.sampling == y.sampling &&
         x.stream_id == y.stream_id &&
         x.after_fetch_section == y.after_fetch_section;
}
void text(const std::string &s) { ImGui::TextUnformatted(s.c_str()); }
void messages(const std::vector<std::string> &ds) {
  for (std::size_t i = 0; i < std::min<std::size_t>(ds.size(), 16); ++i)
    ImGui::TextWrapped("%s", ds[i].c_str());
  if (ds.size() > 16)
    ImGui::TextDisabled("%zu more diagnostics", ds.size() - 16);
}
bool parse_hex(const char *s, std::uint64_t &v) {
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    s += 2;
  const auto *end = s + std::strlen(s);
  if (end == s)
    return false;
  auto r = std::from_chars(s, end, v, 16);
  return r.ec == std::errc{} && r.ptr == end;
}
std::string value_text(const std::optional<PropertyValue> &value) {
  if (!value)
    return "(absent)";
  const auto &v = *value;
  if (v.text)
    return v.text->substr(0, 160);
  if (v.reference_ids) {
    std::ostringstream s;
    s << "refs [";
    for (std::size_t i = 0;
         i < std::min<std::size_t>(v.reference_ids->size(), 4); ++i) {
      if (i)
        s << ", ";
      s << hex((*v.reference_ids)[i]);
    }
    if (v.reference_ids->size() > 4)
      s << ", ...";
    s << "]";
    return s.str();
  }
  if (v.item_count)
    try {
      // References must stay references, even when their wire representation is
      // numeric.
      const auto &type_name = v.incoming_type.name;
      if (type_name == "VoidPtr" || type_name == "ObjectRef" ||
          type_name == "StringHandle")
        return type_name + " (" + std::to_string(v.raw_bytes().size()) +
               " bytes)";
      if (auto n = v.as_uint())
        return std::to_string(*n) + (v.item_count > 1 ? " [...]" : "");
      if (auto n = v.as_int())
        return std::to_string(*n) + (v.item_count > 1 ? " [...]" : "");
      std::ostringstream s;
      s << std::setprecision(9);
      if (auto n = v.as_double()) {
        s << *n;
        return s.str();
      }
      if (auto n = v.as_float()) {
        s << *n;
        return s.str();
      }
      if (auto n = v.as_vec3()) {
        s << '(' << n->x << ", " << n->y << ", " << n->z << ')';
        return s.str();
      }
      if (auto n = v.as_transform()) {
        s << "position (" << n->position.x << ", " << n->position.y << ", "
          << n->position.z << ")";
        return s.str();
      }
    } catch (const std::exception &) {
      return "(malformed value)";
    }
  return v.incoming_type.str() + " [" + std::to_string(v.item_count) +
         " items, " + std::to_string(v.raw_bytes().size()) + " bytes]";
}
const char *kind_name(a::ChangeKind k) {
  switch (k) {
  case a::ChangeKind::created:
    return "Created";
  case a::ChangeKind::destroyed:
    return "Destroyed";
  default:
    return "Changed";
  }
}
std::vector<std::string> components(const std::string &n) {
  if (n == "PxTransform")
    return {"position.x", "position.y", "position.z", "rotation.x",
            "rotation.y", "rotation.z", "rotation.w"};
  if (n == "PxVec3")
    return {"x", "y", "z"};
  if (n == "PxQuat" || n == "PxVec4")
    return {"x", "y", "z", "w"};
  if (n == "PxVec2")
    return {"x", "y"};
  if (n == "PxBounds3")
    return {"minimum.x", "minimum.y", "minimum.z",
            "maximum.x", "maximum.y", "maximum.z"};
  if (n == "PxMat33" || n == "PxMat44") {
    std::vector<std::string> out;
    int d = n == "PxMat33" ? 3 : 4;
    for (int r = 0; r < d; ++r)
      for (int c = 0; c < d; ++c)
        out.push_back("m[" + std::to_string(r) + "][" + std::to_string(c) +
                      "]");
    return out;
  }
  return {"value"};
}
std::optional<double> sample_x(const a::CurveSample &p, bool time) {
  if (time)
    return p.elapsed_seconds;
  return static_cast<double>(p.source.frame_index) + 1.0;
}
void pad_range(double &lo, double &hi) {
  if (lo == hi) {
    const double p = std::max(1.0, std::abs(lo) * .05);
    lo -= p;
    hi += p;
  } else {
    const double p = (hi - lo) * .08;
    if (std::isfinite(p)) {
      lo -= p;
      hi += p;
    }
  }
}
} // namespace

struct CurvePanels::Impl {
  struct Entry {
    std::size_t serial = 0;
    std::string label;
    a::CurveRequest request;
    a::Series data;
    bool visible = true;
    std::array<float, 4> color{.4f, .8f, 1, 1};
  };
  const Session *source = nullptr;
  a::FrameSource context;
  std::vector<Entry> entries;
  std::size_t serial = 0, selected = 0, max_samples = 2000, observed_events = 0;
  bool auto_refresh = false, time_axis = false, manual_x = false, auto_y = true,
       add_open = true;
  double xlo = 0, xhi = 1, ylo = -1, yhi = 1;
  CurvePlotView plot;
  std::chrono::steady_clock::time_point last_refresh{};
  std::string error;
  std::array<char, 32> object_input{};
  std::uint64_t generation = 0, item = 0;
  std::array<char, 192> property_input{}, component_input{};
  std::size_t diff_a = 0, diff_b = 0, compared_a = 0, compared_b = 0,
              diff_page = 0;
  bool diff_dirty = true;
  std::optional<a::StateDiff> diff;
  std::size_t curve_rows = 0, curve_visible = 0, diff_rows = 0,
              diff_visible = 0;
  std::optional<std::pair<std::size_t, std::size_t>> click;
  std::optional<std::pair<std::size_t, std::size_t>> selected_sample;

  void bind(Document &d) {
    if (source != d.session()) {
      source = d.session();
      entries.clear();
      curve_rows = curve_visible = diff_rows = diff_visible = 0;
      selected_sample.reset();
      object_input[0] = property_input[0] = component_input[0] = 0;
      diff.reset();
      selected = 0;
      serial = 0;
      observed_events = 0;
      manual_x = false;
      diff_a = 0;
      diff_b = d.frame_index();
      diff_page = 0;
      diff_dirty = true;
      error.clear();
      plot = {};
    }
    const auto s = source_for(d);
    if (!same_source(context, s)) {
      context = s;
      manual_x = false;
      diff_dirty = true;
      diff.reset();
      diff_rows = 0;
      diff_visible = 0;
      for (auto &e : entries)
        e.request.source = s;
      resample(d);
    }
  }
  void resample(Document &d) {
    if (!source)
      return;
    a::CurveOptions opt;
    opt.max_samples = max_samples;
    opt.resampling = a::Resampling::uniform;
    // Replace each completed result atomically. Previous valid series survives
    // a failed query.
    for (auto &e : entries) {
      auto result = a::sample_curve(*source, e.request, opt);
      e.data = std::move(result);
    }
    selected_sample.reset();
    observed_events = source->event_count();
    last_refresh = std::chrono::steady_clock::now();
    curve_rows = 0;
    for (const auto &e : entries)
      curve_rows += e.data.samples.size();
    (void)d;
  }
  std::vector<std::pair<std::string, std::string>>
  properties(const Document &d, std::uint64_t id) const {
    std::vector<std::pair<std::string, std::string>> result;
    if (!source || !d.metadata())
      return result;
    std::optional<QualifiedName> cls;
    if (auto *obj = d.frame()->snapshot.instance(id);
        obj && obj->generation == generation)
      cls = obj->class_name;
    if (!cls)
      for (const auto &life : source->lifetimes())
        if (life.id == id && life.generation == generation) {
          cls = life.class_name;
          break;
        }
    std::size_t depth = 0;
    while (cls && depth++ < 256) {
      auto *c = d.metadata()->find_class(*cls);
      if (!c)
        break;
      for (const auto &p : c->properties)
        result.push_back({p.first, p.second.datatype.name});
      cls = c->parent;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end(),
                             [](const auto &a, const auto &b) {
                               return a.first == b.first;
                             }),
                 result.end());
    return result;
  }
  void fill_inspected(const Document &d) {
    if (auto key = d.inspected_object()) {
      std::snprintf(object_input.data(), object_input.size(), "%llX",
                    static_cast<unsigned long long>(key->id));
      generation = key->generation;
    }
  }
  void add_controls(Document &d, CurvePanels &owner) {
    if (!ImGui::CollapsingHeader("Add numeric property",
                                 add_open ? ImGuiTreeNodeFlags_DefaultOpen : 0))
      return;
    ImGui::BeginDisabled(!d.inspected_object());
    if (ImGui::Button("Use inspected object"))
      fill_inspected(d);
    ImGui::EndDisabled();
    if (!d.inspected_object() && object_input[0] == 0)
      ImGui::TextWrapped("Select an object in the Inspector, or enter its "
                         "hexadecimal ID and generation.");
    ImGui::SetNextItemWidth(
        std::max(ui_pixels(100.f), ImGui::GetContentRegionAvail().x * .5f));
    ImGui::InputText("Object ID (hex)", object_input.data(),
                     object_input.size(), ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SetNextItemWidth(ui_pixels(120));
    ImGui::InputScalar("Generation", ImGuiDataType_U64, &generation);
    std::uint64_t id = 0;
    const bool valid = parse_hex(object_input.data(), id);
    const auto props = valid
                           ? properties(d, id)
                           : std::vector<std::pair<std::string, std::string>>{};
    ImGui::SetNextItemWidth(
        std::max(ui_pixels(100), ImGui::GetContentRegionAvail().x -
                                    ImGui::CalcTextSize("Property").x -
                                    ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::BeginCombo("Property", property_input[0]
                                          ? property_input.data()
                                          : "Choose recorded property")) {
      for (const auto &p : props)
        if (ImGui::Selectable(p.first.c_str(),
                              p.first == property_input.data())) {
          std::snprintf(property_input.data(), property_input.size(), "%s",
                        p.first.c_str());
          auto cs = components(p.second);
          std::snprintf(component_input.data(), component_input.size(), "%s",
                        cs.front().c_str());
        }
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(
        std::max(ui_pixels(100), ImGui::GetContentRegionAvail().x -
                                    ImGui::CalcTextSize("Property name").x -
                                    ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::InputText("Property name", property_input.data(),
                     property_input.size());
    std::vector<std::string> cs{"value"};
    for (const auto &p : props)
      if (p.first == property_input.data()) {
        cs = components(p.second);
        break;
      }
    ImGui::SetNextItemWidth(
        std::max(ui_pixels(100), ImGui::GetContentRegionAvail().x -
                                    ImGui::CalcTextSize("Component").x -
                                    ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::BeginCombo("Component", component_input[0]
                                           ? component_input.data()
                                           : "value")) {
      for (const auto &c : cs)
        if (ImGui::Selectable(c.c_str(), c == component_input.data()))
          std::snprintf(component_input.data(), component_input.size(), "%s",
                        c.c_str());
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(
        std::max(ui_pixels(100), ImGui::GetContentRegionAvail().x -
                                    ImGui::CalcTextSize("Component path").x -
                                    ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::InputText("Component path", component_input.data(),
                     component_input.size());
    ImGui::SetNextItemWidth(ui_pixels(120));
    ImGui::InputScalar("Array item", ImGuiDataType_U64, &item);
    ImGui::BeginDisabled(!valid || property_input[0] == 0 ||
                         entries.size() >= series_limit);
    if (ImGui::Button("Add curve")) {
      a::CurveRequest q;
      q.object = {id, generation};
      q.property = property_input.data();
      q.component = component_input.data();
      q.item_index = static_cast<std::size_t>(item);
      owner.add_series(d, std::move(q));
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Up to %zu curves, %zu samples each; gaps remain gaps.",
                        series_limit, max_samples);
  }

  void legend() {
    if (entries.empty())
      return;
    if (ImGui::BeginTable("Curve list", 4,
                          ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Show", 0, ui_pixels(35));
      ImGui::TableSetupColumn("Color", 0, ui_pixels(30));
      ImGui::TableSetupColumn("Series", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Remove", 0, ui_pixels(22));
      std::optional<std::size_t> remove;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        auto &e = entries[i];
        ImGui::PushID(static_cast<int>(e.serial));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Checkbox("##visible", &e.visible);
        ImGui::TableNextColumn();
        ImGui::ColorEdit4("##color", e.color.data(),
                          ImGuiColorEditFlags_NoInputs |
                              ImGuiColorEditFlags_NoLabel |
                              ImGuiColorEditFlags_AlphaBar);
        ImGui::TableNextColumn();
        if (ImGui::Selectable(e.label.c_str(), selected == i))
          selected = i;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s\n%zu samples / %zu source rows%s",
                            e.label.c_str(), e.data.samples.size(),
                            e.data.candidate_samples,
                            e.data.resampled ? " (uniformly reduced)" : "");
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
      }
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
      if (e.visible)
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
          time_axis ? "No timed numeric samples. Clock calibration or numeric "
                      "values are unavailable."
                    : "No numeric samples in the selected scene, mode and "
                      "lifetime. Inspect curve diagnostics below.");
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
        if (e.visible)
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
    const float width = std::max(ui_pixels(160.f), ImGui::GetContentRegionAvail().x),
                height = ui_pixels(260);
    const ImVec2 lo{start.x + ui_pixels(55), start.y + ui_pixels(12)},
        hi{start.x + width - ui_pixels(12), start.y + height - ui_pixels(30)};
    plot = {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y, xlo,
            xhi,  ylo,  yhi,         time_axis,   true};
    ImGui::InvisibleButton("Curve canvas", {width, height},
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonMiddle);
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
      if (dx * dx + dy * dy > ui_pixels(4) * ui_pixels(4))
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
    const bool choose = released && hovered && !plot_dragged;
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
      dl->AddText({start.x, lo.y + (hi.y - lo.y) * t - ui_pixels(6)},
                  IM_COL32(180, 185, 195, 255), label);
      std::snprintf(label, sizeof label, "%.4g", xlo + (xhi - xlo) * t);
      dl->AddText({lo.x + (hi.x - lo.x) * t - ui_pixels(10), hi.y + ui_pixels(4)},
                  IM_COL32(180, 185, 195, 255), label);
    }
    dl->AddRect(lo, hi, IM_COL32(100, 111, 125, 255));
    dl->PushClipRect(lo, hi, true);
    float nearest = ui_pixels(10) * ui_pixels(10);
    std::optional<std::pair<std::size_t, std::size_t>> nearest_sample;
    for (std::size_t ei = 0; ei < entries.size(); ++ei) {
      const auto &e = entries[ei];
      if (!e.visible)
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
          dl->AddLine(*previous, p, color, ui_pixels(1.8f));
        previous = p;
        if (*x >= xlo && *x <= xhi && y >= ylo && y <= yhi) {
          ++curve_visible;
          if (e.data.samples.size() <= 200)
            dl->AddCircleFilled(p, ui_pixels(3), color);
          const float dx = p.x - io.MousePos.x, dy = p.y - io.MousePos.y,
                      dist = dx * dx + dy * dy;
          if (dist < nearest) {
            nearest = dist;
            nearest_sample = {{ei, si}};
          }
          if (selected_sample == std::make_optional(std::make_pair(ei, si)))
            dl->AddCircle(p, ui_pixels(6), IM_COL32_WHITE, 0, ui_pixels(1.5f));
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
      text(a::numeric_text(*s.value));
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
        time_axis ? "Elapsed seconds" : "Scene timeline frame");
    ImGui::PopStyleColor();
  }
  void curve_controls(Document &d, CurvePanels &owner, const CsvExport &csv) {
    if (ImGui::Button("Refresh"))
      owner.refresh(d);
    ImGui::SameLine();
    ImGui::Checkbox("Auto (1 Hz)", &auto_refresh);
    ImGui::SameLine();
    ImGui::BeginDisabled(entries.empty() || !csv);
    if (ImGui::Button("Export selected CSV") && csv && !entries.empty()) {
      const auto &data = entries[std::min(selected, entries.size() - 1)].data;
      csv("curve.csv",
          [&data](std::ostream &s) { a::write_curve_csv(s, data); });
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Time axis", &time_axis))
      manual_x = false;
    ImGui::SameLine();
    ImGui::Checkbox("Auto Y", &auto_y);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit view"))
      manual_x = false;
    if (!auto_y) {
      ImGui::SetNextItemWidth(ui_pixels(110));
      ImGui::InputDouble("Y min", &ylo, 0, 0, "%.6g");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ui_pixels(110));
      ImGui::InputDouble("Y max", &yhi, 0, 0, "%.6g");
    }
    if (ImGui::CollapsingHeader("Sampling budget")) {
      int n = static_cast<int>(max_samples);
      ImGui::SetNextItemWidth(ui_pixels(130));
      if (ImGui::InputInt("Max samples / curve", &n)) {
        max_samples = static_cast<std::size_t>(std::clamp(n, 1, 20000));
      }
      ImGui::TextWrapped(
          "Default 2000. Refresh applies this budget using original uniformly "
          "selected rows, never interpolation. Skipped rows break the line.");
    }
    if (!entries.empty())
      legend();
    curve_rows = 0;
    for (const auto &e : entries)
      curve_rows += e.data.samples.size();
    ImGui::Text("%zu curves | %zu sampled rows", entries.size(), curve_rows);
    if (entries.empty())
      ImGui::TextWrapped(
          "Add a recorded numeric property to plot its actual samples. Missing "
          "lifetimes and uncaptured values appear as gaps.");
    else {
      draw_plot();
      if (click)
        owner.seek_sample(d, click->first, click->second);
      if (selected < entries.size()) {
        const auto &data = entries[selected].data;
        std::map<a::Gap, std::size_t> gaps;
        for (const auto &s : data.samples)
          if (!s.value)
            ++gaps[s.gap];
        if (!gaps.empty()) {
          std::string line = "Gaps:";
          for (const auto &g : gaps)
            line += " " + std::to_string(g.second) + " " +
                    a::gap_name(g.first) + ";";
          ImGui::TextWrapped("%s", line.c_str());
        }
        if (data.resampled)
          ImGui::TextWrapped("Uniform reduction: %zu of %zu source rows. "
                             "Skipped rows break the line.",
                             data.samples.size(), data.candidate_samples);
        if (!data.complete)
          ImGui::TextWrapped("Incomplete sampling result%s.",
                             data.cancelled ? " (cancelled)" : "");
        if (source && data.source_event_count < source->event_count())
          ImGui::TextDisabled(
              "New source data available; Refresh to resample.");
      }
      for (const auto &e : entries)
        if (!e.data.diagnostics.empty()) {
          ImGui::PushID(static_cast<int>(e.serial));
          if (ImGui::TreeNode((e.label + " diagnostics").c_str())) {
            messages(e.data.diagnostics);
            ImGui::TreePop();
          }
          ImGui::PopID();
        }
      if (selected_sample && selected_sample->first < entries.size()) {
        const auto &e = entries[selected_sample->first];
        if (selected_sample->second < e.data.samples.size()) {
          const auto &s = e.data.samples[selected_sample->second];
          ImGui::Text("Selected frame %zu | event prefix %zu",
                      s.source.frame_index + 1, s.source.event_count);
          if (s.value)
            text("Exact value: " + a::numeric_text(*s.value));
        }
      }
    }
    add_controls(d, owner);
  }

  std::size_t diff_selected = 0, property_page = 0;
  void diff_controls(Document &d, CurvePanels &owner) {
    const auto n = d.timeline().size();
    ImGui::TextWrapped("Compare snapshots at exact per-scene %s markers. "
                       "Object ID reuse is a destroy/create pair.",
                       d.sample_point() == SamplePoint::after_fetch
                           ? "after-fetch"
                           : "native EndSection");
    if (!n) {
      ImGui::TextWrapped("No completed markers in this scene and sample mode.");
      return;
    }
    std::uint64_t ai = static_cast<std::uint64_t>(std::min(diff_a, n - 1)) + 1,
                  bi = static_cast<std::uint64_t>(std::min(diff_b, n - 1)) + 1;
    ImGui::SetNextItemWidth(ui_pixels(110));
    if (ImGui::InputScalar("A frame", ImGuiDataType_U64, &ai)) {
      diff_a =
          static_cast<std::size_t>(std::clamp<std::uint64_t>(ai, 1, n) - 1);
      diff_dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui_pixels(110));
    if (ImGui::InputScalar("B frame", ImGuiDataType_U64, &bi)) {
      diff_b =
          static_cast<std::size_t>(std::clamp<std::uint64_t>(bi, 1, n) - 1);
      diff_dirty = true;
    }
    if (ImGui::Button("Previous / current")) {
      diff_b = d.frame_index();
      diff_a = diff_b ? diff_b - 1 : 0;
      diff_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Compare"))
      owner.refresh_diff(d);
    ImGui::SameLine();
    if (ImGui::SmallButton("Go A"))
      d.set_frame(std::min(diff_a, n - 1));
    ImGui::SameLine();
    if (ImGui::SmallButton("Go B"))
      d.set_frame(std::min(diff_b, n - 1));
    if (diff_dirty)
      ImGui::TextDisabled(
          "Frame choices changed. Compare to update the result.");
    if (!diff) {
      ImGui::TextWrapped("Choose two frame indices and press Compare.");
      return;
    }
    ImGui::Text("Result A %zu -> B %zu | prefixes %zu -> %zu", compared_a + 1,
                compared_b + 1, diff->before_event_count,
                diff->after_event_count);
    if (diff->truncated || diff->cancelled || !diff->complete)
      ImGui::TextColored({1, .7f, .3f, 1}, "Partial result: %s%s",
                         diff->truncated ? "budget reached " : "",
                         diff->cancelled ? "cancelled" : "");
    messages(diff->diagnostics);
    diff_rows = diff->objects.size();
    diff_visible = 0;
    std::size_t created = 0, destroyed = 0, changed = 0;
    for (const auto &c : diff->objects) {
      if (c.kind == a::ChangeKind::created)
        ++created;
      else if (c.kind == a::ChangeKind::destroyed)
        ++destroyed;
      else
        ++changed;
    }
    ImGui::Text("%zu Created  %zu Destroyed  %zu Changed", created, destroyed,
                changed);
    const auto last = diff_rows ? (diff_rows - 1) / page_size : 0;
    diff_page = std::min(diff_page, last);
    ImGui::BeginDisabled(!diff_page);
    if (ImGui::SmallButton("Previous page"))
      --diff_page;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(diff_page == last);
    if (ImGui::SmallButton("Next page"))
      ++diff_page;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Page %zu / %zu", diff_page + 1, last + 1);
    if (ImGui::BeginTable("Object changes", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit,
                          {0, ui_pixels(180)})) {
      ImGui::TableSetupColumn("Change", 0, ui_pixels(68));
      ImGui::TableSetupColumn("Object / generation",
                              ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Properties", 0, ui_pixels(70));
      ImGui::TableSetupColumn("Inspect", 0, ui_pixels(48));
      ImGui::TableHeadersRow();
      const auto first = diff_page * page_size,
                 count = std::min(page_size, diff_rows - first);
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(count));
      while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
          const auto i = first + static_cast<std::size_t>(row);
          const auto &c = diff->objects[i];
          ++diff_visible;
          ImGui::PushID(static_cast<int>(i));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          text(kind_name(c.kind));
          ImGui::TableNextColumn();
          const auto label =
              hex(c.object.id) + " / " + std::to_string(c.object.generation);
          if (ImGui::Selectable(label.c_str(), diff_selected == i)) {
            diff_selected = i;
            property_page = 0;
          }
          ImGui::TableNextColumn();
          ImGui::Text("%zu", c.properties.size());
          ImGui::TableNextColumn();
          if (ImGui::SmallButton("View")) {
            d.set_frame(c.kind == a::ChangeKind::destroyed ? compared_a
                                                           : compared_b);
            d.inspect(c.object.id);
            diff_selected = i;
            property_page = 0;
          }
          ImGui::PopID();
        }
      ImGui::EndTable();
    }
    if (!diff->objects.empty()) {
      diff_selected = std::min(diff_selected, diff->objects.size() - 1);
      const auto &c = diff->objects[diff_selected];
      text(std::string(kind_name(c.kind)) + " " + hex(c.object.id) +
           " / generation " + std::to_string(c.object.generation));
      if (c.before)
        text("Before: " + c.before->class_name.str() + " | pickable " +
             (c.before->pickable ? "yes" : "no") + " | top-level " +
             (c.before->top_level ? "yes" : "no"));
      if (c.after)
        text("After: " + c.after->class_name.str() + " | pickable " +
             (c.after->pickable ? "yes" : "no") + " | top-level " +
             (c.after->top_level ? "yes" : "no"));
      if (c.color_changed)
        ImGui::TextUnformatted("Color changed");
      if (c.class_changed)
        ImGui::TextUnformatted("Class changed");
      const auto lastp =
          c.properties.empty() ? 0 : (c.properties.size() - 1) / page_size;
      property_page = std::min(property_page, lastp);
      if (lastp) {
        ImGui::BeginDisabled(!property_page);
        if (ImGui::SmallButton("Previous properties"))
          --property_page;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(property_page == lastp);
        if (ImGui::SmallButton("Next properties"))
          ++property_page;
        ImGui::EndDisabled();
      }
      if (ImGui::BeginTable("Property changes", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingStretchProp,
                            {0, ui_pixels(170)})) {
        ImGui::TableSetupColumn("Property");
        ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn("After");
        ImGui::TableHeadersRow();
        const auto first = property_page * page_size,
                   count = std::min(page_size, c.properties.size() - first);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count));
        while (clipper.Step())
          for (int row = clipper.DisplayStart; row < clipper.DisplayEnd;
               ++row) {
            const auto &p = c.properties[first + static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(p.name);
            ImGui::TableNextColumn();
            text(value_text(p.before));
            ImGui::TableNextColumn();
            text(value_text(p.after));
          }
        ImGui::EndTable();
      }
    }
    if (!diff->cameras.empty() || !diff->origin_shifts.empty()) {
      ImGui::Text("%zu camera changes | %zu explicit origin shifts",
                  diff->cameras.size(), diff->origin_shifts.size());
      if (ImGui::TreeNode("Camera and origin changes")) {
        const auto vec = [](const Vec3 &v) {
          std::ostringstream s;
          s << std::setprecision(9) << '(' << v.x << ", " << v.y << ", " << v.z
            << ')';
          return s.str();
        };
        for (std::size_t i = 0;
             i < std::min<std::size_t>(diff->cameras.size(), page_size); ++i) {
          const auto &c = diff->cameras[i];
          text(c.name);
          if (c.before)
            text("Before position " + vec(c.before->position));
          if (c.after)
            text("After position " + vec(c.after->position));
        }
        for (std::size_t i = 0;
             i < std::min<std::size_t>(diff->origin_shifts.size(), page_size);
             ++i) {
          const auto &c = diff->origin_shifts[i];
          text("Scene " + hex(c.scene_id));
          if (c.before)
            text("Before " + vec(*c.before));
          if (c.after)
            text("After " + vec(*c.after));
        }
        ImGui::TextDisabled("First %zu auxiliary rows per kind shown",
                            page_size);
        ImGui::TreePop();
      }
    }
  }
};

CurvePanels::CurvePanels() : impl_(std::make_unique<Impl>()) {}
CurvePanels::~CurvePanels() = default;
void CurvePanels::reset() { impl_ = std::make_unique<Impl>(); }
std::size_t CurvePanels::add_series(Document &d, a::CurveRequest q,
                                    std::string label) {
  impl_->bind(d);
  if (!d.loaded() || !d.session())
    throw std::logic_error("Open a capture before adding a curve");
  if (impl_->entries.size() >= series_limit)
    throw std::length_error(
        "Curve limit is 16; remove a curve before adding another");
  if (q.property.empty())
    throw std::invalid_argument("Choose a numeric property");
  q.source = source_for(d);
  a::CurveOptions opt;
  opt.max_samples = impl_->max_samples;
  opt.resampling = a::Resampling::uniform;
  auto result = a::sample_curve(*d.session(), q, opt);
  if (label.empty())
    label = hex(q.object.id) + " / " + std::to_string(q.object.generation) +
            "  " + q.property + (q.component.empty() ? "" : "." + q.component) +
            (q.item_index ? "[" + std::to_string(q.item_index) + "]" : "");
  Impl::Entry e;
  e.serial = impl_->serial++;
  e.label = std::move(label);
  e.request = std::move(q);
  e.data = std::move(result);
  const ImVec4 palette[] = {{.3f, .8f, 1, 1},   {1, .65f, .3f, 1},
                            {.6f, .9f, .4f, 1}, {.9f, .4f, .8f, 1},
                            {.8f, .8f, .3f, 1}, {.6f, .6f, 1, 1}};
  const auto col = palette[e.serial % 6];
  e.color = {col.x, col.y, col.z, col.w};
  impl_->entries.push_back(std::move(e));
  impl_->selected = impl_->entries.size() - 1;
  impl_->observed_events = d.session()->event_count();
  impl_->last_refresh = std::chrono::steady_clock::now();
  impl_->manual_x = false;
  impl_->error.clear();
  impl_->curve_rows = 0;
  for (const auto &v : impl_->entries)
    impl_->curve_rows += v.data.samples.size();
  return impl_->selected;
}
std::size_t CurvePanels::add_inspected(Document &d, const std::string &p,
                                       const std::string &c, std::size_t item) {
  const auto key = d.inspected_object();
  if (!key)
    throw std::invalid_argument(
        "Select an object in the Inspector or enter its ID and generation");
  a::CurveRequest q;
  q.object = {key->id, key->generation};
  q.property = p;
  q.component = c;
  q.item_index = item;
  return add_series(d, std::move(q));
}
bool CurvePanels::erase_series(std::size_t i) {
  if (i >= impl_->entries.size())
    return false;
  impl_->entries.erase(impl_->entries.begin() + static_cast<std::ptrdiff_t>(i));
  impl_->selected = impl_->entries.empty()
                        ? 0
                        : std::min(impl_->selected, impl_->entries.size() - 1);
  impl_->selected_sample.reset();
  impl_->curve_rows = 0;
  for (const auto &e : impl_->entries)
    impl_->curve_rows += e.data.samples.size();
  return true;
}
void CurvePanels::set_series_visible(std::size_t i, bool v) {
  if (i >= impl_->entries.size())
    throw std::out_of_range("Curve index");
  impl_->entries[i].visible = v;
}
void CurvePanels::set_auto_refresh(bool v) noexcept { impl_->auto_refresh = v; }
void CurvePanels::refresh(Document &d) {
  impl_->bind(d);
  if (d.session() && !d.session()->at_transaction_boundary())
    throw std::logic_error("Refresh waits for a complete property transaction");
  impl_->resample(d);
  impl_->error.clear();
}
std::size_t CurvePanels::series_count() const noexcept {
  return impl_->entries.size();
}
const a::Series *CurvePanels::series(std::size_t i) const noexcept {
  return i < impl_->entries.size() ? &impl_->entries[i].data : nullptr;
}
bool CurvePanels::seek_sample(Document &d, std::size_t ei, std::size_t si) {
  if (ei >= impl_->entries.size() ||
      si >= impl_->entries[ei].data.samples.size())
    return false;
  const auto &e = impl_->entries[ei];
  const auto p = e.data.samples[si].source;
  const auto q = e.request;
  if (d.session() != impl_->source)
    return false;
  if (!d.set_scene(p.scene_id))
    return false;
  d.set_sample_point(q.source.sampling == a::Sampling::after_fetch
                         ? SamplePoint::after_fetch
                         : SamplePoint::native_frame);
  if (!d.set_frame(p.frame_index))
    return false;
  if (auto *obj = d.frame()->snapshot.instance(q.object.id);
      obj && obj->generation == q.object.generation)
    d.inspect(q.object.id);
  impl_->selected = ei;
  impl_->selected_sample = {{ei, si}};
  return true;
}
void CurvePanels::set_time_axis(bool v) noexcept {
  impl_->time_axis = v;
  impl_->manual_x = false;
}
CurvePlotView CurvePanels::plot_view() const noexcept { return impl_->plot; }
void CurvePanels::set_diff_frames(std::size_t a, std::size_t b) noexcept {
  impl_->diff_a = a;
  impl_->diff_b = b;
  impl_->diff_dirty = true;
}
void CurvePanels::refresh_diff(Document &d) {
  // Preserve scripted choices across the first binding.
  const auto a0 = impl_->diff_a, b0 = impl_->diff_b;
  const bool first = !impl_->source;
  impl_->bind(d);
  if (first) {
    impl_->diff_a = a0;
    impl_->diff_b = b0;
  }
  if (!d.session() || d.timeline().empty())
    throw std::logic_error("No completed frames to compare");
  if (impl_->diff_a >= d.timeline().size() ||
      impl_->diff_b >= d.timeline().size())
    throw std::out_of_range(
        "Diff frame index is outside the current scene timeline");
  const auto source = source_for(d);
  a::DiffOptions opts;
  opts.include_lifecycle_properties = true;
  auto result = a::diff_frames(*d.session(), {source, impl_->diff_a},
                               {source, impl_->diff_b}, opts);
  impl_->diff = std::move(result);
  impl_->compared_a = impl_->diff_a;
  impl_->compared_b = impl_->diff_b;
  impl_->diff_dirty = false;
  impl_->diff_page = 0;
  impl_->diff_selected = 0;
  impl_->property_page = 0;
  impl_->diff_rows = impl_->diff->objects.size();
  impl_->error.clear();
}
const a::StateDiff *CurvePanels::diff_result() const noexcept {
  return impl_->diff ? &*impl_->diff : nullptr;
}
std::size_t CurvePanels::row_count(CurvePage p) const noexcept {
  return p == CurvePage::curves ? impl_->curve_rows : impl_->diff_rows;
}
std::size_t CurvePanels::visible_row_count(CurvePage p) const noexcept {
  return p == CurvePage::curves ? impl_->curve_visible : impl_->diff_visible;
}
const std::string &CurvePanels::error() const noexcept { return impl_->error; }
void CurvePanels::draw(Document &d, CurvePage p, const CsvExport &csv) {
  try {
    impl_->bind(d);
    impl_->curve_visible = 0;
    impl_->diff_visible = 0;
    if (!d.loaded()) {
      ImGui::TextWrapped("Open a capture or connect live to inspect recorded "
                         "curves and frame differences.");
      return;
    }
    if (impl_->auto_refresh && p == CurvePage::curves &&
        d.session()->event_count() != impl_->observed_events &&
        d.session()->at_transaction_boundary() &&
        std::chrono::steady_clock::now() - impl_->last_refresh >=
            std::chrono::seconds(1))
      impl_->resample(d);
    ImGui::TextDisabled("Scene %s | %s", hex(d.scene_id().value_or(0)).c_str(),
                        d.sample_point() == SamplePoint::after_fetch
                            ? "Explicit after-fetch markers"
                            : "Native EndSection markers");
    if (p == CurvePage::curves)
      impl_->curve_controls(d, *this, csv);
    else
      impl_->diff_controls(d, *this);
  } catch (const std::exception &e) {
    impl_->error = e.what();
  }
  if (!impl_->error.empty())
    ImGui::TextWrapped("Analysis error: %s", impl_->error.c_str());
}
} // namespace pvd::viewer
