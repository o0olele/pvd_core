#include "camera_controller.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace pvd::viewer {
namespace {
struct D3 { double x, y, z; };
D3 add(D3 a,D3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
D3 multiply(D3 a,double n){return {a.x*n,a.y*n,a.z*n};}
D3 value(Vec3 v){return {v.x,v.y,v.z};}
double length(D3 v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
Vec3 finite_position(D3 v) {
  constexpr double limit=std::numeric_limits<float>::max();
  for(const auto n:{v.x,v.y,v.z})
    if(!std::isfinite(n)||std::abs(n)>limit)
      throw std::invalid_argument("Camera movement exceeds finite world coordinates");
  return {static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z)};
}
void valid(const CameraNavigationPreferences &p) {
  if(!valid_camera_navigation(p))
    throw std::invalid_argument("Invalid camera navigation settings");
}
void validate_camera(const OrbitCamera &c) {
  finite_position(value(c.position()));
  for(const auto n:c.view_projection(1).values)
    if(!std::isfinite(n))throw std::invalid_argument("Camera matrix exceeds finite range");
}
}
bool move_camera(OrbitCamera &camera,const CameraMovement &input,
                 const CameraNavigationPreferences &settings) {
  valid(settings);
  for(const auto n:{input.forward,input.right,input.up})
    if(!std::isfinite(n)||n < -1||n > 1)
      throw std::invalid_argument("Camera movement axes must be in [-1,1]");
  if(!std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0||input.elapsed_seconds>1)
    throw std::invalid_argument("Camera frame delta must be in [0,1] seconds");
  if((input.forward==0&&input.right==0&&input.up==0)||
      input.elapsed_seconds==0||settings.movement_speed==0)return false;
  validate_camera(camera);
  const auto m=camera.view_matrix();
  const D3 right{m.values[0],m.values[4],m.values[8]},
           forward{-m.values[2],-m.values[6],-m.values[10]};
  auto up=value(camera.reference_up);
  const auto up_length=length(up);
  if(!(up_length>0)||!std::isfinite(up_length))
    throw std::invalid_argument("Camera display up is invalid");
  up=multiply(up,1/up_length);
  auto direction=add(add(multiply(forward,input.forward),multiply(right,input.right)),
                     multiply(up,input.up));
  const auto magnitude=length(direction);
  if(magnitude==0)return false;
  if(magnitude>1)direction=multiply(direction,1/magnitude);
  const auto distance=settings.movement_speed*input.elapsed_seconds*(input.boost?2:1);
  auto next=camera;
  next.target=finite_position(add(value(camera.target),multiply(direction,distance)));
  validate_camera(next);
  const bool changed=next.target.x!=camera.target.x||next.target.y!=camera.target.y||
                     next.target.z!=camera.target.z;
  camera=next;
  return changed;
}
bool rotate_camera(OrbitCamera &camera,double dx,double dy,
                   const CameraNavigationPreferences &settings) {
  valid(settings);
  if(!std::isfinite(dx)||!std::isfinite(dy))
    throw std::invalid_argument("Camera look input must be finite");
  if(dx==0&&dy==0)return false;
  validate_camera(camera);
  const auto eye=camera.position();
  auto next=camera;
  next.orbit(dx*settings.look_sensitivity,dy*settings.look_sensitivity);
  if(settings.mode==CameraNavigationMode::first_person) {
    const auto rotated_eye=next.position();
    next.target=finite_position(add(value(next.target),
        {double(eye.x)-rotated_eye.x,double(eye.y)-rotated_eye.y,double(eye.z)-rotated_eye.z}));
  }
  validate_camera(next);
  const bool changed=next.yaw!=camera.yaw||next.pitch!=camera.pitch;
  camera=next;
  return changed;
}
void point_camera_at(OrbitCamera &camera,Vec3 target) {
  auto next=camera;
  next.set_look_at({camera.position(),camera.reference_up,target});
  validate_camera(next);
  camera=next;
}
void set_camera_projection(OrbitCamera &camera,double near_plane,double far_plane,double fov) {
  auto next=camera;
  next.near_plane=near_plane;next.far_plane=far_plane;next.vertical_fov=fov;
  validate_camera(next);
  camera=next;
}
} // namespace pvd::viewer
