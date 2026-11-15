#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "renderer.hpp"
#include "model.hpp"
#include "pvd/body_overlays.hpp"
#include "pvd/coordinate_frames.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace pvd::viewer {
namespace {
using Microsoft::WRL::ComPtr;
void hr(HRESULT result,const char* operation) {
    if(FAILED(result)) {
        std::ostringstream message;message<<operation<<" failed (HRESULT 0x"<<std::hex<<std::uppercase<<static_cast<unsigned long>(result)<<')';
        throw std::runtime_error(message.str());
    }
}
void need(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
bool finite(Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 scale(Vec3 a,float b){return {a.x*b,a.y*b,a.z*b};}
struct Vertex {Vec3 position,normal;std::array<float,4> color;};
struct alignas(16) Constants {
    std::array<float,16> view_projection,world;
    std::array<float,4> color,parameters;
};
std::array<float,4> argb(std::uint32_t value) {
    return {float((value>>16)&255)/255.0f,float((value>>8)&255)/255.0f,float(value&255)/255.0f,float((value>>24)&255)/255.0f};
}
std::array<float,4> rgba(Color color){return {color.r/255.0f,color.g/255.0f,color.b/255.0f,color.a/255.0f};}
Vertex colored(Vec3 position,std::array<float,4> color){return {position,{0,0,0},color};}
constexpr const char* shader=R"HLSL(
cbuffer DrawConstants : register(b0) {
    column_major float4x4 viewProjection;
    column_major float4x4 world;
    float4 tint;
    float4 parameters;
};
struct In {float3 position:POSITION;float3 normal:NORMAL;float4 color:COLOR;};
struct Out {float4 position:SV_POSITION;float3 normal:NORMAL;float4 color:COLOR;};
Out VertexMain(In input) {
    Out output;
    output.position=mul(viewProjection,mul(world,float4(input.position,1)));
    // Presentation-only depth offset for coplanar debug/grid overlays. The
    // captured positions and transforms remain untouched.
    output.position.z-=parameters.y*output.position.w;
    output.normal=mul((float3x3)world,input.normal);
    output.color=input.color*tint;
    return output;
}
float4 PixelMain(Out input,bool front:SV_IsFrontFace):SV_TARGET {
    float lighting=1;
    if(parameters.x>0.5) {
        float3 normal=normalize(input.normal);
        if(!front) normal=-normal;
        lighting=0.32+0.48*saturate(dot(normal,normalize(float3(0.4,0.8,0.5))))
                     +0.18*saturate(dot(normal,normalize(float3(-0.6,0.25,-0.4))));
    }
    return float4(input.color.rgb*lighting,input.color.a);
}
)HLSL";
ComPtr<ID3DBlob> compile(const char* entry,const char* target) {
    ComPtr<ID3DBlob> code,errors;
    const auto result=D3DCompile(shader,std::strlen(shader),"pvd_viewer_scene",nullptr,nullptr,entry,target,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(result)) {
        std::string message="D3DCompile ";message+=entry;
        if(errors)message+=": "+std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());
        hr(result,message.c_str());
    }
    return code;
}
constexpr std::size_t max_dynamic_vertices=4u*1024u*1024u;
void append(std::vector<Vertex>& vertices,const Vertex& vertex) {
    need(vertices.size()<max_dynamic_vertices,"viewer debug/grid vertex budget exceeded");
    need(finite(vertex.position),"viewer debug primitive has nonfinite coordinates");vertices.push_back(vertex);
}
void line(std::vector<Vertex>& vertices,Vec3 a,Vec3 b,std::array<float,4> ca,std::array<float,4> cb) {
    append(vertices,colored(a,ca));append(vertices,colored(b,cb));
}
struct Apartment {
    bool owned=false;
    Apartment(){const auto result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(result!=RPC_E_CHANGED_MODE)hr(result,"CoInitializeEx");owned=SUCCEEDED(result);}
    ~Apartment(){if(owned)CoUninitialize();}
};
float mesh_opacity(const DrawItem& draw,const RenderOptions& options,bool selected) {
    return float(draw.color.a)/255.f*(visualization_scope_matches(options.transparent_scope,selected)?options.transparent_opacity:1.f);
}
bool scope_valid(VisualizationScope scope) {
    return scope==VisualizationScope::none||scope==VisualizationScope::all||scope==VisualizationScope::selected||scope==VisualizationScope::unselected;
}
bool culling_valid(FaceCulling value) {
    return value==FaceCulling::none||value==FaceCulling::front||value==FaceCulling::back;
}
} // namespace

std::vector<DrawItem> visualization_pick_draws(const FrameState& frame,const RenderOptions& options,const VisualizationSelection& selection) {
    need(scope_valid(options.transparent_scope),"unknown transparency scope");
    need(std::isfinite(options.transparent_opacity)&&options.transparent_opacity>=0&&options.transparent_opacity<=1,"viewer opacity must be finite in [0,1]");
    need(culling_valid(options.face_culling),"unknown face culling mode");
    std::vector<DrawItem> result;
    if(!options.show_solids&&!options.show_wireframe)return result;
    const auto selected=visualization_selected_attachments(frame,selection);
    for(const auto& draw:frame.draws)if(draw.visible&&draw.pickable&&draw.mesh&&mesh_opacity(draw,options,selected.count(draw.key)!=0)>0) {
        result.push_back(draw);result.back().face_culling=options.face_culling;
    }
    return result;
}

MultisampleStatus select_multisampling(Antialiasing request,bool color4x,bool depth4x,bool resolve) {
    if(request==Antialiasing::off)return {};
    if(request!=Antialiasing::msaa4x)throw std::invalid_argument("unknown antialiasing mode");
    const bool supported=color4x&&depth4x&&resolve;
    return {4,supported?4u:1u,!supported};
}
struct Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target,depth,multisample_target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants,dynamic_vertices;
    std::array<ComPtr<ID3D11RasterizerState>,3> solid,wire,solid_mirrored,wire_mirrored;
    ComPtr<ID3D11DepthStencilState> depth_write,depth_read,depth_overlay;
    ComPtr<ID3D11BlendState> blend;
    std::uint32_t width=0,height=0;
    std::size_t max_cache=256,dynamic_capacity=0;
    std::uint64_t clock=0,hits=0,misses=0;
    bool mirrored=false,msaa_checked=false,color4x=false,depth4x=false,resolve_color=false;
    MultisampleStatus multisampling;
    RenderStats stats;
    render::BodyOverlays overlays;
    render::CoordinateFrames coordinate_frames;
    struct Mesh {
        std::weak_ptr<const render::TriangleBuffer> owner;
        ComPtr<ID3D11Buffer> vertices,indices;
        UINT index_count=0;
        std::uint64_t last_use=0;
    };
    std::unordered_map<const render::TriangleBuffer*,Mesh> meshes;
    std::unordered_set<const render::TriangleBuffer*> active_meshes;

    void trim_meshes(std::size_t target_count) {
        while(meshes.size()>target_count) {
            auto oldest=meshes.end();
            for(auto it=meshes.begin();it!=meshes.end();++it)
                if(!active_meshes.count(it->first)&&
                   (oldest==meshes.end()||it->second.last_use<oldest->second.last_use))oldest=it;
            if(oldest==meshes.end())break;
            meshes.erase(oldest);
        }
    }

    Impl(ID3D11Device* d,ID3D11DeviceContext* c,std::size_t cache):device(d),context(c),max_cache(cache) {
        need(d&&c,"Renderer requires a D3D11 device and immediate context");
        need(cache>0&&cache<=4096,"viewer GPU mesh cache limit must be in [1,4096]");
        need(c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"Renderer requires an immediate D3D11 context");
        ComPtr<ID3D11Device> owner;c->GetDevice(&owner);need(owner.Get()==d,"renderer device/context mismatch");
        const auto vertex=compile("VertexMain","vs_4_0"),pixel=compile("PixelMain","ps_4_0");
        hr(d->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs),"CreateVertexShader");
        hr(d->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps),"CreatePixelShader");
        const D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,static_cast<UINT>(offsetof(Vertex,position)),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,static_cast<UINT>(offsetof(Vertex,normal)),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,static_cast<UINT>(offsetof(Vertex,color)),D3D11_INPUT_PER_VERTEX_DATA,0}};
        hr(d->CreateInputLayout(elements,3,vertex->GetBufferPointer(),vertex->GetBufferSize(),&layout),"CreateInputLayout");
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(Constants);buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        hr(d->CreateBuffer(&buffer,nullptr,&constants),"CreateBuffer draw constants");
        for(std::size_t mode=0;mode<3;++mode) {
            D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;
            raster.CullMode=mode==0?D3D11_CULL_NONE:mode==1?D3D11_CULL_FRONT:D3D11_CULL_BACK;
            raster.FrontCounterClockwise=TRUE;raster.DepthClipEnable=TRUE;raster.MultisampleEnable=TRUE;
            hr(d->CreateRasterizerState(&raster,&solid[mode]),"CreateRasterizerState solid/cull");
            raster.FrontCounterClockwise=FALSE;
            hr(d->CreateRasterizerState(&raster,&solid_mirrored[mode]),"CreateRasterizerState mirrored solid/cull");
            raster.FrontCounterClockwise=TRUE;raster.FillMode=D3D11_FILL_WIREFRAME;raster.DepthBias=-1;
            hr(d->CreateRasterizerState(&raster,&wire[mode]),"CreateRasterizerState wireframe/cull");
            raster.FrontCounterClockwise=FALSE;
            hr(d->CreateRasterizerState(&raster,&wire_mirrored[mode]),"CreateRasterizerState mirrored wireframe/cull");
        }
        D3D11_DEPTH_STENCIL_DESC depth_desc{};depth_desc.DepthEnable=TRUE;depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        hr(d->CreateDepthStencilState(&depth_desc,&depth_write),"CreateDepthStencilState write");
        depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        hr(d->CreateDepthStencilState(&depth_desc,&depth_read),"CreateDepthStencilState read");
        depth_desc.DepthEnable=FALSE;
        hr(d->CreateDepthStencilState(&depth_desc,&depth_overlay),"CreateDepthStencilState orbit-target overlay");
        D3D11_BLEND_DESC blending{};auto& b=blending.RenderTarget[0];b.BlendEnable=TRUE;
        b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        hr(d->CreateBlendState(&blending,&blend),"CreateBlendState");
    }
    void allocate(std::uint32_t w,std::uint32_t h,MultisampleStatus samples) {
        if(!w||!h){target.Reset();depth.Reset();multisample_target.Reset();rtv.Reset();srv.Reset();dsv.Reset();width=height=0;multisampling=samples;return;}
        const UINT limit=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_0?16384u:8192u;
        need(w<=limit&&h<=limit,"viewer viewport exceeds D3D texture dimensions");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> new_target,new_depth,new_multisample;ComPtr<ID3D11RenderTargetView> new_rtv;
        ComPtr<ID3D11ShaderResourceView> new_srv;ComPtr<ID3D11DepthStencilView> new_dsv;
        hr(device->CreateTexture2D(&desc,nullptr,&new_target),"CreateTexture2D viewport color");
        hr(device->CreateShaderResourceView(new_target.Get(),nullptr,&new_srv),"CreateShaderResourceView viewport");
        if(samples.actual_samples>1) {
            desc.SampleDesc.Count=samples.actual_samples;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            hr(device->CreateTexture2D(&desc,nullptr,&new_multisample),"CreateTexture2D multisampled viewport color");
        }
        hr(device->CreateRenderTargetView(new_multisample?new_multisample.Get():new_target.Get(),nullptr,&new_rtv),"CreateRenderTargetView viewport");
        desc.SampleDesc.Count=samples.actual_samples;
        desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        hr(device->CreateTexture2D(&desc,nullptr,&new_depth),"CreateTexture2D viewport depth");
        hr(device->CreateDepthStencilView(new_depth.Get(),nullptr,&new_dsv),"CreateDepthStencilView viewport");
        target=std::move(new_target);depth=std::move(new_depth);multisample_target=std::move(new_multisample);rtv=std::move(new_rtv);srv=std::move(new_srv);dsv=std::move(new_dsv);
        width=w;height=h;multisampling=samples;
    }
    void resize(std::uint32_t w,std::uint32_t h) {
        if(w==width&&h==height)return;
        allocate(w,h,multisampling);
    }
    void configure_samples(Antialiasing request) {
        if(request==Antialiasing::msaa4x&&!msaa_checked) {
            UINT color_quality=0,depth_quality=0,support=0;
            hr(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,4,&color_quality),"CheckMultisampleQualityLevels viewport color 4x");
            hr(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT,4,&depth_quality),"CheckMultisampleQualityLevels viewport depth 4x");
            hr(device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM,&support),"CheckFormatSupport viewport resolve");
            color4x=color_quality>0;depth4x=depth_quality>0;resolve_color=(support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)!=0;msaa_checked=true;
        }
        const auto requested=select_multisampling(request,color4x,depth4x,resolve_color);
        if(requested.actual_samples!=multisampling.actual_samples&&width&&height)
            allocate(width,height,requested);
        else multisampling=requested;
    }
    Mesh& upload(const std::shared_ptr<const render::TriangleBuffer>& source) {
        const auto found=meshes.find(source.get());
        if(found!=meshes.end()&&!found->second.owner.expired()) {
            ++hits;found->second.last_use=++clock;return found->second;
        }
        if(found!=meshes.end())meshes.erase(found);
        need(source->positions.size()==source->normals.size(),"viewer mesh position/normal count mismatch");
        need(source->indices.size()%3==0,"viewer mesh indices are not a triangle list");
        need(!source->positions.empty()&&!source->indices.empty(),"viewer cannot upload an empty triangle buffer");
        need(source->positions.size()<=std::numeric_limits<UINT>::max()/sizeof(Vertex),"viewer vertex buffer exceeds D3D ByteWidth");
        need(source->indices.size()<=std::numeric_limits<UINT>::max()/sizeof(std::uint32_t),"viewer index buffer exceeds D3D ByteWidth");
        std::vector<Vertex> vertices;vertices.reserve(source->positions.size());
        for(std::size_t i=0;i<source->positions.size();++i) {
            need(finite(source->positions[i])&&finite(source->normals[i]),"viewer mesh has nonfinite vertices/normals");
            vertices.push_back({source->positions[i],source->normals[i],{1,1,1,1}});
        }
        for(auto index:source->indices)need(index<vertices.size(),"viewer triangle index exceeds vertex count");
        Mesh result;result.owner=source;result.index_count=static_cast<UINT>(source->indices.size());result.last_use=++clock;
        D3D11_BUFFER_DESC desc{};desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        desc.ByteWidth=static_cast<UINT>(vertices.size()*sizeof(Vertex));D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=vertices.data();
        hr(device->CreateBuffer(&desc,&initial,&result.vertices),"CreateBuffer cached mesh vertices");
        desc.BindFlags=D3D11_BIND_INDEX_BUFFER;desc.ByteWidth=static_cast<UINT>(source->indices.size()*sizeof(std::uint32_t));initial.pSysMem=source->indices.data();
        hr(device->CreateBuffer(&desc,&initial,&result.indices),"CreateBuffer cached mesh indices");
        // A visible working set larger than the LRU target must remain resident:
        // evicting its first mesh while uploading its last repeats every camera
        // frame. Only inactive resources are candidates, including old retained
        // FrameStates whose CPU meshes have not expired yet.
        trim_meshes(std::max(max_cache,active_meshes.size())-1);
        ++misses;return meshes.emplace(source.get(),std::move(result)).first->second;
    }
    void update(const Mat4& projection,const Mat4& world,std::array<float,4> tint,bool lit,float depth_bias=0) {
        Constants data{projection.values,world.values,tint,{lit?1.0f:0.0f,depth_bias,0,0}};
        context->UpdateSubresource(constants.Get(),0,nullptr,&data,0,0);
    }
    void indexed(Mesh& mesh,const Mat4& projection,const Mat4& world,std::array<float,4> color,bool lines,bool transparent=false,FaceCulling culling=FaceCulling::none) {
        const UINT stride=sizeof(Vertex),offset=0;auto* vb=mesh.vertices.Get();
        context->IASetVertexBuffers(0,1,&vb,&stride,&offset);context->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto mode=static_cast<std::size_t>(culling);
        context->RSSetState(lines?(mirrored?wire_mirrored[mode].Get():wire[mode].Get()):(mirrored?solid_mirrored[mode].Get():solid[mode].Get()));context->OMSetDepthStencilState(lines||transparent?depth_read.Get():depth_write.Get(),0);
        update(projection,world,color,!lines,lines?3e-7f:0);context->DrawIndexed(mesh.index_count,0,0);
        ++stats.draw_calls;stats.triangles+=mesh.index_count/3;
    }
    void draw_dynamic(const std::vector<Vertex>& triangles,const std::vector<Vertex>& lines,const Mat4& projection,bool overlay=false) {
        const auto count=triangles.size()+lines.size();if(!count)return;
        need(count<=max_dynamic_vertices,"viewer combined debug/grid vertex budget exceeded");
        if(count>dynamic_capacity) {
            const auto capacity=std::min<std::size_t>(max_dynamic_vertices,std::max(count,std::max<std::size_t>(1024,dynamic_capacity*2)));
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=static_cast<UINT>(capacity*sizeof(Vertex));desc.Usage=D3D11_USAGE_DYNAMIC;
            desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> buffer;hr(device->CreateBuffer(&desc,nullptr,&buffer),"CreateBuffer dynamic debug geometry");
            dynamic_vertices=std::move(buffer);dynamic_capacity=capacity;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(dynamic_vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map dynamic debug vertices");
        if(!triangles.empty())std::memcpy(mapped.pData,triangles.data(),triangles.size()*sizeof(Vertex));
        if(!lines.empty())std::memcpy(static_cast<std::uint8_t*>(mapped.pData)+triangles.size()*sizeof(Vertex),lines.data(),lines.size()*sizeof(Vertex));
        context->Unmap(dynamic_vertices.Get(),0);
        const UINT stride=sizeof(Vertex),offset=0;auto* buffer=dynamic_vertices.Get();context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
        context->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);context->RSSetState(mirrored?solid_mirrored[0].Get():solid[0].Get());context->OMSetDepthStencilState(overlay?depth_overlay.Get():depth_read.Get(),0);
        if(!triangles.empty()) {
            update(projection,pose_matrix(Transform{}),{1,1,1,1},false,2e-7f);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->Draw(static_cast<UINT>(triangles.size()),0);
            ++stats.draw_calls;stats.triangles+=triangles.size()/3;
        }
        if(!lines.empty()) {
            update(projection,pose_matrix(Transform{}),{1,1,1,1},false,4e-7f);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            context->Draw(static_cast<UINT>(lines.size()),static_cast<UINT>(triangles.size()));++stats.draw_calls;stats.line_segments+=lines.size()/2;
        }
    }
    void render(const FrameState& frame,const OrbitCamera& camera,const RenderOptions& options,const VisualizationSelection& selection) {
        stats={};overlays={};coordinate_frames={};configure_samples(options.antialiasing);stats.multisampling=multisampling;if(!target)return;
        mirrored=camera.handedness==DisplayHandedness::left_handed;
        for(float component:options.background_color)need(std::isfinite(component)&&component>=0&&component<=1,"viewer background color must be finite RGBA in [0,1]");
        need(std::isfinite(options.point_size)&&options.point_size>=1&&options.point_size<=64,"viewer point size must be in [1,64]");
        need(std::isfinite(options.contact_normal_length)&&options.contact_normal_length>=0,"viewer contact normal length must be finite/nonnegative");
        need(std::isfinite(options.particle_velocity_scale)&&options.particle_velocity_scale>=0&&options.particle_velocity_scale<=1e6f,"viewer particle velocity scale must be finite and in [0,1000000]");
        need(std::isfinite(options.rigid_velocity_scale)&&options.rigid_velocity_scale>=0&&options.rigid_velocity_scale<=1e6f,"viewer rigid velocity scale must be finite and in [0,1000000]");
        need(std::isfinite(options.coordinate_frame_scale)&&options.coordinate_frame_scale>=0&&options.coordinate_frame_scale<=1e6f,"viewer coordinate frame scale must be finite in [0,1000000]");
        need(std::isfinite(options.transparent_opacity)&&options.transparent_opacity>=0&&options.transparent_opacity<=1,"viewer opacity must be finite in [0,1]");
        need(culling_valid(options.face_culling),"unknown face culling mode");
        need(valid_color_palette(frame.palette),"viewer palette RGBA must be finite in [0,1]");
        for(const auto scope:{options.bounds_scope,options.velocity_scope,options.scene_query_scope,options.coordinate_frame_scope,options.transparent_scope})need(scope_valid(scope),"unknown visualization scope");
        const auto selected_keys=visualization_selected_attachments(frame,selection);
        const auto spatial_state=[&]() -> const SnapshotView& {
            if(frame.spatial_snapshot)return *frame.spatial_snapshot;
            return frame.snapshot;
        };
        active_meshes.clear();
        if(options.show_solids||options.show_wireframe)
            for(const auto& draw:frame.draws)
                if(draw.visible&&draw.mesh&&!draw.mesh->indices.empty()&&
                   mesh_opacity(draw,options,selected_keys.count(draw.key)!=0)>0)
                    active_meshes.insert(draw.mesh.get());
        for(auto it=meshes.begin();it!=meshes.end();)if(it->second.owner.expired())it=meshes.erase(it);else ++it;
        trim_meshes(std::max(max_cache,active_meshes.size()));
        ID3D11ShaderResourceView* unbound=nullptr;context->PSSetShaderResources(0,1,&unbound);
        auto* output=rtv.Get();context->OMSetRenderTargets(1,&output,dsv.Get());
        context->ClearRenderTargetView(rtv.Get(),options.background_color.data());
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&viewport);
        context->IASetInputLayout(layout.Get());context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        auto* cb=constants.Get();context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
        context->OMSetBlendState(blend.Get(),nullptr,0xffffffffu);
        const auto projection=camera.view_projection(double(width)/height);
        const auto step=2*static_cast<float>(std::pow(10.0,std::floor(std::log10(std::max(0.1,camera.distance)/20.0))));
        const auto axes=display_axes(camera);
        const auto dot=[](Vec3 a,Vec3 b){return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z;};
        const auto center_x=static_cast<float>(std::floor(dot(camera.target,axes.horizontal)/step)*step),center_z=static_cast<float>(std::floor(dot(camera.target,axes.depth)/step)*step);
        const auto grid_position=[&](float x,float z){return add(scale(axes.horizontal,x),scale(axes.depth,z));};
        // Grid is a quiet background guide. Draw it before opaque geometry so a
        // captured plane has a clean surface instead of a dense overlay pattern.
        std::vector<Vertex> grid;
        if(options.show_grid)for(int i=-20;i<=20;++i) {
            const auto offset=i*step;const std::array<float,4> color=i%5==0?std::array<float,4>{0.15f,0.21f,0.27f,0.5f}:std::array<float,4>{0.10f,0.15f,0.20f,0.35f};
            line(grid,grid_position(center_x+offset,center_z-20*step),grid_position(center_x+offset,center_z+20*step),color,color);
            line(grid,grid_position(center_x-20*step,center_z+offset),grid_position(center_x+20*step,center_z+offset),color,color);
        }
        draw_dynamic({},grid,projection);
        std::set<render::PickingKey> triggers;for(const auto& shape:frame.render.instances)if(shape.flags.trigger)triggers.insert(shape.picking_key());
        const auto view=camera.view_matrix();
        std::vector<const DrawItem*> transparent,wire_items;
        const auto tint=[&](const DrawItem& item) {
            const bool selected=selected_keys.count(item.key)!=0;
            auto color=selected?std::array<float,4>{.12f,.78f,.70f,1}:rgba(item.color);
            color[3]=mesh_opacity(item,options,selected);return color;
        };
        for(const auto& item:frame.draws) {
            if(!item.visible||!item.mesh||item.mesh->indices.empty())continue;
            const bool trigger=triggers.count(item.key)!=0;
            // Visibility masks also hide selection overlays. Selection must not
            // accidentally resurrect geometry in a debug-primitives-only view.
            if(!options.show_solids&&!options.show_wireframe)continue;
            const auto color=tint(item);if(color[3]<=0)continue;
            if(options.show_solids&&!trigger) {
                if(color[3]<1)transparent.push_back(&item);
                else indexed(upload(item.mesh),projection,display_matrix(item),color,false,false,options.face_culling);
            }
            if(options.show_wireframe||trigger||selected_keys.count(item.key))wire_items.push_back(&item);
        }
        // Opaque depth is complete before alpha composition. Transparent fills
        // are back-to-front by world-bounds center, depth-tested without writes.
        std::map<render::PickingKey,double> depths;
        if(transparent.size()>1||wire_items.size()>1) {
            const auto depth_of=[&](Vec3 p) {
                need(finite(p),"viewer sorted draw has nonfinite center");
                return -(double(view.values[2])*p.x+double(view.values[6])*p.y+double(view.values[10])*p.z+view.values[14]);
            };
            for(const auto* list:{&transparent,&wire_items})for(const auto* item:*list) {
                auto center=item->world_pose.position;
                if(const auto bounds=display_bounds(*item))center={bounds->minimum.x*.5f+bounds->maximum.x*.5f,bounds->minimum.y*.5f+bounds->maximum.y*.5f,bounds->minimum.z*.5f+bounds->maximum.z*.5f};
                depths.emplace(item->key,depth_of(center));
            }
        }
        const auto far_first=[&](const DrawItem* a,const DrawItem* b) {
            const auto ad=depths.at(a->key),bd=depths.at(b->key);
            return ad!=bd?ad>bd:a->key<b->key;
        };
        if(transparent.size()>1)std::stable_sort(transparent.begin(),transparent.end(),far_first);
        for(const auto* item:transparent) {
            indexed(upload(item->mesh),projection,display_matrix(*item),tint(*item),false,true,options.face_culling);
            ++stats.transparent_draws;
        }
        if(wire_items.size()>1)std::stable_sort(wire_items.begin(),wire_items.end(),far_first);
        for(const auto* item:wire_items) {
            const bool selected=selected_keys.count(item->key)!=0,trigger=triggers.count(item->key)!=0;
            auto color=selected?std::array<float,4>{1,.81f,.29f,1}:
                       trigger?std::array<float,4>{.91f,.64f,.28f,.9f}:std::array<float,4>{.25f,.40f,.48f,.7f};
            color[3]*=mesh_opacity(*item,options,selected);
            indexed(upload(item->mesh),projection,display_matrix(*item),color,true,true,options.face_culling);
        }
        std::vector<Vertex> lines,triangles;lines.reserve(512);
        if(options.show_axes) {
            const auto length=std::max(1.0f,4*step);line(lines,{0,0,0},{length,0,0},{0.9f,0.30f,0.28f,1},{0.9f,0.30f,0.28f,1});
            line(lines,{0,0,0},{0,length,0},{0.28f,0.79f,0.46f,1},{0.28f,0.79f,0.46f,1});
            line(lines,{0,0,0},{0,0,length},{0.28f,0.54f,0.92f,1},{0.28f,0.54f,0.92f,1});
        }
        const Vec3 right{view.values[0],view.values[4],view.values[8]},up{view.values[1],view.values[5],view.values[9]};
        auto point=[&](Vec3 center,std::array<float,4> color,float pixels) {
            const double depth=-(double(view.values[2])*center.x+double(view.values[6])*center.y+double(view.values[10])*center.z+view.values[14]);
            if(depth<=camera.near_plane||depth>camera.far_plane)return;
            const auto half=static_cast<float>(pixels*depth*std::tan(camera.vertical_fov/2)/height);
            const auto x=scale(right,half),y=scale(up,half);
            const Vec3 corners[]={add(add(center,scale(x,-1)),scale(y,-1)),add(add(center,x),scale(y,-1)),add(add(center,x),y),add(add(center,scale(x,-1)),y)};
            for(auto index:{0,1,2,0,2,3})append(triangles,colored(corners[index],color));++stats.points;
        };
        if(options.show_debug_points)for(const auto& p:frame.debug.points)point(p.position,argb(p.color),options.point_size);
        if(options.show_debug_lines)for(const auto& l:frame.debug.lines)line(lines,l.position0,l.position1,argb(l.color0),argb(l.color1));
        if(options.show_joint_frames)for(const auto& l:frame.joint_frame_lines)line(lines,l.position0,l.position1,argb(l.color0),argb(l.color1));
        if(options.show_joint_limits)for(const auto& l:frame.joint_limit_lines)line(lines,l.position0,l.position1,argb(l.color0),argb(l.color1));
        if(options.show_bounds||options.show_linear_velocity||options.show_angular_velocity) {
            auto displayed_geometry=frame.render;
            if(options.show_bounds) {
                std::map<PickingKey,const DrawItem*> displayed_draws;
                for(const auto& draw:frame.draws)displayed_draws.emplace(draw.key,&draw);
                for(auto& shape:displayed_geometry.instances) {
                    const auto found=displayed_draws.find(shape.picking_key());
                    if(found!=displayed_draws.end())shape.world_bounds=display_bounds(*found->second);
                }
            }
            std::size_t remaining=262144;
            for(bool bounds_pass:{true,false}) {
                if(bounds_pass?!options.show_bounds:!options.show_linear_velocity&&!options.show_angular_velocity)continue;
                render::BodyOverlayOptions request;
                request.show_bounds=bounds_pass;
                request.show_linear_velocity=!bounds_pass&&options.show_linear_velocity;
                request.show_angular_velocity=!bounds_pass&&options.show_angular_velocity;
                request.velocity_scale=options.rigid_velocity_scale;request.max_lines=remaining;
                request.max_diagnostics=256-overlays.diagnostics.size();request.visible_attachments.emplace();
                const auto scope=bounds_pass?options.bounds_scope:options.velocity_scope;
                for(const auto& draw:frame.draws)if(draw.visible&&visualization_scope_matches(scope,selected_keys.count(draw.key)!=0))request.visible_attachments->push_back(draw.key);
                auto part=render::build_body_overlays(bounds_pass?displayed_geometry:frame.render,spatial_state(),request);
                remaining-=part.bounds.size()+part.linear_velocity.size()+part.angular_velocity.size();
                overlays.bounds.insert(overlays.bounds.end(),part.bounds.begin(),part.bounds.end());
                overlays.linear_velocity.insert(overlays.linear_velocity.end(),part.linear_velocity.begin(),part.linear_velocity.end());
                overlays.angular_velocity.insert(overlays.angular_velocity.end(),part.angular_velocity.begin(),part.angular_velocity.end());
                overlays.diagnostics.insert(overlays.diagnostics.end(),part.diagnostics.begin(),part.diagnostics.end());
                overlays.bounds_count+=part.bounds_count;overlays.linear_vectors+=part.linear_vectors;overlays.angular_vectors+=part.angular_vectors;
                overlays.processed_attachments+=part.processed_attachments;overlays.complete&=part.complete;
            }
            for(const auto* family:{&overlays.bounds,&overlays.linear_velocity,&overlays.angular_velocity})
                for(const auto& l:*family)line(lines,l.position0,l.position1,argb(l.color),argb(l.color));
            stats.body_bounds=overlays.bounds_count;
            stats.linear_velocity_vectors=overlays.linear_vectors;
            stats.angular_velocity_vectors=overlays.angular_vectors;
        }
        if(options.show_actor_frames||options.show_shape_frames||options.show_mass_frames) {
            render::CoordinateFrameOptions request;
            request.actor=options.show_actor_frames;request.shape=options.show_shape_frames;request.mass=options.show_mass_frames;
            request.axis_length=options.coordinate_frame_scale;request.attachments.emplace();
            for(const auto& draw:frame.draws)if(draw.visible&&visualization_scope_matches(options.coordinate_frame_scope,selected_keys.count(draw.key)!=0))request.attachments->push_back(draw.key);
            coordinate_frames=render::build_coordinate_frames(frame.render,spatial_state(),request);
            for(const auto* family:{&coordinate_frames.actor,&coordinate_frames.shape,&coordinate_frames.mass})
                for(const auto& value:*family)line(lines,value.position0,value.position1,argb(value.color),argb(value.color));
            stats.actor_frames=coordinate_frames.actor_count;stats.shape_frames=coordinate_frames.shape_count;stats.mass_frames=coordinate_frames.mass_count;
            stats.coordinate_frame_diagnostics=coordinate_frames.diagnostics.size();stats.coordinate_frames_complete=coordinate_frames.complete;
        }
        if(options.show_scene_queries) {
            const auto query_color=[&](const render::SceneQuerySource& source,std::uint32_t color) {
                return frame.scene_query_selection&&same_query_source(source,*frame.scene_query_selection)?
                    std::array<float,4>{1,.82f,.23f,1}:argb(color);
            };
            // Selected queries are emitted last so coincident paths/volumes
            // from other calls cannot hide their highlight.
            for(int selected_pass=0;selected_pass<2;++selected_pass) {
            const auto in_pass=[&](const render::SceneQuerySource& source) {
                const bool selected=frame.scene_query_selection&&same_query_source(source,*frame.scene_query_selection);
                return visualization_scope_matches(options.scene_query_scope,selected)&&selected==(selected_pass==1);
            };
            for(const auto& value:frame.scene_queries.lines) {
                if(!in_pass(value.source))continue;
                if(value.kind==render::SceneQueryLineKind::impact_normal&&!options.show_query_normals)continue;
                const auto color=query_color(value.source,value.color);
                line(lines,value.position0,value.position1,color,color);++stats.query_lines;
            }
            if(options.show_query_volumes)for(const auto& volume:frame.query_volumes)if(volume.mesh&&in_pass(volume.source)) {
                const auto color=query_color(volume.source,0xff6ccedbu);
                for(std::size_t i=0;i+2<volume.mesh->indices.size();i+=3) {
                    const auto a=render::transform_point(volume.pose,volume.mesh->positions.at(volume.mesh->indices[i]));
                    const auto b=render::transform_point(volume.pose,volume.mesh->positions.at(volume.mesh->indices[i+1]));
                    const auto c=render::transform_point(volume.pose,volume.mesh->positions.at(volume.mesh->indices[i+2]));
                    line(lines,a,b,color,color);line(lines,b,c,color,color);line(lines,c,a,color,color);
                    stats.query_lines+=3;
                }
                ++stats.query_volumes;
            }
            }
        }
        if(options.show_debug_triangles)for(const auto& t:frame.debug.triangles) {
            append(triangles,colored(t.position0,argb(t.color0)));append(triangles,colored(t.position1,argb(t.color1)));append(triangles,colored(t.position2,argb(t.color2)));
        }
        if(options.show_contacts)for(const auto& c:frame.debug.contacts) {
            point(c.position,{1,0.49f,0.22f,1},options.point_size+1);
            line(lines,c.position,add(c.position,scale(c.normal,options.contact_normal_length)),{1,0.73f,0.31f,1},{1,0.73f,0.31f,1});
        }
        // Native deformables have their own owner/element identity. These are
        // camera-facing point markers and exact fabric edges, never rigid
        // shapes or reconstructed cloth surface triangles.
        const auto visible_owner=[&](deformables::OwnerKey key) {
            return std::any_of(frame.deformable_draws.begin(),frame.deformable_draws.end(),[&](const auto& draw){return draw.key==key&&draw.visible;});
        };
        const auto selected=[&](deformables::OwnerKey key,deformables::ElementKind kind,std::uint32_t index) {
            const auto& selection=frame.deformable_selection;
            return selection&&selection->object==key&&
                (selection->kind==deformables::ElementKind::owner||
                 (selection->sample_event_count==frame.snapshot.event_count&&selection->kind==kind&&selection->index==index));
        };
        if(options.show_cloth)for(const auto& cloth:frame.deformables.cloths) {
            if(!visible_owner(cloth.key))continue;
            const auto base=cloth_color(cloth,frame.palette);
            if(base[3]<=0)continue;
            for(std::size_t i=0;i<cloth.world_positions.size();++i) {
                const bool pin=i<cloth.inverse_masses.size()&&cloth.inverse_masses[i]==0;
                const bool active=selected(cloth.key,deformables::ElementKind::cloth_vertex,static_cast<std::uint32_t>(i));
                point(cloth.world_positions[i],active?std::array<float,4>{1,.82f,.23f,base[3]}:
                    pin&&options.show_cloth_pins?std::array<float,4>{1,.48f,.22f,base[3]}:base,
                    options.point_size+(active?4.f:pin&&options.show_cloth_pins?2.f:0.f));
                ++stats.cloth_points;
            }
            if(options.show_cloth_constraints)for(const auto& edge:cloth.constraints) {
                if(edge.particle0>=cloth.world_positions.size()||edge.particle1>=cloth.world_positions.size())continue;
                const auto color=selected(cloth.key,deformables::ElementKind::cloth_constraint,edge.index)?
                    std::array<float,4>{.15f,.93f,.81f,base[3]}:std::array<float,4>{base[0]*.85f,base[1]*.85f,base[2]*.85f,.8f*base[3]};
                line(lines,cloth.world_positions[edge.particle0],cloth.world_positions[edge.particle1],color,color);++stats.cloth_constraints;
            }
        }
        if(options.show_particles)for(const auto& cloud:frame.deformables.particle_clouds) {
            if(!visible_owner(cloud.key))continue;
            const auto base=particle_color(cloud,frame.palette);
            if(base[3]<=0)continue;
            for(const auto& particle:cloud.particles)if(particle.position) {
                const bool active=selected(cloud.key,deformables::ElementKind::particle,particle.index);
                point(*particle.position,active?std::array<float,4>{1,.82f,.23f,base[3]}:base,options.point_size+(active?4.f:0.f));++stats.particle_points;
                if(options.show_particle_velocity&&particle.velocity) {
                    const auto color=std::array<float,4>{.38f,.87f,.72f,.9f*base[3]};
                    line(lines,*particle.position,add(*particle.position,scale(*particle.velocity,options.particle_velocity_scale)),color,color);
                    ++stats.particle_velocity_vectors;
                }
            }
        }
        draw_dynamic(triangles,lines,projection);
        if(options.show_orbit_target) {
            const auto half=static_cast<float>(7*camera.distance*std::tan(camera.vertical_fov/2)/height);
            const auto x=scale(right,half),y=scale(up,half);
            std::vector<Vertex> marker;
            const std::array<float,4> color{.2f,.95f,.85f,1};
            line(marker,add(camera.target,scale(x,-1)),add(camera.target,x),color,color);
            line(marker,add(camera.target,scale(y,-1)),add(camera.target,y),color,color);
            draw_dynamic({},marker,projection,true);stats.orbit_target_markers=1;
        }
        // Avoid retaining the viewport as an output when the host samples its SRV.
        context->OMSetRenderTargets(0,nullptr,nullptr);
        if(multisample_target)context->ResolveSubresource(target.Get(),0,multisample_target.Get(),0,DXGI_FORMAT_R8G8B8A8_UNORM);
        stats.cached_meshes=meshes.size();stats.cache_hits=hits;stats.cache_misses=misses;
    }
};

Renderer::Renderer(ID3D11Device* device,ID3D11DeviceContext* context,std::size_t limit):impl_(std::make_unique<Impl>(device,context,limit)){}
Renderer::~Renderer()=default;Renderer::Renderer(Renderer&&) noexcept=default;Renderer& Renderer::operator=(Renderer&&) noexcept=default;
void Renderer::resize(std::uint32_t w,std::uint32_t h){impl_->resize(w,h);}
void Renderer::render(const FrameState& frame,const OrbitCamera& camera,const RenderOptions& options,const VisualizationSelection& selection){impl_->render(frame,camera,options,selection);}
ID3D11ShaderResourceView* Renderer::shader_resource_view() const noexcept{return impl_->srv.Get();}
ID3D11Texture2D* Renderer::texture() const noexcept{return impl_->target.Get();}
std::uint32_t Renderer::width() const noexcept{return impl_->width;}
std::uint32_t Renderer::height() const noexcept{return impl_->height;}
RenderStats Renderer::stats() const noexcept{return impl_->stats;}
const render::BodyOverlays& Renderer::body_overlays() const noexcept{return impl_->overlays;}
const render::CoordinateFrames& Renderer::coordinate_frames() const noexcept{return impl_->coordinate_frames;}
void Renderer::clear_mesh_cache() noexcept{impl_->meshes.clear();impl_->stats.cached_meshes=0;}

RgbaImage readTextureRgba8(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source) {
    need(device&&context&&source,"GPU readback requires device, immediate context and texture");
    need(context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"GPU readback requires an immediate context");
    ComPtr<ID3D11Device> owner;source->GetDevice(&owner);need(owner.Get()==device,"GPU readback texture/device mismatch");
    owner.Reset();context->GetDevice(&owner);need(owner.Get()==device,"GPU readback context/device mismatch");
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||
                    desc.Format==DXGI_FORMAT_B8G8R8X8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    need((rgba||bgra)&&desc.ArraySize==1,"GPU PNG readback supports one RGBA8/BGRA8 texture");
    const bool opaque=desc.Format==DXGI_FORMAT_B8G8R8X8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    ComPtr<ID3D11Texture2D> resolved;
    if(desc.SampleDesc.Count>1) {
        UINT support=0;hr(device->CheckFormatSupport(desc.Format,&support),"CheckFormatSupport resolve");
        need((support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)!=0,"GPU texture format does not support resolve");
        auto resolve_desc=desc;resolve_desc.MipLevels=1;resolve_desc.SampleDesc={1,0};resolve_desc.BindFlags=0;
        resolve_desc.CPUAccessFlags=0;resolve_desc.MiscFlags=0;resolve_desc.Usage=D3D11_USAGE_DEFAULT;
        hr(device->CreateTexture2D(&resolve_desc,nullptr,&resolved),"CreateTexture2D screenshot resolve");
        context->ResolveSubresource(resolved.Get(),0,source,0,desc.Format);source=resolved.Get();
    }
    auto staging_desc=desc;staging_desc.MipLevels=1;staging_desc.SampleDesc={1,0};staging_desc.BindFlags=0;
    staging_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;staging_desc.MiscFlags=0;staging_desc.Usage=D3D11_USAGE_STAGING;
    ComPtr<ID3D11Texture2D> staging;hr(device->CreateTexture2D(&staging_desc,nullptr,&staging),"CreateTexture2D screenshot staging");
    context->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,nullptr);
    need(desc.Width&&desc.Height&&std::uint64_t(desc.Width)*desc.Height<=std::numeric_limits<std::size_t>::max()/4,"GPU screenshot dimensions overflow");
    RgbaImage image{desc.Width,desc.Height,std::vector<std::uint8_t>(std::size_t(desc.Width)*desc.Height*4)};
    D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map screenshot staging");
    struct Unmap {ID3D11DeviceContext* context;ID3D11Resource* resource;~Unmap(){context->Unmap(resource,0);}} unmap{context,staging.Get()};
    for(std::uint32_t y=0;y<image.height;++y) {
        auto* destination=image.pixels.data()+std::size_t(y)*image.width*4;
        const auto* input=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        if(rgba)std::memcpy(destination,input,std::size_t(image.width)*4);
        else for(std::uint32_t x=0;x<image.width;++x) {
            destination[x*4]=input[x*4+2];destination[x*4+1]=input[x*4+1];destination[x*4+2]=input[x*4];destination[x*4+3]=opaque?255:input[x*4+3];
        }
    }
    return image;
}
void saveTexturePng(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,const std::filesystem::path& path) {
    need(!path.empty(),"screenshot path must not be empty");auto image=readTextureRgba8(device,context,texture);
    need(image.pixels.size()<=std::numeric_limits<UINT>::max(),"PNG image exceeds WIC byte count");
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    Apartment apartment;ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"CoCreateInstance WIC imaging factory");
    ComPtr<IWICStream> stream;hr(factory->CreateStream(&stream),"WIC CreateStream");
    hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"WIC InitializeFromFilename screenshot");
    ComPtr<IWICBitmapEncoder> encoder;hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"WIC CreateEncoder PNG");
    hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"WIC encoder Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;hr(encoder->CreateNewFrame(&frame,nullptr),"WIC CreateNewFrame");
    hr(frame->Initialize(nullptr),"WIC frame Initialize");hr(frame->SetSize(image.width,image.height),"WIC frame SetSize");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;hr(frame->SetPixelFormat(&format),"WIC frame SetPixelFormat");
    if(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA))for(std::size_t i=0;i<image.pixels.size();i+=4)std::swap(image.pixels[i],image.pixels[i+2]);
    else need(IsEqualGUID(format,GUID_WICPixelFormat32bppRGBA),"PNG encoder negotiated an unsupported pixel format");
    hr(frame->WritePixels(image.height,image.width*4,static_cast<UINT>(image.pixels.size()),image.pixels.data()),"WIC frame WritePixels");
    hr(frame->Commit(),"WIC frame Commit");hr(encoder->Commit(),"WIC encoder Commit");
}
} // namespace pvd::viewer
