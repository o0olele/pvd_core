#include "pvd/render.hpp"
#include "pvd/debug_render.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace pvd;
namespace r = pvd::render;
using Row=std::vector<std::string>;
using Key=std::pair<std::uint64_t,std::uint64_t>;
void require(bool condition,const std::string& message) {if(!condition)throw std::runtime_error(message);}
std::uint64_t u(const Row& row,std::size_t index) {return std::stoull(row.at(index));}
float f(const Row& row,std::size_t index) {return std::stof(row.at(index));}
Vec3 vector(const Row& row,std::size_t index) {return {f(row,index),f(row,index+1),f(row,index+2)};}
bool close(float actual,float expected) {
    return std::isfinite(actual)&&std::abs(actual-expected)<=3e-5f+std::abs(expected)*3e-6f;
}
void scalar(float actual,float expected,const char* message) {require(close(actual,expected),std::string(message)+": "+std::to_string(actual)+" != "+std::to_string(expected));}
void compare(Vec3 actual,Vec3 expected,const char* message) {
    scalar(actual.x,expected.x,message);scalar(actual.y,expected.y,message);scalar(actual.z,expected.z,message);
}
void rotation(Quat actual,const Row& row,std::size_t start,const char* message) {
    const Quat expected{f(row,start),f(row,start+1),f(row,start+2),f(row,start+3)};
    const double dot=double(actual.x)*expected.x+double(actual.y)*expected.y+double(actual.z)*expected.z+double(actual.w)*expected.w;
    require(std::abs(std::abs(dot)-1)<1e-4,message);
}
Vec3 subtract(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 normal(Vec3 a,Vec3 b,Vec3 c) {
    b=subtract(b,a);c=subtract(c,a);
    Vec3 n{b.y*c.z-b.z*c.y,b.z*c.x-b.x*c.z,b.x*c.y-b.y*c.x};
    const auto length=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
    require(length>0,"nondegenerate SDK triangle");return {n.x/length,n.y/length,n.z/length};
}
const r::MeshData* mesh(const r::ShapeInstance& shape) {
    return std::visit([](const auto& geometry)->const r::MeshData* {
        using T=std::decay_t<decltype(geometry)>;
        if constexpr(std::is_same_v<T,r::TriangleMesh>||std::is_same_v<T,r::ConvexMesh>||std::is_same_v<T,r::HeightField>)return geometry.mesh.get();
        else return nullptr;
    },shape.geometry);
}
Vec3 worldVertex(const r::ShapeInstance& shape,Vec3 vertex) {
    const auto scaled=std::visit([&](const auto& geometry)->Vec3 {
        using T=std::decay_t<decltype(geometry)>;
        if constexpr(std::is_same_v<T,r::TriangleMesh>||std::is_same_v<T,r::ConvexMesh>)return r::apply_mesh_scale(geometry.scale,vertex);
        else if constexpr(std::is_same_v<T,r::HeightField>)return {vertex.x*geometry.row_scale,vertex.y*geometry.height_scale,vertex.z*geometry.column_scale};
        else return vertex;
    },shape.geometry);
    return r::transform_point(shape.world_pose,scaled);
}
std::string kind(const r::Geometry& geometry) {
    const char* names[]={"box","sphere","capsule","plane","convex_mesh","triangle_mesh","height_field"};
    return names[static_cast<unsigned>(r::geometry_kind(geometry))];
}
void shapeRow(const Row& row,const std::map<Key,const r::ShapeInstance*>& shapes) {
    const auto found=shapes.find({u(row,2),u(row,3)});
    if(!u(row,4)){require(found==shapes.end(),"removed actor/shape attachment must not render");return;}
    require(found!=shapes.end(),"missing SDK shape attachment "+row[2]+"/"+row[3]);
    const auto& shape=*found->second;require(row.size()==35,"SDK shape CSV schema");
    require(kind(shape.geometry)==row[5],"SDK geometry kind");
    compare(shape.world_pose.position,vector(row,6),"SDK actor*shape-local world translation");
    rotation(shape.world_pose.rotation,row,9,"SDK actor*shape-local world rotation");
    std::visit([&](const auto& geometry) {
        using T=std::decay_t<decltype(geometry)>;
        if constexpr(std::is_same_v<T,r::Box>)compare(geometry.half_extents,vector(row,13),"SDK box half extents");
        if constexpr(std::is_same_v<T,r::Sphere>)scalar(geometry.radius,f(row,13),"SDK sphere radius");
        if constexpr(std::is_same_v<T,r::Capsule>){scalar(geometry.radius,f(row,13),"SDK capsule radius");scalar(geometry.half_height,f(row,14),"SDK capsule half height");}
        if constexpr(std::is_same_v<T,r::TriangleMesh>||std::is_same_v<T,r::ConvexMesh>) {
            compare(geometry.scale.scale,vector(row,17),"SDK mesh scale");rotation(geometry.scale.rotation,row,20,"SDK mesh scale rotation");
        }
        if constexpr(std::is_same_v<T,r::HeightField>) {
            scalar(geometry.height_scale,f(row,13),"SDK heightfield height scale");
            scalar(geometry.row_scale,f(row,14),"SDK heightfield row scale");scalar(geometry.column_scale,f(row,15),"SDK heightfield column scale");
            require(geometry.rows==3 && geometry.columns==4,"SDK heightfield dimensions");
        }
    },shape.geometry);
    if(auto* resource=mesh(shape))require(resource->resource.id==u(row,16),"SDK shared mesh resource ID");
    require(shape.flags.trigger==bool(u(row,24)) && shape.actor.kinematic==bool(u(row,25)),"SDK trigger/kinematic flags");
    if(u(row,34))require(shape.actor.sleeping && *shape.actor.sleeping==bool(u(row,26)),"SDK sleeping state");
    require(shape.actor.kind==(u(row,34)?r::ActorKind::rigid_dynamic:r::ActorKind::rigid_static),"SDK rigid actor kind");
    require(shape.flags.raw==u(row,33),"SDK shape flags");
    if(row[5]=="plane")require(!shape.world_bounds,"plane stays infinite");
    else {
        require(bool(shape.world_bounds),"finite SDK geometry has bounds");
        compare(shape.world_bounds->minimum,vector(row,27),"SDK analytic/vertex-tight lower bound");
        compare(shape.world_bounds->maximum,vector(row,30),"SDK analytic/vertex-tight upper bound");
    }
}
void geometryRows(const std::vector<Row>& rows,const r::Frame& frame) {
    std::map<Key,const r::ShapeInstance*> shapes;
    std::map<Key,std::size_t> vertexCounts,triangleCounts;
    for(const auto& shape:frame.instances)require(shapes.emplace(Key{shape.actor_id,shape.shape_id},&shape).second,"unique actor/shape attachment");
    std::size_t alive=0;
    for(const auto& row:rows) {
        if(row[0]=="shape") {shapeRow(row,shapes);alive+=u(row,4)!=0;}
        if(row[0]=="vertex"||row[0]=="triangle") {
            const auto found=shapes.find({u(row,2),u(row,3)});require(found!=shapes.end(),"mesh expected shape");
            const auto& shape=*found->second;const auto* resource=mesh(shape);require(resource,"SDK mesh resource available");
            const auto index=static_cast<std::size_t>(u(row,4));
            if(row[0]=="vertex") {
                ++vertexCounts[{u(row,2),u(row,3)}];
                require(index<resource->vertices.size(),"SDK resource vertex count");
                compare(resource->vertices[index],vector(row,5),"SDK original resource vertex");
                compare(worldVertex(shape,resource->vertices[index]),vector(row,8),"SDK scaled world vertex");
            } else {
                ++triangleCounts[{u(row,2),u(row,3)}];
                require(index<resource->indices.size()/3,"SDK emitted triangle count");
                const auto base=index*3;
                for(std::size_t j=0;j<3;++j)require(resource->indices[base+j]==u(row,5+j),"SDK native triangle winding/heightfield tessellation/hole");
                compare(normal(worldVertex(shape,resource->vertices[resource->indices[base]]),
                               worldVertex(shape,resource->vertices[resource->indices[base+1]]),
                               worldVertex(shape,resource->vertices[resource->indices[base+2]])),vector(row,8),"SDK world triangle normal");
            }
        }
        if(row[0]=="mesh_flags") {
            const auto found=shapes.find({u(row,2),u(row,3)});require(found!=shapes.end(),"mesh flags expected shape");
            std::visit([&](const auto& geometry) {
                using T=std::decay_t<decltype(geometry)>;
                if constexpr(std::is_same_v<T,r::TriangleMesh>||std::is_same_v<T,r::ConvexMesh>) {
                    require(geometry.mesh_flags==u(row,4),"SDK mesh geometry flags");
                    if constexpr(std::is_same_v<T,r::TriangleMesh>)require(geometry.double_sided==bool(u(row,4)&2),"SDK triangle double-sided flag");
                } else if constexpr(std::is_same_v<T,r::HeightField>) {
                    require(geometry.flags==u(row,4)&&geometry.double_sided==bool(u(row,4)&2),"SDK heightfield flags");
                } else require(false,"mesh flags on primitive geometry");
            },found->second->geometry);
        }
    }
    for(const auto& shape:shapes)if(const auto* data=mesh(*shape.second)) {
        require(data->vertices.size()==vertexCounts[shape.first],"SDK exact resource vertex count");
        require(data->indices.size()==triangleCounts[shape.first]*3,"SDK exact triangulation count including HF holes");
    }
    require(frame.instances.size()==alive,"no missing or extra SDK shape attachments");
    require(frame.diagnostics.empty(),"valid SDK scene should extract without diagnostics"+
        (frame.diagnostics.empty()?std::string{}:(": "+frame.diagnostics[0].property+" "+frame.diagnostics[0].message)));
}
void debugRows(const std::vector<Row>& rows,const r::DebugResult& debug,std::uint64_t renderer,std::uint64_t owner) {
    require(debug.complete&&debug.diagnostics.empty(),"SDK user-render packet decodes completely");
    require(debug.points.size()==2&&debug.lines.size()==1&&debug.triangles.size()==1&&debug.texts.size()==1,"SDK points/lines/triangles/text counts");
    auto source=[&](const r::DebugSource& src){require(src.channel_instance_id==renderer&&src.instance_id==owner,"SDK debug attribution and picking ID");};
    for(const auto& row:rows) {
        if(row[0]=="point") {
            const auto& point=debug.points.at(static_cast<std::size_t>(u(row,3)));source(point.source);
            compare(point.position,vector(row,4),"SDK debug point");require(point.color==u(row,7),"SDK point ARGB");
        }
        if(row[0]=="line") {
            const auto& line=debug.lines.at(static_cast<std::size_t>(u(row,3)));source(line.source);
            compare(line.position0,vector(row,4),"SDK debug line vertex0");compare(line.position1,vector(row,8),"SDK debug line vertex1");
            require(line.color0==u(row,7)&&line.color1==u(row,11),"SDK line per-vertex ARGB");
        }
        if(row[0]=="debug_triangle") {
            const auto& triangle=debug.triangles.at(static_cast<std::size_t>(u(row,3)));source(triangle.source);
            compare(triangle.position0,vector(row,4),"SDK debug triangle vertex0");compare(triangle.position1,vector(row,8),"SDK debug triangle vertex1");
            compare(triangle.position2,vector(row,12),"SDK debug triangle vertex2");
            require(triangle.color0==u(row,7)&&triangle.color1==u(row,11)&&triangle.color2==u(row,15),"SDK triangle per-vertex ARGB");
        }
        if(row[0]=="text") {
            const auto& text=debug.texts.at(static_cast<std::size_t>(u(row,3)));source(text.source);
            compare(text.position,vector(row,4),"SDK debug text position");scalar(text.size,f(row,7),"SDK text size");
            require(text.color==u(row,8)&&text.text==row[9],"SDK text content and ARGB");
        }
    }
}
void contactRows(const std::vector<Row>& rows,const r::DebugResult& debug) {
    require(debug.complete&&debug.contacts_available&&debug.diagnostics.empty(),"SDK native PvdContact array decodes");
    std::vector<bool> used(debug.contacts.size(),false);std::size_t expected=0;
    for(const auto& row:rows)if(row[0]=="contact") {
        ++expected;bool matched=false;
        for(std::size_t i=0;i<debug.contacts.size();++i)if(!used[i]) {
            const auto& contact=debug.contacts[i];
            const bool same=contact.shape0==u(row,2)&&contact.shape1==u(row,3);
            const bool reversed=contact.shape0==u(row,3)&&contact.shape1==u(row,2);
            if(!(same||reversed))continue;
            const auto position=vector(row,4);
            if(!close(contact.position.x,position.x)||!close(contact.position.y,position.y)||!close(contact.position.z,position.z))continue;
            compare(contact.position,position,"SDK callback contact point");
            auto normal=vector(row,7);if(reversed)normal={-normal.x,-normal.y,-normal.z};
            compare(contact.normal,normal,"SDK callback contact normal");scalar(contact.separation,f(row,10),"SDK callback contact separation");
            used[i]=true;matched=true;break;
        }
        require(matched,"every SDK callback contact has a matching PVD contact");
    }
    require(expected>0&&expected==debug.contacts.size(),"SDK callback and PVD contacts are a bijection");
}
std::map<unsigned,std::vector<Row>> readExpected(const std::string& path) {
    std::ifstream file(path);require(bool(file),"open SDK expected CSV");std::map<unsigned,std::vector<Row>> rows;std::string line;
    while(std::getline(file,line)) {
        if(line.empty()||line[0]=='#')continue;if(line.back()=='\r')line.pop_back();
        std::istringstream input(line);Row row;std::string field;
        while(std::getline(input,field,','))row.push_back(field);
        require(row.size()>=2,"expected CSV row");rows[static_cast<unsigned>(u(row,1))].push_back(std::move(row));
    }
    return rows;
}
void run(const std::string& capture,const std::string& expectedPath) {
    SessionOptions options;options.checkpoint_interval=32;auto session=Session::load_file(capture,options);
    auto expected=readExpected(expectedPath);require(expected.size()==12,"12 independent SDK expected steps");
    std::vector<SectionMarker> markers;
    for(const auto& section:session.sections())if(section.name=="validation_after_fetch")markers.push_back(section);
    require(markers.size()==12&&session.frames().size()==12,"explicit post-fetch markers and native frames");
    r::Extractor extractor;std::vector<r::Frame> retained;std::set<r::GeometryKind> kinds;
    for(std::size_t step=1;step<=markers.size();++step) {
        const auto& marker=markers[step-1];const auto snapshot=session.snapshot_at_event(marker.event_count);
        r::ExtractOptions selection;selection.scene_id=marker.section_id;
        auto frame=extractor.extract(session.metadata(),snapshot,selection);geometryRows(expected.at(static_cast<unsigned>(step)),frame);
        for(const auto& shape:frame.instances)kinds.insert(r::geometry_kind(shape.geometry));
        std::uint64_t renderer=0,owner=0;
        for(const auto& row:expected.at(static_cast<unsigned>(step)))if(row[0]=="renderer"){renderer=u(row,2);owner=u(row,3);}
        require(renderer&&owner,"independent SDK renderer IDs");
        r::DebugRange range{marker.begin_event_count,marker.event_count,marker.section_id,{renderer}};
        debugRows(expected.at(static_cast<unsigned>(step)),r::extract_debug(snapshot,session.metadata(),session.channels(),range),renderer,owner);
        contactRows(expected.at(static_cast<unsigned>(step)),r::extract_contacts(session.snapshot_at_frame(step-1),session.metadata(),marker.section_id));
        retained.push_back(std::move(frame));
    }
    require(kinds.size()==7,"all seven PhysX geometry kinds validated");
    const auto& first=retained.front();std::map<std::uint64_t,std::vector<const r::ShapeInstance*>> shapeOwners;
    std::map<std::uint64_t,const r::MeshData*> meshes;
    bool sharedShape=false,reusedResource=false;
    for(const auto& shape:first.instances) {
        shapeOwners[shape.shape_id].push_back(&shape);
        if(const auto* data=mesh(shape)) {
            auto [found,inserted]=meshes.emplace(data->resource.id,data);
            if(!inserted){require(found->second==data,"shared SDK resource uses immutable cache entry");reusedResource=true;}
        }
    }
    for(const auto& owners:shapeOwners)if(owners.second.size()>1) {
        require(owners.second[0]->actor_id!=owners.second[1]->actor_id&&
                !(owners.second[0]->picking_key()==owners.second[1]->picking_key()),"shared shape has distinct attachment picking keys");sharedShape=true;
        r::ExtractOptions selected;selected.scene_id=owners.second[0]->scene_id;selected.selected.push_back(owners.second[0]->picking_key());
        const auto frame=extractor.extract(session.metadata(),session.snapshot_at_event(markers[0].event_count),selected);
        require(std::count_if(frame.instances.begin(),frame.instances.end(),[](const auto& shape){return shape.selected;})==1,"selection identifies one shared-shape attachment");
    }
    require(sharedShape&&reusedResource&&extractor.cache_stats().hits>0,"SDK sharing and mesh cache reuse exercised");
    extractor.clear_cache();
    // Previously returned frames must retain their immutable mesh data after
    // eviction; reverse seeks must not depend on extraction history.
    geometryRows(expected.at(1),retained.front());
    for(std::size_t step=markers.size();step>0;--step) {
        r::ExtractOptions selection;selection.scene_id=markers[step-1].section_id;
        const auto frame=extractor.extract(session.metadata(),session.snapshot_at_event(markers[step-1].event_count),selection);
        geometryRows(expected.at(static_cast<unsigned>(step)),frame);
    }
    std::cout<<"PhysX 3.4 render replay passed: seven geometries, 12 post-fetch states, SDK mesh vertices/normals, shared shapes/resources, debug primitives, 48 contacts\n";
}
} // namespace
int main(int argc,char** argv) {
    try {require(argc==3,"usage: pvd_physx34_render <capture.pxd2> <expected.csv>");run(argv[1],argv[2]);return 0;}
    catch(const std::exception& error){std::cerr<<"PhysX render integration failed: "<<error.what()<<'\n';return 1;}
}
