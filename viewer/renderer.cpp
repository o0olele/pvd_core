#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "renderer.hpp"
#include "model.hpp"
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
} // namespace

struct Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target,depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants,dynamic_vertices;
    ComPtr<ID3D11RasterizerState> solid,wire;
    ComPtr<ID3D11DepthStencilState> depth_write,depth_read;
    ComPtr<ID3D11BlendState> blend;
    std::uint32_t width=0,height=0;
    std::size_t max_cache=256,dynamic_capacity=0;
    std::uint64_t clock=0,hits=0,misses=0;
    RenderStats stats;
    struct Mesh {
        std::weak_ptr<const render::TriangleBuffer> owner;
        ComPtr<ID3D11Buffer> vertices,indices;
        UINT index_count=0;
        std::uint64_t last_use=0;
    };
    std::unordered_map<const render::TriangleBuffer*,Mesh> meshes;

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
        D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
        raster.FrontCounterClockwise=TRUE;raster.DepthClipEnable=TRUE;
        hr(d->CreateRasterizerState(&raster,&solid),"CreateRasterizerState solid");
        raster.FillMode=D3D11_FILL_WIREFRAME;raster.DepthBias=-1;
        hr(d->CreateRasterizerState(&raster,&wire),"CreateRasterizerState selection wireframe");
        D3D11_DEPTH_STENCIL_DESC depth_desc{};depth_desc.DepthEnable=TRUE;depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        hr(d->CreateDepthStencilState(&depth_desc,&depth_write),"CreateDepthStencilState write");
        depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        hr(d->CreateDepthStencilState(&depth_desc,&depth_read),"CreateDepthStencilState read");
        D3D11_BLEND_DESC blending{};auto& b=blending.RenderTarget[0];b.BlendEnable=TRUE;
        b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        hr(d->CreateBlendState(&blending,&blend),"CreateBlendState");
    }
    void resize(std::uint32_t w,std::uint32_t h) {
        if(w==width&&h==height)return;
        if(!w||!h){target.Reset();depth.Reset();rtv.Reset();srv.Reset();dsv.Reset();width=height=0;return;}
        const UINT limit=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_0?16384u:8192u;
        need(w<=limit&&h<=limit,"viewer viewport exceeds D3D texture dimensions");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> new_target,new_depth;ComPtr<ID3D11RenderTargetView> new_rtv;
        ComPtr<ID3D11ShaderResourceView> new_srv;ComPtr<ID3D11DepthStencilView> new_dsv;
        hr(device->CreateTexture2D(&desc,nullptr,&new_target),"CreateTexture2D viewport color");
        hr(device->CreateRenderTargetView(new_target.Get(),nullptr,&new_rtv),"CreateRenderTargetView viewport");
        hr(device->CreateShaderResourceView(new_target.Get(),nullptr,&new_srv),"CreateShaderResourceView viewport");
        desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        hr(device->CreateTexture2D(&desc,nullptr,&new_depth),"CreateTexture2D viewport depth");
        hr(device->CreateDepthStencilView(new_depth.Get(),nullptr,&new_dsv),"CreateDepthStencilView viewport");
        target=std::move(new_target);depth=std::move(new_depth);rtv=std::move(new_rtv);srv=std::move(new_srv);dsv=std::move(new_dsv);
        width=w;height=h;
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
        while(meshes.size()>=max_cache) {
            const auto oldest=std::min_element(meshes.begin(),meshes.end(),[](const auto& a,const auto& b){return a.second.last_use<b.second.last_use;});
            meshes.erase(oldest);
        }
        ++misses;return meshes.emplace(source.get(),std::move(result)).first->second;
    }
    void update(const Mat4& projection,const Mat4& world,std::array<float,4> tint,bool lit,float depth_bias=0) {
        Constants data{projection.values,world.values,tint,{lit?1.0f:0.0f,depth_bias,0,0}};
        context->UpdateSubresource(constants.Get(),0,nullptr,&data,0,0);
    }
    void indexed(Mesh& mesh,const Mat4& projection,const Mat4& world,std::array<float,4> color,bool lines) {
        const UINT stride=sizeof(Vertex),offset=0;auto* vb=mesh.vertices.Get();
        context->IASetVertexBuffers(0,1,&vb,&stride,&offset);context->IASetIndexBuffer(mesh.indices.Get(),DXGI_FORMAT_R32_UINT,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetState(lines?wire.Get():solid.Get());context->OMSetDepthStencilState(lines?depth_read.Get():depth_write.Get(),0);
        update(projection,world,color,!lines,lines?3e-7f:0);context->DrawIndexed(mesh.index_count,0,0);
        ++stats.draw_calls;stats.triangles+=mesh.index_count/3;
    }
    void draw_dynamic(const std::vector<Vertex>& triangles,const std::vector<Vertex>& lines,const Mat4& projection) {
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
        context->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);context->RSSetState(solid.Get());context->OMSetDepthStencilState(depth_read.Get(),0);
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
    void render(const FrameState& frame,const OrbitCamera& camera,const RenderOptions& options) {
        stats={};if(!target)return;
        need(std::isfinite(options.point_size)&&options.point_size>=1&&options.point_size<=64,"viewer point size must be in [1,64]");
        need(std::isfinite(options.contact_normal_length)&&options.contact_normal_length>=0,"viewer contact normal length must be finite/nonnegative");
        need(std::isfinite(options.particle_velocity_scale)&&options.particle_velocity_scale>=0&&options.particle_velocity_scale<=1e6f,"viewer particle velocity scale must be finite and in [0,1000000]");
        for(auto it=meshes.begin();it!=meshes.end();)if(it->second.owner.expired())it=meshes.erase(it);else ++it;
        ID3D11ShaderResourceView* unbound=nullptr;context->PSSetShaderResources(0,1,&unbound);
        auto* output=rtv.Get();context->OMSetRenderTargets(1,&output,dsv.Get());
        constexpr float background[]={0.043f,0.063f,0.086f,1};context->ClearRenderTargetView(rtv.Get(),background);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&viewport);
        context->IASetInputLayout(layout.Get());context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        auto* cb=constants.Get();context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
        context->OMSetBlendState(blend.Get(),nullptr,0xffffffffu);
        const auto projection=camera.view_projection(double(width)/height);
        const auto step=2*static_cast<float>(std::pow(10.0,std::floor(std::log10(std::max(0.1,camera.distance)/20.0))));
        const auto center_x=static_cast<float>(std::floor(camera.target.x/step)*step),center_z=static_cast<float>(std::floor(camera.target.z/step)*step);
        // Grid is a quiet background guide. Draw it before opaque geometry so a
        // captured plane has a clean surface instead of a dense overlay pattern.
        std::vector<Vertex> grid;
        if(options.show_grid)for(int i=-20;i<=20;++i) {
            const auto offset=i*step;const std::array<float,4> color=i%5==0?std::array<float,4>{0.15f,0.21f,0.27f,0.5f}:std::array<float,4>{0.10f,0.15f,0.20f,0.35f};
            line(grid,{center_x+offset,0,center_z-20*step},{center_x+offset,0,center_z+20*step},color,color);
            line(grid,{center_x-20*step,0,center_z+offset},{center_x+20*step,0,center_z+offset},color,color);
        }
        draw_dynamic({},grid,projection);
        std::set<render::PickingKey> triggers;for(const auto& shape:frame.render.instances)if(shape.flags.trigger)triggers.insert(shape.picking_key());
        for(const auto& item:frame.draws) {
            if(!item.visible||!item.mesh||item.mesh->indices.empty())continue;
            const bool trigger=triggers.count(item.key)!=0;
            // Visibility masks also hide selection overlays. Selection must not
            // accidentally resurrect geometry in a debug-primitives-only view.
            if(!options.show_solids&&!options.show_wireframe)continue;
            auto& mesh=upload(item.mesh);const auto world=pose_matrix(item.world_pose);
            if(options.show_solids&&!trigger)indexed(mesh,projection,world,item.selected?std::array<float,4>{0.12f,0.78f,0.70f,1}:rgba(item.color),false);
            if(options.show_wireframe||trigger||item.selected)
                indexed(mesh,projection,world,item.selected?std::array<float,4>{1,0.81f,0.29f,1}:
                    trigger?std::array<float,4>{0.91f,0.64f,0.28f,0.9f}:std::array<float,4>{0.25f,0.40f,0.48f,0.7f},true);
        }
        std::vector<Vertex> lines,triangles;lines.reserve(512);
        if(options.show_axes) {
            const auto length=std::max(1.0f,4*step);line(lines,{0,0,0},{length,0,0},{0.9f,0.30f,0.28f,1},{0.9f,0.30f,0.28f,1});
            line(lines,{0,0,0},{0,length,0},{0.28f,0.79f,0.46f,1},{0.28f,0.79f,0.46f,1});
            line(lines,{0,0,0},{0,0,length},{0.28f,0.54f,0.92f,1},{0.28f,0.54f,0.92f,1});
        }
        const auto view=camera.view_matrix();const Vec3 right{view.values[0],view.values[4],view.values[8]},up{view.values[1],view.values[5],view.values[9]};
        auto point=[&](Vec3 center,std::array<float,4> color,float pixels) {
            const double depth=-(double(view.values[2])*center.x+double(view.values[6])*center.y+double(view.values[10])*center.z+view.values[14]);
            if(depth<=camera.near_plane)return;
            const auto half=static_cast<float>(pixels*depth*std::tan(camera.vertical_fov/2)/height);
            const auto x=scale(right,half),y=scale(up,half);
            const Vec3 corners[]={add(add(center,scale(x,-1)),scale(y,-1)),add(add(center,x),scale(y,-1)),add(add(center,x),y),add(add(center,scale(x,-1)),y)};
            for(auto index:{0,1,2,0,2,3})append(triangles,colored(corners[index],color));++stats.points;
        };
        if(options.show_debug_points)for(const auto& p:frame.debug.points)point(p.position,argb(p.color),options.point_size);
        if(options.show_debug_lines)for(const auto& l:frame.debug.lines)line(lines,l.position0,l.position1,argb(l.color0),argb(l.color1));
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
            const auto base=cloth.color?rgba(*cloth.color):std::array<float,4>{.43f,.69f,.82f,1};
            for(std::size_t i=0;i<cloth.world_positions.size();++i) {
                const bool pin=i<cloth.inverse_masses.size()&&cloth.inverse_masses[i]==0;
                const bool active=selected(cloth.key,deformables::ElementKind::cloth_vertex,static_cast<std::uint32_t>(i));
                point(cloth.world_positions[i],active?std::array<float,4>{1,.82f,.23f,1}:
                    pin&&options.show_cloth_pins?std::array<float,4>{1,.48f,.22f,1}:base,
                    options.point_size+(active?4.f:pin&&options.show_cloth_pins?2.f:0.f));
                ++stats.cloth_points;
            }
            if(options.show_cloth_constraints)for(const auto& edge:cloth.constraints) {
                if(edge.particle0>=cloth.world_positions.size()||edge.particle1>=cloth.world_positions.size())continue;
                const auto color=selected(cloth.key,deformables::ElementKind::cloth_constraint,edge.index)?
                    std::array<float,4>{.15f,.93f,.81f,1}:std::array<float,4>{base[0]*.85f,base[1]*.85f,base[2]*.85f,.8f};
                line(lines,cloth.world_positions[edge.particle0],cloth.world_positions[edge.particle1],color,color);++stats.cloth_constraints;
            }
        }
        if(options.show_particles)for(const auto& cloud:frame.deformables.particle_clouds) {
            if(!visible_owner(cloud.key))continue;
            const auto base=cloud.color?rgba(*cloud.color):cloud.kind==deformables::ParticleKind::fluid?
                std::array<float,4>{.26f,.68f,.97f,1}:std::array<float,4>{.91f,.72f,.32f,1};
            for(const auto& particle:cloud.particles)if(particle.position) {
                const bool active=selected(cloud.key,deformables::ElementKind::particle,particle.index);
                point(*particle.position,active?std::array<float,4>{1,.82f,.23f,1}:base,options.point_size+(active?4.f:0.f));++stats.particle_points;
                if(options.show_particle_velocity&&particle.velocity) {
                    const auto color=std::array<float,4>{.38f,.87f,.72f,.9f};
                    line(lines,*particle.position,add(*particle.position,scale(*particle.velocity,options.particle_velocity_scale)),color,color);
                    ++stats.particle_velocity_vectors;
                }
            }
        }
        draw_dynamic(triangles,lines,projection);
        // Avoid retaining the viewport as an output when the host samples its SRV.
        context->OMSetRenderTargets(0,nullptr,nullptr);
        stats.cached_meshes=meshes.size();stats.cache_hits=hits;stats.cache_misses=misses;
    }
};

Renderer::Renderer(ID3D11Device* device,ID3D11DeviceContext* context,std::size_t limit):impl_(std::make_unique<Impl>(device,context,limit)){}
Renderer::~Renderer()=default;Renderer::Renderer(Renderer&&) noexcept=default;Renderer& Renderer::operator=(Renderer&&) noexcept=default;
void Renderer::resize(std::uint32_t w,std::uint32_t h){impl_->resize(w,h);}
void Renderer::render(const FrameState& frame,const OrbitCamera& camera,const RenderOptions& options){impl_->render(frame,camera,options);}
ID3D11ShaderResourceView* Renderer::shader_resource_view() const noexcept{return impl_->srv.Get();}
ID3D11Texture2D* Renderer::texture() const noexcept{return impl_->target.Get();}
std::uint32_t Renderer::width() const noexcept{return impl_->width;}
std::uint32_t Renderer::height() const noexcept{return impl_->height;}
RenderStats Renderer::stats() const noexcept{return impl_->stats;}
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
