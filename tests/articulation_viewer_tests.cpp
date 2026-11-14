#include "imgui.h"
#include "imgui_internal.h"
#include "panels.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace pvd;
using namespace pvd::viewer;
using Sample = std::pair<unsigned, std::string>;
struct Row {
  std::uint64_t object;
  std::string cls, property, value;
  unsigned component;
  char type;
};
void check(bool v, const std::string &message) {
  if (!v)
    throw std::runtime_error(message);
}
struct Oracle {
  std::map<Sample, std::vector<Row>> samples;
  explicit Oracle(const char *path) {
    std::ifstream input(path);
    check(bool(input), "SDK CSV input");
    std::string line;
    std::getline(input, line);
    check(line == "step,phase,object,class,property,component,type,value",
          "SDK CSV header");
    while (std::getline(input, line)) {
      std::vector<std::string> fields;
      std::istringstream split(line);
      std::string field;
      while (std::getline(split, field, ','))
        fields.push_back(field);
      check(fields.size() == 8, "SDK CSV row");
      samples[{static_cast<unsigned>(std::stoul(fields[0])), fields[1]}]
          .push_back({std::stoull(fields[2]), fields[3], fields[4], fields[7],
                      static_cast<unsigned>(std::stoul(fields[5])),
                      fields[6].at(0)});
    }
    check(samples.size() == 41,
          "real initial + dual-scene native/post samples");
  }
  const Row &at(Sample sample, std::uint64_t object, const char *property,
                unsigned component = 0) const {
    for (const auto &r : samples.at(sample))
      if (r.object == object && r.property == property &&
          r.component == component)
        return r;
    throw std::runtime_error("missing SDK oracle property " +
                             std::string(property));
  }
  std::uint64_t integer(Sample sample, std::uint64_t object,
                        const char *property) const {
    return std::stoull(at(sample, object, property).value);
  }
  std::uint64_t named(Sample sample, const std::string &name) const {
    for (const auto &r : samples.at(sample))
      if (r.property == "Name" && r.value == name)
        return r.object;
    throw std::runtime_error("missing SDK oracle name " + name);
  }
  std::set<std::uint64_t> references(Sample sample, std::uint64_t object,
                                     const char *property) const {
    std::set<std::uint64_t> result;
    for (const auto &r : samples.at(sample))
      if (r.object == object && r.property == property && r.type == 'r')
        result.insert(std::stoull(r.value));
    return result;
  }
};
std::vector<std::uint64_t> refs(const InstanceState &o, const char *name) {
  const auto *p = o.property(name);
  return p && p->reference_ids ? *p->reference_ids
                               : std::vector<std::uint64_t>{};
}
ObjectKey key(const InstanceState &o) { return {o.id, o.generation}; }
void inspect(Document &doc, std::uint64_t id) {
  const auto *object = doc.frame()->snapshot.instance(id);
  check(object, "captured reference target exists");
  const auto expected = key(*object);
  check(doc.inspect(id), "captured reference is navigable");
  check(doc.inspected_object() == std::optional<ObjectKey>(expected) &&
            doc.inspected_instance() && doc.inspected_instance()->id == id,
        "navigation preserves actual object generation");
}
std::size_t visible(const Document &doc) {
  return static_cast<std::size_t>(
      std::count_if(doc.frame()->draws.begin(), doc.frame()->draws.end(),
                    [](const auto &draw) { return draw.visible; }));
}
struct Workflow {
  Document doc;
  Oracle oracle;
  std::vector<std::uint64_t> scene_ids;
  std::size_t visits = 0, tree_links = 0, references = 0;
  Workflow(const char *capture, const char *expected) : oracle(expected) {
    doc.open_file(capture);
    for (const auto &frame : doc.session()->frames())
      if (std::find(scene_ids.begin(), scene_ids.end(), frame.scene_id) ==
          scene_ids.end())
        scene_ids.push_back(frame.scene_id);
    check(scene_ids.size() == 2 && doc.scenes().size() == 2,
          "real two-scene document");
    for (const auto &scene : doc.scenes())
      check(scene.native_frames == 10 && scene.after_fetch_frames == 10,
            "both native and explicit fetch timelines are available");
  }
  Sample seek(unsigned scene, SamplePoint point, unsigned step) {
    check(doc.set_scene(scene_ids.at(scene)) && doc.set_sample_point(point) &&
              doc.set_frame(step - 1),
          "public scene/sample/frame navigation");
    const auto sample = Sample{step, point == SamplePoint::after_fetch
                                         ? (scene ? "post1" : "post0")
                                         : (scene ? "frame1" : "frame0")};
    check(doc.timeline().size() == 10 && doc.frame()->marker &&
              doc.frame()->marker->scene_id == scene_ids[scene] &&
              doc.frame()->marker->sample_point == point &&
              doc.frame()->snapshot.event_count ==
                  doc.frame()->marker->event_count,
          "published snapshot is the actual selected marker prefix");
    return sample;
  }
  void graph(Sample sample, unsigned scene) {
    const auto frame = doc.frame();
    std::map<std::uint64_t, std::uint64_t> attachments;
    for (const auto &r : oracle.samples.at(sample))
      if (r.cls == "PxShape" && r.property == "@Present" &&
          oracle.integer(sample, r.object, "@Scene") == scene_ids[scene])
        attachments.emplace(oracle.integer(sample, r.object, "@Actor"),
                            r.object);
    const auto tree = doc.actor_tree();
    check(tree.size() == attachments.size() &&
              frame->draws.size() == attachments.size() &&
              frame->render.instances.size() == attachments.size(),
          "scene tree and viewport contain exact SDK actor/shape attachments");
    for (const auto &actor : tree) {
      check(attachments.count(actor.key.id) && actor.shapes.size() == 1 &&
                actor.shapes[0].key.id == attachments.at(actor.key.id) &&
                actor.shapes[0].present,
            "duplicate native link Shapes do not duplicate tree attachments");
      const auto *source = frame->snapshot.instance(actor.key.id);
      check(source && actor.key == key(*source), "actor tree generation");
      if (source->class_name.name == "PxArticulationLink") {
        ++tree_links;
        const auto parent = refs(*source, "Parent");
        check(parent.size() == 1, "native link has its actual Parent");
        inspect(doc, parent[0]);
        const auto *p = doc.inspected_instance();
        check(p->class_name.name == "PxArticulation" ||
                  p->class_name.name == "PxArticulationLink",
              "root/child Parent navigation targets native graph classes");
        const auto joint = refs(*source, "InboundJoint");
        if (!joint.empty() && joint[0]) {
          inspect(doc, joint[0]);
          const auto *j = doc.inspected_instance();
          check(j->class_name.name == "PxArticulationJoint" &&
                    refs(*j, "Link") == std::vector<std::uint64_t>{source->id},
                "inbound native joint navigates back to its child link");
          const auto *stiffness = j->property("Stiffness");
          check(stiffness &&
                    std::abs(std::stod(propertyValueText(*stiffness)) -
                             std::stod(
                                 oracle.at(sample, j->id, "Stiffness").value)) <
                        1e-6,
                "Inspector presents independent SDK inbound joint parameter");
          ++references;
        }
      }
    }
    std::set<std::uint64_t> grouped;
    for (const auto &group : doc.object_groups())
      for (const auto &object : group.objects) {
        const auto *source = frame->snapshot.instance(object.key.id);
        check(
            source && object.key == key(*source) &&
                group.class_name == source->class_name &&
                grouped.insert(object.key.id).second,
            "all-object tree preserves real classes/generations exactly once");
      }
    check(grouped.size() == frame->snapshot.instances.size(),
          "object tree includes all-scene owners, joints and aggregates");
    for (const auto &r : oracle.samples.at(sample)) {
      if (r.property != "@Present" ||
          (r.cls != "PxArticulation" && r.cls != "PxAggregate"))
        continue;
      inspect(doc, r.object);
      check(!doc.selected_key(),
            "native aggregate/owner inspection does not fabricate a shape");
      check(doc.inspected_instance()->class_name.name == r.cls,
            "native owner class exposed in Inspector");
      for (const char *property : {"Links", "Actors", "Articulations"})
        for (const auto id :
             refs(*frame->snapshot.instance(r.object), property)) {
          inspect(doc, id);
          ++references;
        }
    }
    doc.clear_selection();
    ++visits;
  }
  void replay() {
    for (bool reverse : {false, true})
      for (unsigned scene = 0; scene < 2; ++scene)
        for (const auto point :
             {SamplePoint::native_frame, SamplePoint::after_fetch})
          for (unsigned index = 0; index < 10; ++index) {
            const unsigned step = reverse ? 10 - index : index + 1;
            graph(seek(scene, point, step), scene);
          }
    check(tree_links && references && visits == 80,
          "both source timelines and scene graphs visited in both directions");
    std::cout << "Native viewer graph: " << visits << " samples, " << tree_links
              << " link nodes, " << references << " reference navigations\n";
  }
  void selection() {
    auto sample = seek(0, SamplePoint::after_fetch, 6);
    const auto owner = oracle.named(sample, "Branched & articulation <A>");
    const auto frame = doc.frame();
    const auto roots = refs(*frame->snapshot.instance(owner), "Links");
    const auto found = std::find_if(
        frame->draws.begin(), frame->draws.end(), [&](const auto &draw) {
          const auto *actor = frame->snapshot.instance(draw.key.actor.id);
          return actor && actor->class_name.name == "PxArticulationLink" &&
                 std::find(roots.begin(), roots.end(), draw.key.actor.id) !=
                     roots.end();
        });
    check(found != frame->draws.end(), "actual articulation root attachment");
    const auto selected = found->key;
    const auto pose_bytes = frame->snapshot.instance(selected.actor.id)
                                ->property("GlobalPose")
                                ->raw_bytes();
    check(doc.select(selected) && doc.inspected_object() == selected.shape,
          "native attachment selection inspects exact shape");
    OrbitCamera camera;
    check(doc.fit_selected(camera, 1.6) && std::isfinite(camera.distance) &&
              camera.distance > 0,
          "fit selected native attachment");
    const auto expected_center = [&](unsigned axis) {
      return (std::stof(oracle.at(sample, selected.shape.id, "@BoundsMin", axis)
                            .value) +
              std::stof(oracle.at(sample, selected.shape.id, "@BoundsMax", axis)
                            .value)) *
             .5f;
    };
    check(std::abs(camera.target.x - expected_center(0)) < 1e-4f &&
              std::abs(camera.target.y - expected_center(1)) < 1e-4f &&
              std::abs(camera.target.z - expected_center(2)) < 1e-4f,
          "fit targets independent SDK attachment bounds rather than entire "
          "scene");
    const auto selected_draw =
        std::find_if(doc.frame()->draws.begin(), doc.frame()->draws.end(),
                     [&](const auto &draw) { return draw.key == selected; });
    check(selected_draw != doc.frame()->draws.end() && selected_draw->selected,
          "view override highlights real native attachment");
    doc.set_visible(selected, false);
    check(!doc.visible(selected) && visible(doc) + 1 == frame->draws.size() &&
              !doc.fit_selected(camera),
          "hidden selected attachment is excluded from fit");
    doc.show_all();
    doc.isolate(selected);
    check(doc.isolated() && visible(doc) == 1 && doc.visible(selected),
          "isolate attachment excludes other native owners and rigids");
    const auto ray = camera.screen_ray(800, 500, 1600, 1000);
    const auto hit = pick(doc.frame()->draws, ray);
    check(hit && hit->key == selected && doc.select(hit->key),
          "camera ray picks isolated real articulation attachment");
    doc.show_all();
    doc.set_actor_visible(selected.scene, selected.actor, false);
    check(!doc.visible(selected),
          "native actor hide targets actual generation");
    check(doc.select(selected), "select captured hidden attachment by tree");
    seek(0, SamplePoint::after_fetch, 7);
    check(!doc.selected_key() && !doc.inspected_object() &&
              !doc.select(selected) &&
              visible(doc) == doc.frame()->draws.size(),
          "released/reused native lifetime cannot inherit old selection/hide");
    bool reused = false;
    for (const auto &draw : doc.frame()->draws)
      reused |= (draw.key.actor.id == selected.actor.id &&
                 draw.key.actor.generation != selected.actor.generation) ||
                (draw.key.shape.id == selected.shape.id &&
                 draw.key.shape.generation != selected.shape.generation);
    // This SDK allocator may choose a different individual root address. The
    // fixture must nevertheless exercise native ID reuse somewhere in its
    // graph.
    for (const auto &object : doc.frame()->snapshot.instances) {
      const auto *old = frame->snapshot.instance(object.first);
      reused |= old && old->generation != object.second.generation;
    }
    check(reused,
          "real fixture exercises reused source IDs with new generation");
    seek(0, SamplePoint::after_fetch, 6);
    check(!doc.selected_key() && doc.select(selected) && !doc.visible(selected),
          "reverse seek recovers old lifetime without retargeting selection");
    doc.show_all();
    check(doc.frame()
                  ->snapshot.instance(selected.actor.id)
                  ->property("GlobalPose")
                  ->raw_bytes() == pose_bytes,
          "selection, fit, hide and isolate never mutate captured source pose");
    check(doc.set_scene(scene_ids[1]) && !doc.selected_key() &&
              !doc.inspected_object() && !doc.select(selected),
          "scene switch clears foreign native attachment selection");
    seek(0, SamplePoint::after_fetch, 8);
    check(doc.actor_tree().empty() && doc.frame()->draws.empty() &&
              !doc.fit_scene(camera),
          "source scene is actually empty after populated aggregate migration");
    seek(1, SamplePoint::after_fetch, 8);
    check(doc.actor_tree().size() == 9 && doc.frame()->draws.size() == 9 &&
              doc.fit_scene(camera),
          "destination tree/fit follows all real moved aggregate attachments");
    doc.show_all();
    std::cout << "Native attachment fit/hide/isolate/lifecycle passed\n";
  }
};

struct Harness {
  Document &doc;
  PanelState state;
  float width;
  bool hierarchy = false;
  std::string text;
  float overflow = 0;
  ImGuiWindow *window = nullptr;
  Harness(Document &d, float w) : doc(d), width(w) {}
  void frame(std::optional<ImVec2> mouse = {}, std::optional<bool> down = {}) {
    auto &io = ImGui::GetIO();
    io.DisplaySize = {width, 1100};
    io.DeltaTime = 1.f / 60;
    if (mouse)
      io.AddMousePosEvent(mouse->x, mouse->y);
    if (down)
      io.AddMouseButtonEvent(0, *down);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width, 1000});
    const auto name =
        "Native articulation workflow " + std::to_string(int(width));
    ImGui::Begin(name.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::LogToBuffer();
    if (hierarchy)
      drawHierarchy(doc, state);
    else
      drawInspector(doc, state);
    text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    window = ImGui::GetCurrentWindow();
    overflow =
        std::max(0.f, window->DC.CursorMaxPos.x - window->InnerRect.Max.x);
    ImGui::End();
    ImGui::Render();
    check(state.error.empty(), "real panel action: " + state.error);
  }
  void click(ImVec2 point) {
    frame(point, false);
    frame(point, true);
    frame(point, false);
    frame(point, false);
  }
  void reference(std::uint64_t from, const char *property,
                 std::uint64_t target) {
    inspect(doc, from);
    std::snprintf(state.property_search.data(), state.property_search.size(),
                  "%s", property);
    frame();
    frame();
    auto *table = ImGui::GetCurrentContext()->Tables.GetByKey(
        window->GetID("##properties"));
    check(table && text.find(property) != std::string::npos,
          "actual Inspector reference row");
    const ImVec2 point{table->Columns[1].WorkMinX + 12,
                       (table->RowPosY1 + table->RowPosY2) * .5f};
    click(point);
    check(doc.inspected_object() && doc.inspected_object()->id == target &&
              doc.inspected_instance(),
          "actual mouse reference button navigates native object graph");
    check(overflow <= 1, "native Inspector fits available width");
  }
};
void widgets(Workflow &workflow, float width) {
  auto sample = workflow.seek(0, SamplePoint::after_fetch, 2);
  auto &doc = workflow.doc;
  const auto owner =
      workflow.oracle.named(sample, "Branched & articulation <A>");
  const auto root = refs(*doc.frame()->snapshot.instance(owner), "Links").at(0);
  const auto child = refs(*doc.frame()->snapshot.instance(root), "Links").at(0);
  const auto joint =
      refs(*doc.frame()->snapshot.instance(child), "InboundJoint").at(0);
  Harness h(doc, width);
  h.reference(child, "Parent", root);
  h.reference(root, "Parent", owner);
  h.reference(child, "InboundJoint", joint);
  h.reference(joint, "Link", child);
  inspect(doc, joint);
  std::snprintf(h.state.property_search.data(), h.state.property_search.size(),
                "Stiffness");
  h.frame();
  check(h.text.find("Stiffness") != std::string::npos &&
            h.text.find("PxArticulationJoint") != std::string::npos,
        "real Inspector exposes inbound joint properties");
  inspect(doc, owner);
  h.state.property_search.fill(0);
  h.frame();
  check(h.text.find("MaxProjectionIterations") != std::string::npos &&
            h.text.find("SeparationTolerance") != std::string::npos,
        "real Inspector exposes articulation solver fields");
  const auto aggregate = workflow.oracle.integer(sample, owner, "Aggregate");
  inspect(doc, aggregate);
  h.frame();
  check(h.text.find("MaxNbActors") != std::string::npos &&
            h.text.find("SelfCollision") != std::string::npos &&
            h.text.find("Articulations") != std::string::npos,
        "real Inspector exposes actual aggregate fields and membership");
  h.hierarchy = true;
  h.frame();
  h.frame();
  check(h.text.find("PxArticulationLink") != std::string::npos &&
            h.text.find("PxArticulationJoint") != std::string::npos &&
            h.text.find("PxAggregate") != std::string::npos && h.overflow <= 1,
        "actual all-object categories expose native classes without overflow");
  std::snprintf(h.state.hierarchy_search.data(),
                h.state.hierarchy_search.size(), "Recreated branch");
  workflow.seek(0, SamplePoint::after_fetch, 7);
  h.frame();
  check(h.text.find("Recreated branch") != std::string::npos &&
            h.state.object_count > 0 && h.overflow <= 1,
        "real hierarchy search follows recreated native owner/link names");
  std::cout << "Native ImGui Inspector/tree width=" << width << " passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 3,
          "usage: pvd_articulation_viewer_tests capture.pxd2 expected.csv");
    Workflow workflow(argv[1], argv[2]);
    workflow.replay();
    workflow.selection();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    widgets(workflow, 420);
    widgets(workflow, 1060);
    ImGui::DestroyContext();
    std::cout << "Native articulation/aggregate viewer tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();
    return 1;
  }
}
