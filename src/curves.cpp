#include "pvd/curves.hpp"
#include <algorithm>
#include "pvd/coordinates.hpp"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <type_traits>

namespace pvd::analysis {
namespace {
void valid_source(const FrameSource &source) {
  if (source.sampling != Sampling::native_frame &&
      source.sampling != Sampling::after_fetch)
    throw std::invalid_argument("unknown curve sampling mode");
  if (source.sampling == Sampling::after_fetch &&
      source.after_fetch_section.empty())
    throw std::invalid_argument("explicit section name must not be empty");
}
template <class Visitor>
void each_frame(const Session &session, const FrameSource &source,
                Visitor visitor) {
  valid_source(source);
  std::size_t ordinal = 0;
  if (source.sampling == Sampling::native_frame) {
    for (const auto &f : session.frames())
      if (f.scene_id == source.scene_id) {
        SampleLocation loc{ordinal++, f.event_count, f.scene_id, f.stream_id,
                           f.end_timestamp};
        if ((!source.stream_id || *source.stream_id == f.stream_id) &&
            !visitor(loc))
          return;
      }
  } else {
    for (const auto &f : session.sections())
      if (f.section_id == source.scene_id &&
          f.name == source.after_fetch_section) {
        SampleLocation loc{ordinal++, f.event_count, f.section_id, f.stream_id,
                           f.end_timestamp};
        if ((!source.stream_id || *source.stream_id == f.stream_id) &&
            !visitor(loc))
          return;
      }
  }
}
std::vector<std::size_t> uniform_indices(std::size_t count,
                                         std::size_t wanted) {
  std::vector<std::size_t> result;
  result.reserve(wanted);
  if (!wanted)
    return result;
  if (wanted == 1) {
    result.push_back(0);
    return result;
  }
  const auto denominator = wanted - 1, step = (count - 1) / denominator,
             remainder = (count - 1) % denominator;
  std::size_t position = 0, error = 0;
  for (std::size_t i = 0; i < wanted; ++i) {
    result.push_back(position);
    if (i + 1 == wanted)
      break;
    position += step;
    if (remainder && error >= denominator - remainder) {
      ++position;
      error -= denominator - remainder;
    } else
      error += remainder;
  }
  return result;
}
std::uint64_t integer_at(const Bytes &bytes, std::size_t offset,
                         std::size_t width, ByteOrder order) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    auto shift = order == ByteOrder::little ? i : width - i - 1;
    value |= std::uint64_t(bytes[offset + i]) << (shift * 8);
  }
  return value;
}
struct ReadResult {
  std::optional<Numeric> value;
  Gap gap = Gap::none;
};
ReadResult read_numeric(const PropertyValue &value,
                        const PropertyDefinition *definition,
                        std::string component, std::size_t item,
                        CoordinateSpace space) {
  const auto &type = value.incoming_type;
  if (value.reference_ids ||
      (definition &&
       definition->datatype == QualifiedName{"physx3", "ObjectRef"}) ||
      (type.namespace_name == "physx3" &&
       (type.name == "VoidPtr" || type.name == "ObjectRef" ||
        type.name == "String" || type.name == "StringHandle")))
    return {{}, Gap::not_numeric};
  if (item >= value.item_count)
    return {{}, Gap::item_missing};
  if (type.namespace_name != "physx3" || value.opaque)
    return {{}, Gap::unsupported_type};
  const auto &n = type.name;
  std::size_t width = 0, offset = 0, field_width = 0;
  enum class Kind { unsigned_int, signed_int, f32, f64 };
  Kind kind = Kind::f32;
  auto scalar = [&](std::size_t w, Kind k) {
    width = field_width = w;
    kind = k;
    return component.empty() || component == "value";
  };
  bool component_valid = true;
  if (n == "PvdU8" || n == "PvdBool")
    component_valid = scalar(1, Kind::unsigned_int);
  else if (n == "PvdU16")
    component_valid = scalar(2, Kind::unsigned_int);
  else if (n == "PvdU32")
    component_valid = scalar(4, Kind::unsigned_int);
  else if (n == "PvdU64")
    component_valid = scalar(8, Kind::unsigned_int);
  else if (n == "PvdI8")
    component_valid = scalar(1, Kind::signed_int);
  else if (n == "PvdI16")
    component_valid = scalar(2, Kind::signed_int);
  else if (n == "PvdI32")
    component_valid = scalar(4, Kind::signed_int);
  else if (n == "PvdI64")
    component_valid = scalar(8, Kind::signed_int);
  else if (n == "PvdF32")
    component_valid = scalar(4, Kind::f32);
  else if (n == "PvdF64")
    component_valid = scalar(8, Kind::f64);
  else {
    field_width = 4;
    kind = Kind::f32;
    auto axis = [&](const std::string &s,
                    std::size_t dimensions) -> std::optional<std::size_t> {
      if (s.size() != 1)
        return {};
      auto p = std::string("xyzw").find(s[0]);
      return p < dimensions ? std::optional<std::size_t>(p) : std::nullopt;
    };
    if (n == "PxVec2" || n == "PxVec3" || n == "PxVec4" || n == "PxQuat") {
      width = n == "PxVec2" ? 8 : n == "PxVec3" ? 12 : 16;
      auto a = axis(component, width / 4);
      component_valid = bool(a);
      if (a)
        offset = *a * 4;
    } else if (n == "PxTransform") {
      width = 28;
      for (const auto *prefix : {"Transform.", "PxTransform."})
        if (component.compare(0, std::strlen(prefix), prefix) == 0) {
          component.erase(0, std::strlen(prefix));
          break;
        }
      std::optional<std::size_t> a;
      if (component.compare(0, 9, "position.") == 0) {
        a = axis(component.substr(9), 3);
        if (a)
          offset = 16 + *a * 4;
      } else if (component.compare(0, 9, "rotation.") == 0) {
        a = axis(component.substr(9), 4);
        if (a)
          offset = *a * 4;
      }
      component_valid = bool(a);
    } else if (n == "PxBounds3") {
      width = 24;
      std::optional<std::size_t> a;
      if (component.compare(0, 8, "minimum.") == 0) {
        a = axis(component.substr(8), 3);
        if (a)
          offset = *a * 4;
      } else if (component.compare(0, 8, "maximum.") == 0) {
        a = axis(component.substr(8), 3);
        if (a)
          offset = 12 + *a * 4;
      }
      component_valid = bool(a);
    } else if (n == "PxMat33" || n == "PxMat44") {
      const std::size_t dimension = n == "PxMat33" ? 3 : 4;
      width = dimension * dimension * 4;
      component_valid = component.size() == 7 && component[0] == 'm' &&
                        component[1] == '[' && component[3] == ']' &&
                        component[4] == '[' && component[6] == ']' &&
                        component[2] >= '0' && component[5] >= '0' &&
                        std::size_t(component[2] - '0') < dimension &&
                        std::size_t(component[5] - '0') < dimension;
      if (component_valid)
        offset = (std::size_t(component[5] - '0') * dimension +
                  std::size_t(component[2] - '0')) *
                 4;
    } else if (n == "U32Array4") {
      width = 16;
      kind = Kind::unsigned_int;
      component_valid =
          component.size() == 1 && component[0] >= '0' && component[0] <= '3';
      if (component_valid)
        offset = std::size_t(component[0] - '0') * 4;
    } else
      return {{}, Gap::unsupported_type};
  }
  if (!component_valid)
    return {{}, Gap::invalid_component};
  const auto &bytes = space == CoordinateSpace::native
                          ? coordinates::native_bytes(value) : value.raw_bytes();
  if (!width ||
      value.item_count > std::numeric_limits<std::size_t>::max() / width ||
      bytes.size() != value.item_count * width)
    return {{}, Gap::malformed_value};
  const auto bits =
      integer_at(bytes, item * width + offset, field_width, value.byte_order);
  if (n == "PvdBool" && bits > 1)
    return {{}, Gap::malformed_value};
  if (kind == Kind::unsigned_int)
    return {Numeric{bits}, Gap::none};
  if (kind == Kind::signed_int) {
    auto extended = bits;
    if (field_width < 8 && (bits & (std::uint64_t(1) << (field_width * 8 - 1))))
      extended |= (~std::uint64_t(0)) << (field_width * 8);
    std::int64_t v;
    std::memcpy(&v, &extended, 8);
    return {Numeric{v}, Gap::none};
  }
  double result;
  if (kind == Kind::f32) {
    auto u = static_cast<std::uint32_t>(bits);
    float f;
    std::memcpy(&f, &u, 4);
    result = f;
  } else
    std::memcpy(&result, &bits, 8);
  if (!std::isfinite(result))
    return {{}, Gap::non_finite};
  return {Numeric{result}, Gap::none};
}
std::string quoted(const std::string &text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"')
      out += '"';
    out += c;
  }
  return out + '"';
}
bool equal_color(const std::optional<Color> &a, const std::optional<Color> &b) {
  return a.has_value() == b.has_value() &&
         (!a || (a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a));
}
bool equal_float(float a, float b) {
  std::uint32_t x, y;
  std::memcpy(&x, &a, 4);
  std::memcpy(&y, &b, 4);
  return x == y;
}
bool equal_vec(Vec3 a, Vec3 b) {
  return equal_float(a.x, b.x) && equal_float(a.y, b.y) &&
         equal_float(a.z, b.z);
}
bool equal_camera(const CameraState &a, const CameraState &b) {
  return equal_vec(a.position, b.position) && equal_vec(a.up, b.up) &&
         equal_vec(a.target, b.target);
}
InstanceAttributes attributes(const InstanceState &o) {
  return {o.class_name, o.pickable, o.top_level, o.color};
}
std::size_t retained_size(const PropertyValue &v) {
  std::size_t size = v.raw_bytes().size();
  auto add = [&](std::size_t n) {
    size = n > std::numeric_limits<std::size_t>::max() - size
               ? std::numeric_limits<std::size_t>::max()
               : size + n;
  };
  if (v.text)
    add(v.text->size());
  if (v.reference_ids) {
    if (v.reference_ids->size() > std::numeric_limits<std::size_t>::max() / 8)
      return std::numeric_limits<std::size_t>::max();
    add(v.reference_ids->size() * 8);
  }
  if (v.native_coordinate_bytes && v.native_coordinate_bytes != v.bytes)
    add(v.native_coordinate_bytes->size());
  return size;
}
} // namespace
std::optional<SampleLocation> locate_frame(const Session &s,
                                           const FramePoint &point) {
  std::optional<SampleLocation> out;
  each_frame(s, point.source, [&](const SampleLocation &loc) {
    if (loc.frame_index == point.frame_index) {
      out = loc;
      return false;
    }
    return loc.frame_index < point.frame_index;
  });
  return out;
}
double plot_value(const Numeric &n) noexcept {
  return std::visit([](auto v) { return static_cast<double>(v); }, n);
}
std::string numeric_text(const Numeric &n) {
  return std::visit(
      [](auto v) {
        using T = decltype(v);
        if constexpr (std::is_integral_v<T>)
          return std::to_string(v);
        else {
          std::ostringstream text;
          text.imbue(std::locale::classic());
          text << std::setprecision(std::numeric_limits<double>::max_digits10)
               << v;
          return text.str();
        }
      },
      n);
}
std::optional<double> timestamp_seconds(const StreamHeader &h,
                                        std::uint64_t ticks) noexcept {
  if (!h.timestamp_numerator || !h.timestamp_denominator)
    return {};
  auto value = static_cast<long double>(ticks) * h.timestamp_numerator /
               h.timestamp_denominator * 1e-9L;
  auto result = static_cast<double>(value);
  return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}
const char *gap_name(Gap g) noexcept {
  switch (g) {
  case Gap::none:
    return "";
  case Gap::object_absent:
    return "object_absent";
  case Gap::generation_mismatch:
    return "generation_mismatch";
  case Gap::property_missing:
    return "property_missing";
  case Gap::item_missing:
    return "item_missing";
  case Gap::not_numeric:
    return "not_numeric";
  case Gap::unsupported_type:
    return "unsupported_type";
  case Gap::invalid_component:
    return "invalid_component";
  case Gap::malformed_value:
    return "malformed_value";
  case Gap::non_finite:
    return "non_finite";
  }
  return "unknown";
}
Series sample_curve(const Session &s, CurveRequest request,
                    CurveOptions options) {
  if (request.coordinate_space != CoordinateSpace::native &&
      request.coordinate_space != CoordinateSpace::wire)
    throw std::invalid_argument("unknown curve coordinate space");
  if (!options.max_samples || !request.stride ||
      request.first_frame > request.end_frame)
    throw std::invalid_argument("invalid curve sample budget/range/stride");
  if (request.property.empty())
    throw std::invalid_argument("curve property must not be empty");
  Series out;
  out.request = std::move(request);
  out.source_event_count = s.event_count();
  const auto &q = out.request;
  auto accepted = [&](const SampleLocation &loc) {
    return loc.frame_index >= q.first_frame && loc.frame_index < q.end_frame &&
           (loc.frame_index - q.first_frame) % q.stride == 0;
  };
  auto cancelled = [&] {
    if (options.cancelled && options.cancelled()) {
      out.complete = false;
      out.cancelled = true;
      return true;
    }
    return false;
  };
  if (cancelled())
    return out;
  each_frame(s, q.source, [&](const SampleLocation &loc) {
    if (cancelled())
      return false;
    if (accepted(loc))
      ++out.candidate_samples;
    return true;
  });
  if (out.cancelled)
    return out;
  if (out.candidate_samples > options.max_samples &&
      options.resampling == Resampling::reject)
    throw std::length_error("curve candidate count exceeds max_samples");
  out.resampled = out.candidate_samples > options.max_samples;
  const auto count = std::min(out.candidate_samples, options.max_samples);
  auto selected = uniform_indices(out.candidate_samples, count);
  out.samples.reserve(count);
  std::size_t ordinal = 0, next = 0;
  std::optional<std::uint64_t> origin;
  if (!s.header().timestamp_numerator || !s.header().timestamp_denominator)
    out.diagnostics.push_back(
        "counter calibration unavailable; timestamps remain raw ticks");
  each_frame(s, q.source, [&](const SampleLocation &loc) {
    if (cancelled())
      return false;
    if (!accepted(loc))
      return true;
    const auto candidate = ordinal++;
    if (next >= selected.size())
      return false;
    if (candidate != selected[next])
      return true;
    ++next;
    CurveSample sample;
    sample.source = loc;
    sample.break_before =
        !out.samples.empty() &&
        loc.frame_index - out.samples.back().source.frame_index > 1;
    sample.seconds = timestamp_seconds(s.header(), loc.timestamp_ticks);
    if (!origin)
      origin = loc.timestamp_ticks;
    if (sample.seconds) {
      // Subtract unsigned counters before floating conversion. MSVC long double
      // is double: converting adjacent uint64 counters near UINT64_MAX first
      // would erase their difference even though the raw delta is exact.
      const long double tick_delta =
          loc.timestamp_ticks >= *origin
              ? static_cast<long double>(loc.timestamp_ticks - *origin)
              : -static_cast<long double>(*origin - loc.timestamp_ticks);
      auto delta = tick_delta * s.header().timestamp_numerator /
                   s.header().timestamp_denominator * 1e-9L;
      auto elapsed = static_cast<double>(delta);
      if (std::isfinite(elapsed))
        sample.elapsed_seconds = elapsed;
    }
    auto snapshot = s.snapshot_at_event(loc.event_count);
    auto object = snapshot.instance(q.object.id);
    if (!object)
      sample.gap = Gap::object_absent;
    else if (object->generation != q.object.generation)
      sample.gap = Gap::generation_mismatch;
    else if (auto value = object->property(q.property)) {
      auto numeric = read_numeric(
          *value, s.metadata().find_property(object->class_name, q.property),
          q.component, q.item_index, q.coordinate_space);
      sample.value = std::move(numeric.value);
      sample.gap = numeric.gap;
    } else
      sample.gap = Gap::property_missing;
    out.samples.push_back(std::move(sample));
    if (options.progress)
      options.progress(out.samples.size(), count);
    return true;
  });
  return out;
}
void write_curve_csv(std::ostream &out, const Series &series) {
  out << "frame_index,scene_id,stream_id,event_count,timestamp_ticks,time_"
         "seconds,elapsed_seconds,object_id,generation,property,component,item_"
         "index,value_kind,value,gap,break_before,sampling,series_complete,"
         "series_cancelled,"
         "series_resampled,candidate_samples,coordinate_space\r\n";
  for (const auto &sample : series.samples) {
    const auto &q = series.request;
    const auto &s = sample.source;
    std::ostringstream row;
    row.imbue(std::locale::classic());
    row << s.frame_index << ',' << s.scene_id << ',' << s.stream_id << ','
        << s.event_count << ',' << s.timestamp_ticks << ',';
    if (sample.seconds)
      row << numeric_text(Numeric{*sample.seconds});
    row << ',';
    if (sample.elapsed_seconds)
      row << numeric_text(Numeric{*sample.elapsed_seconds});
    row << ',' << q.object.id << ',' << q.object.generation << ','
        << quoted(q.property) << ',' << quoted(q.component) << ','
        << q.item_index << ',';
    if (sample.value)
      row << (std::holds_alternative<std::uint64_t>(*sample.value)  ? "u64"
              : std::holds_alternative<std::int64_t>(*sample.value) ? "i64"
                                                                    : "f64");
    row << ',';
    if (sample.value)
      row << numeric_text(*sample.value);
    row << ',' << gap_name(sample.gap) << ',' << sample.break_before << ','
        << (q.source.sampling == Sampling::native_frame
                ? "native_frame"
                : "explicit_after_fetch")
        << ',' << series.complete << ',' << series.cancelled << ','
        << series.resampled << ',' << series.candidate_samples << ','
        << (q.coordinate_space == CoordinateSpace::native ? "native" : "wire")
        << "\r\n";
    out << row.str();
  }
  if (!out)
    throw std::runtime_error("curve CSV output failed");
}

StateDiff diff_snapshots(const Snapshot &before, const Snapshot &after,
                         DiffOptions options) {
  if (options.coordinate_space != CoordinateSpace::native &&
      options.coordinate_space != CoordinateSpace::wire)
    throw std::invalid_argument("unknown diff coordinate space");
  if (!options.max_objects || !options.max_properties ||
      !options.max_auxiliary_changes || !options.max_retained_value_bytes)
    throw std::invalid_argument("diff resource limits must be nonzero");
  StateDiff out;
  out.coordinate_space = options.coordinate_space;
  out.before_event_count = before.event_count;
  out.after_event_count = after.event_count;
  std::size_t visited_objects = 0, visited_properties = 0, retained = 0,
              auxiliary = 0;
  auto cancel = [&] {
    if (options.cancelled && options.cancelled()) {
      out.complete = false;
      out.cancelled = true;
      return true;
    }
    return false;
  };
  auto limit = [&](const char *reason) {
    out.complete = false;
    out.truncated = true;
    if (out.diagnostics.empty())
      out.diagnostics.emplace_back(reason);
  };
  auto properties = [&](ObjectChange &change, const InstanceState *a,
                        const InstanceState *b) {
    const std::map<std::string, PropertyValue> empty;
    const auto &ap = a ? a->properties : empty, &bp = b ? b->properties : empty;
    auto ai = ap.begin(), bi = bp.begin();
    while (ai != ap.end() || bi != bp.end()) {
      if (cancel())
        return false;
      if (visited_properties >= options.max_properties) {
        limit("diff visited-property limit reached");
        return false;
      }
      ++visited_properties;
      std::string name;
      const PropertyValue *av = nullptr, *bv = nullptr;
      if (bi == bp.end() || (ai != ap.end() && ai->first < bi->first)) {
        name = ai->first;
        av = &ai++->second;
      } else if (ai == ap.end() || bi->first < ai->first) {
        name = bi->first;
        bv = &bi++->second;
      } else {
        name = ai->first;
        av = &ai++->second;
        bv = &bi++->second;
      }
      bool type_changed =
          !av || !bv ||
          (av->incoming_type != bv->incoming_type ||
           av->item_count != bv->item_count ||
           av->byte_order != bv->byte_order ||
           av->pointer_width != bv->pointer_width || av->opaque != bv->opaque);
      const bool wire_changed = !av || !bv || av->raw_bytes() != bv->raw_bytes();
      const bool native_changed = !av || !bv ||
          coordinates::native_bytes(*av) != coordinates::native_bytes(*bv);
      bool data_changed = options.coordinate_space == CoordinateSpace::native
                              ? native_changed : wire_changed;
      const std::optional<std::vector<std::uint64_t>> empty_refs;
      const std::optional<std::string> empty_text;
      const auto &ar = av ? av->reference_ids : empty_refs;
      const auto &br = bv ? bv->reference_ids : empty_refs;
      const auto &at = av ? av->text : empty_text;
      const auto &bt = bv ? bv->text : empty_text;
      bool references_changed = ar != br, text_changed = at != bt;
      if (!type_changed && !data_changed && !references_changed &&
          !text_changed)
        continue;
      auto retain = [&](const PropertyValue *value) {
        if (!value)
          return true;
        auto cost = retained_size(*value);
        if (cost > options.max_retained_value_bytes - retained) {
          limit("diff retained-value byte limit reached");
          return false;
        }
        retained += cost;
        return true;
      };
      if (!retain(av) || !retain(bv))
        return false;
      PropertyChange p;
      p.name = std::move(name);
      if (av)
        p.before = *av;
      if (bv)
        p.after = *bv;
      p.type_changed = type_changed;
      p.data_changed = data_changed;
      p.wire_data_changed = wire_changed;
      p.coordinates_changed = native_changed && !wire_changed;
      p.references_changed = references_changed;
      p.text_changed = text_changed;
      change.properties.push_back(std::move(p));
    }
    return true;
  };
  auto object = [&](const InstanceState *a, const InstanceState *b) {
    if (cancel())
      return false;
    if (visited_objects >= options.max_objects) {
      limit("diff visited-object limit reached");
      return false;
    }
    ++visited_objects;
    ObjectChange change;
    const auto &key = a ? *a : *b;
    change.object = {key.id, key.generation};
    if (a)
      change.before = attributes(*a);
    if (b)
      change.after = attributes(*b);
    if (!a)
      change.kind = ChangeKind::created;
    else if (!b)
      change.kind = ChangeKind::destroyed;
    else {
      change.kind = ChangeKind::changed;
      change.class_changed = a->class_name != b->class_name;
      change.pickable_changed = a->pickable != b->pickable;
      change.top_level_changed = a->top_level != b->top_level;
      change.color_changed = !equal_color(a->color, b->color);
    }
    bool completed = true;
    if ((a && b) || options.include_lifecycle_properties)
      completed = properties(change, a, b);
    if (change.kind != ChangeKind::changed || change.class_changed ||
        change.pickable_changed || change.top_level_changed ||
        change.color_changed || !change.properties.empty())
      out.objects.push_back(std::move(change));
    return completed;
  };
  if (cancel())
    return out;
  auto ai = before.instances.begin(), bi = after.instances.begin();
  while (ai != before.instances.end() || bi != after.instances.end()) {
    if (bi == after.instances.end() ||
        (ai != before.instances.end() && ai->first < bi->first)) {
      if (!object(&ai++->second, nullptr))
        return out;
    } else if (ai == before.instances.end() || bi->first < ai->first) {
      if (!object(nullptr, &bi++->second))
        return out;
    } else {
      const auto &a = ai++->second, &b = bi++->second;
      if (a.generation != b.generation) {
        if (!object(&a, nullptr) || !object(nullptr, &b))
          return out;
      } else if (!object(&a, &b))
        return out;
    }
  }
  auto auxiliary_budget = [&] {
    if (cancel())
      return false;
    if (auxiliary >= options.max_auxiliary_changes) {
      limit("diff auxiliary-change limit reached");
      return false;
    }
    ++auxiliary;
    return true;
  };
  auto ac = before.cameras.begin(), bc = after.cameras.begin();
  while (ac != before.cameras.end() || bc != after.cameras.end()) {
    if (cancel())
      return out;
    std::string name;
    const CameraState *a = nullptr, *b = nullptr;
    if (bc == after.cameras.end() ||
        (ac != before.cameras.end() && ac->first < bc->first)) {
      name = ac->first;
      a = &ac++->second;
    } else if (ac == before.cameras.end() || bc->first < ac->first) {
      name = bc->first;
      b = &bc++->second;
    } else {
      name = ac->first;
      a = &ac++->second;
      b = &bc++->second;
    }
    if (a && b && equal_camera(*a, *b))
      continue;
    if (!auxiliary_budget())
      return out;
    CameraChange change;
    change.name = std::move(name);
    if (a)
      change.before = *a;
    if (b)
      change.after = *b;
    out.cameras.push_back(std::move(change));
  }
  auto as = before.origin_shifts.begin(), bs = after.origin_shifts.begin();
  while (as != before.origin_shifts.end() || bs != after.origin_shifts.end()) {
    if (cancel())
      return out;
    std::uint64_t id;
    const Vec3 *a = nullptr, *b = nullptr;
    if (bs == after.origin_shifts.end() ||
        (as != before.origin_shifts.end() && as->first < bs->first)) {
      id = as->first;
      a = &as++->second;
    } else if (as == before.origin_shifts.end() || bs->first < as->first) {
      id = bs->first;
      b = &bs++->second;
    } else {
      id = as->first;
      a = &as++->second;
      b = &bs++->second;
    }
    if (a && b && equal_vec(*a, *b))
      continue;
    if (!auxiliary_budget())
      return out;
    OriginShiftChange change;
    change.scene_id = id;
    if (a)
      change.before = *a;
    if (b)
      change.after = *b;
    out.origin_shifts.push_back(std::move(change));
  }
  return out;
}
StateDiff diff_events(const Session &s, std::size_t before, std::size_t after,
                      DiffOptions options) {
  if (before > s.event_count() || after > s.event_count())
    throw std::out_of_range("diff event prefix exceeds Session");
  auto cancelled = [&] {
    StateDiff out;
    out.before_event_count = before;
    out.after_event_count = after;
    out.cancelled = true;
    out.complete = false;
    return out;
  };
  if (options.cancelled && options.cancelled())
    return cancelled();
  auto a = s.snapshot_at_event(before);
  if (options.cancelled && options.cancelled())
    return cancelled();
  auto b = s.snapshot_at_event(after);
  return diff_snapshots(a, b, std::move(options));
}
StateDiff diff_frames(const Session &s, const FramePoint &before,
                      const FramePoint &after, DiffOptions options) {
  auto a = locate_frame(s, before), b = locate_frame(s, after);
  if (!a || !b)
    throw std::out_of_range(
        "diff frame does not exist in selected scene/stream/mode");
  return diff_events(s, a->event_count, b->event_count, std::move(options));
}
} // namespace pvd::analysis
