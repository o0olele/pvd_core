// Independent SDK oracle for the headless render extractor. No pvd_core usage.
// CSV values come from public PhysX shape/geometry/mesh APIs and the simulation
// contact callback. PxMeshScale::transform and PxMeshQuery supply mesh references.
#include <PxPhysicsAPI.h>
#include <PxPvdDataStream.h>
#include <PxPvdUserRenderer.h>
#include <task/PxCpuDispatcher.h>
#include <task/PxTask.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace physx;
namespace pvd = physx::pvdsdk;
constexpr float dt = 1.0f / 60.0f;
void need(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void checked(pvd::PvdError code) { need(code==pvd::PvdErrorType::Success,"PVD stream operation failed"); }
std::uint64_t id(const void* value) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)); }
struct Assertions : PxAssertHandler {
    void operator()(const char* expression,const char* file,int line,bool&) override {
        std::fprintf(stderr,"PhysX assertion: %s (%s:%d)\n",expression,file,line); std::fflush(nullptr); std::_Exit(3);
    }
};
struct Errors : PxErrorCallback {
    std::atomic<unsigned> count{0};
    void reportError(PxErrorCode::Enum code,const char* text,const char* file,int line) override {
        std::fprintf(stderr,"PhysX [%d] %s (%s:%d)\n",int(code),text,file,line);
        if(code!=PxErrorCode::eDEBUG_INFO && code!=PxErrorCode::eDEBUG_WARNING) ++count;
    }
};
struct Dispatcher : PxCpuDispatcher {
    void submitTask(PxBaseTask& task) override {task.run();task.release();}
    PxU32 getWorkerCount() const override {return 0;}
};
struct Contact {
    std::uint64_t shape0,shape1;
    PxVec3 position,normal;
    float separation;
};
struct Contacts : PxSimulationEventCallback {
    std::vector<Contact> points;
    void onConstraintBreak(PxConstraintInfo*,PxU32) override {}
    void onWake(PxActor**,PxU32) override {}
    void onSleep(PxActor**,PxU32) override {}
    void onTrigger(PxTriggerPair*,PxU32) override {}
    void onAdvance(const PxRigidBody*const*,const PxTransform*,PxU32) override {}
    void onContact(const PxContactPairHeader&,const PxContactPair* pairs,PxU32 count) override {
        for(PxU32 i=0;i<count;++i) {
            std::array<PxContactPairPoint,64> data{};
            const auto n=pairs[i].extractContacts(data.data(),static_cast<PxU32>(data.size()));
            for(PxU32 j=0;j<n;++j) points.push_back({id(pairs[i].shapes[0]),id(pairs[i].shapes[1]),
                data[j].position,data[j].normal,data[j].separation});
        }
    }
};
PxFilterFlags filter(PxFilterObjectAttributes a,PxFilterData,PxFilterObjectAttributes b,PxFilterData,
                     PxPairFlags& flags,const void*,PxU32) {
    if(PxFilterObjectIsTrigger(a)||PxFilterObjectIsTrigger(b)) flags=PxPairFlag::eTRIGGER_DEFAULT;
    else flags=PxPairFlag::eCONTACT_DEFAULT|PxPairFlag::eNOTIFY_TOUCH_FOUND|
               PxPairFlag::eNOTIFY_TOUCH_PERSISTS|PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}
struct Entry {
    PxRigidActor* actor=nullptr;
    PxShape* shape=nullptr;
    std::uint64_t actor_id=0,shape_id=0;
    const char* name="";
    bool dynamic=false;
};
struct Context {
    PxDefaultAllocator allocator; Errors errors; Dispatcher dispatcher; Contacts contacts;
    PxFoundation* foundation=nullptr; PxPvd* debugger=nullptr; PxPvdTransport* transport=nullptr;
    PxPhysics* physics=nullptr; PxCooking* cooking=nullptr; PxScene* scene=nullptr; PxMaterial* material=nullptr;
    std::vector<Entry> entries;
    std::vector<PxShape*> owned_shapes;
    std::vector<PxBase*> resources;
    void init(const std::string& path) {
        foundation=PxCreateFoundation(PX_FOUNDATION_VERSION,allocator,errors);need(foundation,"foundation");
        debugger=PxCreatePvd(*foundation);need(debugger,"debugger");
        transport=PxDefaultPvdFileTransportCreate(path.c_str());need(transport,"file transport");
        need(debugger->connect(*transport,PxPvdInstrumentationFlag::eDEBUG),"PVD connect");
        physics=PxCreatePhysics(PX_PHYSICS_VERSION,*foundation,PxTolerancesScale(),true,debugger);need(physics,"physics");
        cooking=PxCreateCooking(PX_PHYSICS_VERSION,*foundation,PxCookingParams(physics->getTolerancesScale()));need(cooking,"cooking");
        PxSceneDesc desc(physics->getTolerancesScale());desc.gravity=PxVec3(0,-9.81f,0);
        desc.cpuDispatcher=&dispatcher;desc.filterShader=filter;desc.simulationEventCallback=&contacts;
        desc.flags|=PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
        scene=physics->createScene(desc);need(scene,"scene");
        scene->getScenePvdClient()->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONTACTS,true);
        material=physics->createMaterial(0.4f,0.4f,0.02f);need(material,"material");
    }
    Entry& actor(const char* name,const PxGeometry& geometry,const PxTransform& pose,
                 const PxTransform& local,bool dynamic=false,bool trigger=false,PxShape* shared=nullptr) {
        PxRigidActor* body=dynamic?static_cast<PxRigidActor*>(physics->createRigidDynamic(pose)):
                                   static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
        need(body,"create actor");
        body->setName(name);
        PxShape* shape=shared?shared:physics->createShape(geometry,*material,true);
        if(!shape) {body->release();throw std::runtime_error("create shape");}
        if(!shared) {
            shape->setLocalPose(local);
            if(trigger) {shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE,false);shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE,true);}
        }
        body->attachShape(*shape);
        if(!shared) shape->release();
        if(dynamic) {
            auto* moving=static_cast<PxRigidDynamic*>(body);
            moving->setMass(10);moving->setMassSpaceInertiaTensor(PxVec3(2));
        }
        scene->addActor(*body);
        entries.push_back({body,shape,id(body),id(shape),name,dynamic});
        return entries.back();
    }
    ~Context() {
        for(auto& entry:entries) if(entry.actor) entry.actor->release();
        for(auto* shape:owned_shapes) shape->release();
        for(auto* resource:resources) resource->release();
        if(scene)scene->release();if(material)material->release();if(cooking)cooking->release();if(physics)physics->release();
        if(debugger){debugger->disconnect();debugger->release();}if(transport)transport->release();if(foundation)foundation->release();
    }
};
void vec(std::ofstream& out,const PxVec3& v) {out<<','<<v.x<<','<<v.y<<','<<v.z;}
void quat(std::ofstream& out,const PxQuat& q) {out<<','<<q.x<<','<<q.y<<','<<q.z<<','<<q.w;}
void vertex(std::ofstream& out,unsigned step,const Entry& entry,unsigned index,const PxVec3& raw,const PxVec3& world) {
    out<<"vertex,"<<step<<','<<entry.actor_id<<','<<entry.shape_id<<','<<index;vec(out,raw);vec(out,world);out<<'\n';
}
void triangle(std::ofstream& out,unsigned step,const Entry& entry,unsigned index,const PxU32* indices,const PxTriangle& tri) {
    const auto normal=(tri.verts[1]-tri.verts[0]).cross(tri.verts[2]-tri.verts[0]).getNormalized();
    out<<"triangle,"<<step<<','<<entry.actor_id<<','<<entry.shape_id<<','<<index<<','<<indices[0]<<','<<indices[1]<<','<<indices[2];
    vec(out,normal);out<<'\n';
}
void shapeExpected(std::ofstream& out,unsigned step,const Entry& entry) {
    out<<"shape,"<<step<<','<<entry.actor_id<<','<<entry.shape_id<<','<<(entry.actor?1:0);
    if(!entry.actor) {out<<'\n';return;}
    const auto holder=entry.shape->getGeometry();const auto& geometry=holder.any();
    const auto world=entry.actor->getGlobalPose()*entry.shape->getLocalPose();
    const auto type=geometry.getType();
    const char* kinds[]={"sphere","plane","capsule","box","convex_mesh","triangle_mesh","height_field"};
    out<<','<<kinds[type];vec(out,world.p);quat(out,world.q);
    PxVec3 dims(0);PxMeshScale scale;const void* resource=nullptr;PxBounds3 bounds=PxBounds3::empty();
    std::vector<PxVec3> raw_vertices,world_vertices;
    std::vector<std::array<PxU32,3>> triangles;
    std::vector<PxTriangle> world_triangles;
    if(type==PxGeometryType::eBOX)dims=holder.box().halfExtents;
    if(type==PxGeometryType::eSPHERE)dims.x=holder.sphere().radius;
    if(type==PxGeometryType::eCAPSULE){dims.x=holder.capsule().radius;dims.y=holder.capsule().halfHeight;}
    if(type==PxGeometryType::eTRIANGLEMESH) {
        const auto& g=holder.triangleMesh();resource=g.triangleMesh;scale=g.scale;
        raw_vertices.assign(g.triangleMesh->getVertices(),g.triangleMesh->getVertices()+g.triangleMesh->getNbVertices());
        for(PxU32 i=0;i<g.triangleMesh->getNbTriangles();++i) {
            PxTriangle tri;std::array<PxU32,3> indices{};PxMeshQuery::getTriangle(g,world,i,tri,indices.data());
            triangles.push_back(indices);world_triangles.push_back(tri);
        }
    }
    if(type==PxGeometryType::eCONVEXMESH) {
        const auto& g=holder.convexMesh();resource=g.convexMesh;scale=g.scale;
        raw_vertices.assign(g.convexMesh->getVertices(),g.convexMesh->getVertices()+g.convexMesh->getNbVertices());
        for(PxU32 polygon=0;polygon<g.convexMesh->getNbPolygons();++polygon) {
            PxHullPolygon data;need(g.convexMesh->getPolygonData(polygon,data),"convex polygon");
            const auto* indices=g.convexMesh->getIndexBuffer()+data.mIndexBase;
            for(PxU32 j=1;j+1<data.mNbVerts;++j) {
                std::array<PxU32,3> face{indices[0],indices[j],indices[j+1]};
                PxTriangle tri;for(unsigned k=0;k<3;++k)tri.verts[k]=world.transform(scale.transform(raw_vertices[face[k]]));
                triangles.push_back(face);world_triangles.push_back(tri);
            }
        }
    }
    if(type==PxGeometryType::eHEIGHTFIELD) {
        const auto& g=holder.heightField();resource=g.heightField;
        dims=PxVec3(g.heightScale,g.rowScale,g.columnScale);
        const auto rows=g.heightField->getNbRows(),columns=g.heightField->getNbColumns();
        for(PxU32 r=0;r<rows;++r)for(PxU32 c=0;c<columns;++c)
            raw_vertices.push_back(PxVec3(float(r),float(g.heightField->getSample(r,c).height),float(c)));
        for(PxU32 r=0;r+1<rows;++r)for(PxU32 c=0;c+1<columns;++c)for(unsigned k=0;k<2;++k) {
            const auto face=2*(r*columns+c)+k;
            if(g.heightField->getTriangleMaterialIndex(face)==PxHeightFieldMaterial::eHOLE)continue;
            PxTriangle tri;std::array<PxU32,3> indices{};PxMeshQuery::getTriangle(g,world,face,tri,indices.data());
            triangles.push_back(indices);world_triangles.push_back(tri);
        }
    }
    for(const auto& raw:raw_vertices) {
        const auto scaled=type==PxGeometryType::eHEIGHTFIELD?PxVec3(raw.x*dims.y,raw.y*dims.x,raw.z*dims.z):scale.transform(raw);
        const auto point=world.transform(scaled);world_vertices.push_back(point);bounds.include(point);
    }
    if(raw_vertices.empty() && type!=PxGeometryType::ePLANE)bounds=PxGeometryQuery::getWorldBounds(geometry,world,1.0f);
    vec(out,dims);out<<','<<id(resource);vec(out,scale.scale);quat(out,scale.rotation);
    const auto* moving=entry.dynamic?static_cast<PxRigidDynamic*>(entry.actor):nullptr;
    out<<','<<(entry.shape->getFlags().isSet(PxShapeFlag::eTRIGGER_SHAPE)?1:0)
       <<','<<(moving&&moving->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)?1:0)
       <<','<<(moving&&moving->isSleeping()?1:0);
    if(type==PxGeometryType::ePLANE){vec(out,PxVec3(0));vec(out,PxVec3(0));}
    else {vec(out,bounds.minimum);vec(out,bounds.maximum);}
    out<<','<<unsigned(entry.shape->getFlags())<<','<<(entry.dynamic?1:0)<<'\n';
    if(type==PxGeometryType::eTRIANGLEMESH || type==PxGeometryType::eCONVEXMESH || type==PxGeometryType::eHEIGHTFIELD) {
        const unsigned flags=type==PxGeometryType::eTRIANGLEMESH?unsigned(holder.triangleMesh().meshFlags):
            type==PxGeometryType::eCONVEXMESH?unsigned(holder.convexMesh().meshFlags):unsigned(holder.heightField().heightFieldFlags);
        out<<"mesh_flags,"<<step<<','<<entry.actor_id<<','<<entry.shape_id<<','<<flags<<'\n';
    }
    for(std::size_t i=0;i<raw_vertices.size();++i)vertex(out,step,entry,static_cast<unsigned>(i),raw_vertices[i],world_vertices[i]);
    for(std::size_t i=0;i<triangles.size();++i)triangle(out,step,entry,static_cast<unsigned>(i),triangles[i].data(),world_triangles[i]);
}
struct RenderClient : pvd::RendererEventClient {
    pvd::PvdDataStream& stream;pvd::PvdUserRenderer& renderer;
    RenderClient(pvd::PvdDataStream& s,pvd::PvdUserRenderer& r):stream(s),renderer(r){}
    void handleBufferFlush(const std::uint8_t* data,std::uint32_t length) override {
        checked(stream.setPropertyValue(&renderer,"events",data,length));
    }
};
void debugExpected(std::ofstream& out,unsigned step,pvd::PvdUserRenderer& renderer,const Entry& owner) {
    const PxVec3 shift(float(step),0,0);
    pvd::PvdDebugPoint points[]={{PxVec3(1,2,3)+shift,0xff123456u},{PxVec3(-1,4,2)+shift,0x80654321u}};
    pvd::PvdDebugLine line(PxVec3(0,1,2)+shift,PxVec3(3,4,5)+shift,0xffaabbccu);line.color1=0x88776655u;
    pvd::PvdDebugTriangle tri(PxVec3(0,0,0)+shift,PxVec3(1,0,0)+shift,PxVec3(0,1,0)+shift,0xff00ff00u);
    tri.color1=0xffff0000u;tri.color2=0xff0000ffu;
    pvd::PvdDebugText text(PxVec3(2,3,4)+shift,1.25f,0xfffedcbau,"render fixture");
    renderer.setInstanceId(owner.actor);renderer.drawPoints(points,2);renderer.drawLines(&line,1);
    renderer.drawTriangles(&tri,1);renderer.drawText(text);renderer.flushRenderEvents();
    out<<"renderer,"<<step<<','<<id(&renderer)<<','<<owner.actor_id<<'\n';
    for(unsigned i=0;i<2;++i){out<<"point,"<<step<<','<<owner.actor_id<<','<<i;vec(out,points[i].pos);out<<','<<points[i].color<<'\n';}
    out<<"line,"<<step<<','<<owner.actor_id<<",0";vec(out,line.pos0);out<<','<<line.color0;vec(out,line.pos1);out<<','<<line.color1<<'\n';
    out<<"debug_triangle,"<<step<<','<<owner.actor_id<<",0";vec(out,tri.pos0);out<<','<<tri.color0;vec(out,tri.pos1);out<<','<<tri.color1;
    vec(out,tri.pos2);out<<','<<tri.color2<<'\n';
    out<<"text,"<<step<<','<<owner.actor_id<<",0";vec(out,text.position);out<<','<<text.size<<','<<text.color<<','<<text.string<<'\n';
}
void run(const std::filesystem::path& prefix) {
    if(!prefix.parent_path().empty())std::filesystem::create_directories(prefix.parent_path());
    std::ofstream out(prefix.string()+"_expected.csv");need(bool(out),"expected CSV");out.exceptions(std::ios::badbit|std::ios::failbit);out<<std::setprecision(9);
    out<<"# shape,step,actor,shape,alive,kind,world_p(3),world_q(4),dims(3),resource,scale(3),scale_q(4),trigger,kinematic,sleeping,bounds_min(3),bounds_max(3),shape_flags,dynamic\n"
          "# vertex,step,actor,shape,index,resource_position(3),world_position(3)\n"
          "# triangle,step,actor,shape,index,i0,i1,i2,world_normal(3)\n"
          "# mesh_flags,step,actor,shape,SDK_geometry_flags\n"
          "# renderer,step,renderer_channel_id,owner_id; point/line/debug_triangle/text follow SDK vertex order and ARGB colors\n"
          "# contact,step,shape0,shape1,position(3),normal(3),separation; marker after_fetch contains pose and user rendering\n";
    Context context;context.init(prefix.string()+"_capture.pxd2");context.entries.reserve(16);
    const PxVec3 vertices[]={{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1},{0,1.5f,0}};
    const PxU32 indices[]={0,4,1,1,4,2,2,4,3,3,4,0,0,1,2,0,2,3};
    PxTriangleMeshDesc meshDesc;meshDesc.points.count=5;meshDesc.points.stride=sizeof(PxVec3);meshDesc.points.data=vertices;
    meshDesc.triangles.count=6;meshDesc.triangles.stride=3*sizeof(PxU32);meshDesc.triangles.data=indices;
    auto* mesh=context.cooking->createTriangleMesh(meshDesc,context.physics->getPhysicsInsertionCallback());need(mesh,"triangle cooking");context.resources.push_back(mesh);
    PxConvexMeshDesc convexDesc;convexDesc.points=meshDesc.points;convexDesc.flags=PxConvexFlag::eCOMPUTE_CONVEX;
    auto* convex=context.cooking->createConvexMesh(convexDesc,context.physics->getPhysicsInsertionCallback());need(convex,"convex cooking");context.resources.push_back(convex);
    std::array<PxHeightFieldSample,12> samples{};
    for(unsigned i=0;i<samples.size();++i){samples[i].height=static_cast<PxI16>(int(i%5)-2);samples[i].materialIndex0=0;samples[i].materialIndex1=0;if(i%2)samples[i].setTessFlag();}
    samples[1].materialIndex0=PxHeightFieldMaterial::eHOLE;
    PxHeightFieldDesc hfDesc;hfDesc.nbRows=3;hfDesc.nbColumns=4;hfDesc.samples.data=samples.data();hfDesc.samples.stride=sizeof(PxHeightFieldSample);
    auto* hf=context.cooking->createHeightField(hfDesc,context.physics->getPhysicsInsertionCallback());need(hf,"heightfield cooking");context.resources.push_back(hf);
    context.actor("ground_plane",PxPlaneGeometry(),PxTransform(PxVec3(0),PxQuat(PxHalfPi,PxVec3(0,0,1))),PxTransform(PxIdentity));
    const auto fallingIndex=context.entries.size();context.actor("falling_box",PxBoxGeometry(0.5f,0.5f,0.5f),
        PxTransform(PxVec3(0,0.51f,0),PxQuat(0.2f,PxVec3(0,1,0))),PxTransform(PxIdentity),true);
    context.actor("offset_sphere",PxSphereGeometry(0.7f),PxTransform(PxVec3(5,3,0),PxQuat(0.4f,PxVec3(1,0,0))),
                  PxTransform(PxVec3(0.3f,0.5f,-0.2f),PxQuat(0.25f,PxVec3(0,1,0))));
    const auto kinematicIndex=context.entries.size();auto& kinematic=context.actor("kinematic_capsule",PxCapsuleGeometry(0.4f,1.1f),
        PxTransform(PxVec3(9,3,0),PxQuat(0.4f,PxVec3(0,1,0))),PxTransform(PxVec3(-0.2f,0.4f,0.3f),PxQuat(0.3f,PxVec3(0,0,1))),true);
    static_cast<PxRigidDynamic*>(kinematic.actor)->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC,true);
    context.actor("sleeping_sphere",PxSphereGeometry(0.6f),PxTransform(PxVec3(13,3,0)),PxTransform(PxIdentity),true);
    auto* sleeper=static_cast<PxRigidDynamic*>(context.entries.back().actor);sleeper->setActorFlag(PxActorFlag::eDISABLE_GRAVITY,true);sleeper->putToSleep();
    context.actor("trigger_box",PxBoxGeometry(0.4f,0.7f,0.9f),PxTransform(PxVec3(17,4,0),PxQuat(0.2f,PxVec3(1,0,0))),
        PxTransform(PxVec3(0.2f,0.3f,-0.5f)),false,true);
    const PxMeshScale meshScale(PxVec3(1.25f,0.75f,1.5f),PxQuat(0.35f,PxVec3(0,0,1)));
    const auto removedIndex=context.entries.size();context.actor("triangle_first",PxTriangleMeshGeometry(mesh,meshScale,PxMeshGeometryFlag::eDOUBLE_SIDED),
        PxTransform(PxVec3(21,3,0),PxQuat(0.4f,PxVec3(0,1,0))),PxTransform(PxVec3(0.1f,-0.3f,0.6f),PxQuat(0.3f,PxVec3(1,0,0))));
    context.actor("triangle_reuse",PxTriangleMeshGeometry(mesh,PxMeshScale(PxVec3(0.8f,1.2f,1.7f),PxQuat(-0.3f,PxVec3(0,1,0)))),
        PxTransform(PxVec3(25,3,0),PxQuat(-0.2f,PxVec3(0,0,1))),PxTransform(PxVec3(-0.4f,0.2f,0.1f),PxQuat(0.1f,PxVec3(0,1,0))));
    context.actor("convex",PxConvexMeshGeometry(convex,PxMeshScale(PxVec3(0.9f,1.4f,1.1f),PxQuat(0.45f,PxVec3(0,0,1)))),
        PxTransform(PxVec3(29,3,0),PxQuat(0.3f,PxVec3(0,1,0))),PxTransform(PxVec3(0.3f,-0.4f,0.2f),PxQuat(0.2f,PxVec3(1,0,0))));
    context.actor("heightfield",PxHeightFieldGeometry(hf,PxMeshGeometryFlag::eDOUBLE_SIDED,0.2f,1.1f,0.8f),
        PxTransform(PxVec3(33,3,0),PxQuat(0.25f,PxVec3(0,1,0))),PxTransform(PxVec3(0.2f,0.3f,-0.4f),PxQuat(-0.2f,PxVec3(0,0,1))));
    auto* shared=context.physics->createShape(PxBoxGeometry(0.3f,0.5f,0.7f),*context.material,false);need(shared,"shared shape");
    shared->setLocalPose(PxTransform(PxVec3(0.4f,-0.3f,0.6f),PxQuat(0.3f,PxVec3(0,1,0))));context.owned_shapes.push_back(shared);
    context.actor("shared_actor_a",PxBoxGeometry(),PxTransform(PxVec3(39,3,0),PxQuat(0.4f,PxVec3(1,0,0))),PxTransform(PxIdentity),false,false,shared);
    context.actor("shared_actor_b",PxBoxGeometry(),PxTransform(PxVec3(43,3,0),PxQuat(-0.4f,PxVec3(0,0,1))),PxTransform(PxIdentity),false,false,shared);
    auto* stream=pvd::PvdDataStream::create(context.debugger);need(stream,"validation stream");
    auto* renderer=pvd::PvdUserRenderer::create();need(renderer,"user renderer");
    RenderClient client(*stream,*renderer);renderer->setClient(&client);checked(stream->createInstance(renderer));
    struct Guard {pvd::PvdDataStream* stream;pvd::PvdUserRenderer* renderer;~Guard(){renderer->setClient(nullptr);stream->destroyInstance(renderer);renderer->release();stream->release();}} guard{stream,renderer};
    unsigned totalContacts=0;
    for(unsigned step=1;step<=12;++step) {
        if(step==5){context.entries[removedIndex].actor->release();context.entries[removedIndex].actor=nullptr;context.entries[removedIndex].shape=nullptr;}
        auto* body=static_cast<PxRigidDynamic*>(context.entries[kinematicIndex].actor);
        body->setKinematicTarget(PxTransform(PxVec3(9+float(step)*0.05f,3,0),PxQuat(0.4f,PxVec3(0,1,0))));
        context.contacts.points.clear();context.scene->simulate(dt);need(context.scene->fetchResults(true),"fetch results");
        checked(stream->beginSection(context.scene,"validation_after_fetch"));
        for(const auto& entry:context.entries)shapeExpected(out,step,entry);
        debugExpected(out,step,*renderer,context.entries[fallingIndex]);
        for(const auto& point:context.contacts.points) {
            out<<"contact,"<<step<<','<<point.shape0<<','<<point.shape1;vec(out,point.position);vec(out,point.normal);out<<','<<point.separation<<'\n';++totalContacts;
        }
        checked(stream->endSection(context.scene,"validation_after_fetch"));
    }
    need(totalContacts>0,"fixture must produce real contacts");need(context.errors.count==0,"SDK reported an error");
    std::cout<<"Rendered SDK fixture: "<<prefix.string()<<"_capture.pxd2; 12 steps; "<<totalContacts<<" callback contacts\n";
}
} // namespace
int main(int argc,char** argv) {
    Assertions assertions;PxSetAssertHandler(assertions);
    try {need(argc<=2,"usage: pvd_physx34_render_capture [output-prefix]");run(argc==2?argv[1]:"physx34_render");return 0;}
    catch(const std::exception& error){std::cerr<<"Render capture failed: "<<error.what()<<'\n';return 1;}
}
