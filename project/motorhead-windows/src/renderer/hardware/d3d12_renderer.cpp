#include <renderer/hardware/d3d12_renderer.hpp>
#include <renderer/hardware/d3d_dynamic.hpp>
#include <core/logging/runtime_log.hpp>

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mh::render::hardware {
namespace {
static_assert(offsetof(SceneLighting, environment) % 16U == 0U);
static_assert(offsetof(SceneLighting, environment) <= 65536U);

template <typename T> class ComPtr {
public:
  ComPtr() = default;
  explicit ComPtr(T *value) : value_(value) {}
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr &) = delete;
  ComPtr &operator=(const ComPtr &) = delete;
  ComPtr(ComPtr &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ComPtr &operator=(ComPtr &&other) noexcept {
    if (this != &other) reset(std::exchange(other.value_, nullptr));
    return *this;
  }
  T *get() const noexcept { return value_; }
  T **put() noexcept { reset(); return &value_; }
  void reset(T *value = nullptr) noexcept {
    if (value_ != nullptr) value_->Release();
    value_ = value;
  }
  explicit operator bool() const noexcept { return value_ != nullptr; }
private:
  T *value_ = nullptr;
};

void require_hr(const HRESULT hr, const char *operation) {
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
                             std::to_string(static_cast<unsigned long>(hr)) +
                             ")");
  }
}

ComPtr<ID3DBlob> compile_shader(const char *source, const char *entry,
                                const char *profile) {
  ComPtr<ID3DBlob> shader;
  ComPtr<ID3DBlob> errors;
  const auto result = mh::render::hardware::compile_shader(
      source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile,
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, shader.put(), errors.put());
  if (FAILED(result)) {
    const auto message = errors
                             ? std::string(static_cast<const char *>(
                                               errors.get()->GetBufferPointer()),
                                           errors.get()->GetBufferSize())
                             : std::string("unknown shader compiler error");
    throw std::runtime_error("compile D3D12 scene shader: " + message);
  }
  return shader;
}

constexpr const char shader_source[] = R"(
struct VSIn {
  float3 paint_albedo : COLOR1;
  float4 surface_position : TEXCOORD4;
  float3 world_up : TEXCOORD5;
  float3 key_direction : TEXCOORD6;
  float3 key_color : TEXCOORD7;
  float3 environment_normal : NORMAL0;
  float4 position : POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
  float4 fog : TEXCOORD1;
  float4 fog_parameters : TEXCOORD2;
};
struct VSOut {
  float3 paint_albedo : COLOR1;
  float4 surface_position : TEXCOORD4;
  float3 world_up : TEXCOORD5;
  float3 key_direction : TEXCOORD6;
  float3 key_color : TEXCOORD7;
  float3 environment_normal : NORMAL0;
  float4 position : SV_Position;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
  float4 fog : TEXCOORD1;
  float view_depth : TEXCOORD2;
  float4 fog_parameters : TEXCOORD3;
};
VSOut vs_main(VSIn input) {
  VSOut output;
  output.paint_albedo = input.paint_albedo;
  output.surface_position = input.surface_position;
  output.world_up = input.world_up;
  output.key_direction = input.key_direction;
  output.key_color = input.key_color;
  output.environment_normal = input.environment_normal;
  output.position = input.position;
  output.color = input.color;
  output.uv = input.uv;
  output.fog = input.fog;
  output.view_depth = input.position.w;
  output.fog_parameters = input.fog_parameters;
  return output;
}
Texture2D scene_texture : register(t0);
SamplerState scene_sampler : register(s0);
float2 surface_uv(VSOut input) {
  const float length_squared = dot(input.environment_normal, input.environment_normal);
  if (input.surface_position.w > 0.5 && input.surface_position.w < 3.5 && abs(input.surface_position.w - 2.0) > 0.1 && length_squared > 0.000001) {
    // Retain the disc's normal-map projection and authored maps, but normalize
    // the interpolated normal per pixel so highlights follow curved surfaces.
    const float3 normal = input.environment_normal * rsqrt(length_squared);
    return saturate(float2(0.5 + 0.495 * normal.x, 0.5 - 0.495 * normal.y));
  }
  return input.uv;
}
cbuffer SceneLightingConstants : register(b1) {
  float4 lighting_control;
  float4 lighting_ambient;
  float4 lighting_direction;
  float4 lighting_sun;
  float4 lighting_up;
  float4 lighting_positions[32];
  float4 lighting_colors[32];
  float4 headlight_positions[32];
  float4 headlight_directions[32];
  float4 headlight_colors[32];
  float4 probe_right, probe_up, probe_forward;
  float4 probe_origins[8];
  float4 camera_position;
  float4 history_right, history_up, history_forward, history_origin;
  float4 history_projection;
  float4 history_car_origins[8];
  float4 temporal_jitter;
  float4 car_right[8], car_up[8], car_forward[8], car_shadow_centers[8];
  float4 history_car_right[8], history_car_up[8], history_car_forward[8];
};
StructuredBuffer<float4> probe_colors : register(t3);
StructuredBuffer<float> shadow_depths : register(t4);
int nearest_probe(float3 p) {
  int selected = 0; float distance2 = 1e30;
  [unroll] for (int i = 0; i < 8; ++i) {
    const float3 delta = p - probe_origins[i].xyz;
    const float d2 = dot(delta, delta);
    if (probe_origins[i].w > 0.5 && d2 < distance2) { selected = i; distance2 = d2; }
  }
  return selected;
}
float3 sample_local_probe(float3 view_direction, float3 p) {
  const int base = nearest_probe(p) * 1536;
  const float3 d = view_direction.x * probe_right.xyz + view_direction.y * probe_up.xyz + view_direction.z * probe_forward.xyz;
  const float3 a = abs(d);
  int face; float2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = d.x > 0 ? 0 : 1;
    uv = float2(d.x > 0 ? -d.z : d.z, -d.y) / max(0.0001, a.x);
  } else if (a.y >= a.z) {
    face = d.y > 0 ? 2 : 3;
    uv = float2(d.x, d.y > 0 ? d.z : -d.z) / max(0.0001, a.y);
  } else {
    face = d.z > 0 ? 4 : 5;
    uv = float2(d.z > 0 ? d.x : -d.x, -d.y) / max(0.0001, a.z);
  }
  const float2 pixel = clamp((uv * 0.5 + 0.5) * 16.0 - 0.5, 0.0, 15.0);
  const int2 lo = int2(floor(pixel)), hi = min(lo + 1, 15);
  const float2 f = frac(pixel);
  return lerp(lerp(probe_colors[base+face*256+lo.y*16+lo.x].rgb, probe_colors[base+face*256+lo.y*16+hi.x].rgb, f.x),
              lerp(probe_colors[base+face*256+hi.y*16+lo.x].rgb, probe_colors[base+face*256+hi.y*16+hi.x].rgb, f.x), f.y);
}
float3 srgb_to_linear(float3 c) {
  c = saturate(c);
  return float3(c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
                c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
                c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
}
float3 linear_to_srgb(float3 c) {
  c = max(c, 0.0);
  return float3(c.r <= 0.0031308 ? c.r * 12.92 : 1.055 * pow(c.r, 1.0/2.4) - 0.055,
                c.g <= 0.0031308 ? c.g * 12.92 : 1.055 * pow(c.g, 1.0/2.4) - 0.055,
                c.b <= 0.0031308 ? c.b * 12.92 : 1.055 * pow(c.b, 1.0/2.4) - 0.055);
}
float shadow_tap(int light_index, int face, int2 pixel, float3 delta, float3 normal) {
  const float2 uv = (float2(pixel) + 0.5) / 8.0 - 1.0;
  float3 ray;
  if (face == 0) ray = float3(1,-uv.y,-uv.x);
  else if (face == 1) ray = float3(-1,-uv.y,uv.x);
  else if (face == 2) ray = float3(uv.x,1,uv.y);
  else if (face == 3) ray = float3(uv.x,-1,-uv.y);
  else if (face == 4) ray = float3(uv.x,-uv.y,1);
  else ray = float3(-uv.x,-uv.y,-1);
  ray = normalize(ray);
  const float distance = length(delta);
  const float denominator = dot(ray, normal);
  // Compare the receiver plane at each PCF tap. Comparing every tap against
  // the centre distance falsely shadows a flat road at grazing light angles.
  const float plane_distance = abs(denominator) > 0.0001 ? dot(delta, normal) / denominator : distance;
  const float receiver = clamp(plane_distance, distance * 0.5, distance * 2.0);
  const float depth = shadow_depths[light_index*1536 + face*256 + pixel.y*16 + pixel.x];
  return smoothstep(receiver - 0.10 - distance*0.005, receiver - 0.02, depth);
}
float car_light_visibility(float3 p, float3 source) {
  const float3 ray_view = source-p;
  const float3 ray = ray_view.x*probe_right.xyz + ray_view.y*probe_up.xyz + ray_view.z*probe_forward.xyz;
  float visibility = 1.0;
  [loop] for (int car_index=0;car_index<8;++car_index) {
    if (car_shadow_centers[car_index].w<0.5) continue;
    const float3 delta = p-car_shadow_centers[car_index].xyz;
    const float3 world_delta = delta.x*probe_right.xyz + delta.y*probe_up.xyz + delta.z*probe_forward.xyz;
    const float3 radii = max(float3(car_right[car_index].w,car_up[car_index].w,car_forward[car_index].w),0.05);
    const float3 a = float3(dot(world_delta,car_right[car_index].xyz),dot(world_delta,car_up[car_index].xyz),dot(world_delta,car_forward[car_index].xyz))/radii;
    const float3 b = float3(dot(ray,car_right[car_index].xyz),dot(ray,car_up[car_index].xyz),dot(ray,car_forward[car_index].xyz))/radii;
    if (dot(a,a)<1.05 || dot(a+b,a+b)<1.05) continue;
    const float bb=dot(b,b), ab=dot(a,b), c=dot(a,a)-1.0;
    const float discriminant=ab*ab-bb*c;
    if(discriminant<=0.0 || bb<0.0001) continue;
    const float root=sqrt(discriminant);
    const float enter=(-ab-root)/bb, leave=(-ab+root)/bb;
    if(enter>0.005 && enter<0.995)
      visibility *= 1.0-saturate((leave-enter)*length(ray)/0.25);
  }
  return visibility;
}float light_visibility(float3 p, float3 n, float3 source, int light_index) {
  const float3 delta = p + n * 0.035 - source;
  const float distance = length(delta);
  const float3 d = delta.x * probe_right.xyz + delta.y * probe_up.xyz + delta.z * probe_forward.xyz;
  const float3 a = abs(d);
  int face; float2 uv;
  if (a.x >= a.y && a.x >= a.z) {
    face = d.x > 0 ? 0 : 1;
    uv = float2(d.x > 0 ? -d.z : d.z, -d.y) / max(0.0001, a.x);
  } else if (a.y >= a.z) {
    face = d.y > 0 ? 2 : 3;
    uv = float2(d.x, d.y > 0 ? d.z : -d.z) / max(0.0001, a.y);
  } else {
    face = d.z > 0 ? 4 : 5;
    uv = float2(d.z > 0 ? d.x : -d.x, -d.y) / max(0.0001, a.z);
  }
  const float2 pixel = clamp((uv * 0.5 + 0.5) * 16.0 - 0.5, 0.0, 15.0);
  const int2 lo = int2(floor(pixel)), hi = min(lo + 1, 15);
  const float2 f = frac(pixel);
  const float3 normal = n.x * probe_right.xyz + n.y * probe_up.xyz + n.z * probe_forward.xyz;
  const float4 visible = float4(shadow_tap(light_index, face, lo, d, normal),
      shadow_tap(light_index, face, int2(hi.x,lo.y), d, normal),
      shadow_tap(light_index, face, int2(lo.x,hi.y), d, normal),
      shadow_tap(light_index, face, hi, d, normal));
  return lerp(lerp(visible.x, visible.y, f.x), lerp(visible.z, visible.w, f.x), f.y) * car_light_visibility(p, source);
}void material_light(float3 n, float3 v, float3 l, float3 radiance,
                    float roughness, inout float3 diffuse, inout float3 specular, float f0) {
  const float nl = saturate(dot(n, l));
  const float nv = max(0.001, saturate(dot(n, v)));
  const float3 h = (l + v) * rsqrt(max(0.000001, dot(l + v, l + v)));
  const float nh = saturate(dot(n, h));
  const float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(v, h)), 5.0);
  const float a2 = pow(roughness, 4.0);
  const float denominator = nh * nh * (a2 - 1.0) + 1.0;
  const float distribution = a2 / max(0.000001, denominator * denominator);
  const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
  const float visibility = 1.0 / max(0.0001, (nv * (1.0 - k) + k) * (nl * (1.0 - k) + k));
  // Shared dielectric response in the renderer's legacy irradiance units.
  diffuse += radiance * nl * (1.0 - fresnel);
  specular += radiance * nl * distribution * visibility * fresnel * 0.25;
}
void lighting_terms(float3 p, float3 n, float3 v, float roughness,
                    float sun_visibility, out float3 diffuse, out float3 specular, float f0) {
  const float hemi = saturate(0.5 + 0.5 * dot(n, lighting_up.xyz));
  // Broad local sky/scenery bounce; source colours, not a fixed white fill.
  const float bounce = saturate(dot(lighting_ambient.rgb, float3(0.2126,0.7152,0.0722)) * 2.0);
  const float3 local_bounce = sample_local_probe(n, p) * 0.5 +
      sample_local_probe(normalize(n + lighting_up.xyz * 0.6), p) * 0.5;
  // Preserve authored daylight fill; replacing it with a coarse probe made
  // bright outdoor tracks too dark. Local bounce adds directional colour.
  diffuse = lighting_ambient.rgb * (0.4 + hemi * 0.9) + local_bounce * bounce * 0.45;
  const float3 l = lighting_direction.xyz;
  specular = 0.0;
  material_light(n, v, l, lighting_sun.rgb * sun_visibility, roughness, diffuse, specular, f0);
  [loop] for (int i = 0; i < (int)lighting_control.y; ++i) {
    const float3 delta = lighting_positions[i].xyz - p;
    const float d2 = dot(delta, delta);
    const float range = lighting_positions[i].w;
    if (d2 >= range * range) continue;
    const float distance = sqrt(max(0.01, d2));
    const float3 direction = delta / distance;
    if (dot(n, direction) <= 0.0) continue;
    const float falloff = saturate(1.0 - distance / range);
    const float3 radiance = lighting_colors[i].rgb * lighting_colors[i].w *
        falloff * falloff / (1.0 + d2 * 0.04) * light_visibility(p, n, lighting_positions[i].xyz, i);
    material_light(n, v, direction, radiance, roughness, diffuse, specular, f0);
  }
  // One source-space beam lights cars, wheels and world materials. Enhanced
  // mode does not also draw the legacy road projection over this result.
  [loop] for (int i = 0; i < (int)lighting_control.w; ++i) {
    const float3 delta = headlight_positions[i].xyz - p;
    const float d2 = dot(delta, delta);
    if (d2 < 0.04 || d2 > headlight_positions[i].w * headlight_positions[i].w) continue;
    const float distance = sqrt(d2);
    const float3 l = delta / distance;
    const float cone = smoothstep(0.86, 0.98, dot(-l, headlight_directions[i].xyz));
    if (cone <= 0.0001 || dot(n, l) <= 0.0) continue;
    const float falloff = saturate(1.0 - distance / headlight_positions[i].w);
    const float3 radiance = headlight_colors[i].rgb * headlight_colors[i].w *
        cone * falloff * falloff / (1.0 + 0.035 * d2) * light_visibility(p, n, headlight_positions[i].xyz, 32 + i);
    material_light(n, v, l, radiance, roughness, diffuse, specular, f0);
  }

}
float3 material_normal(VSOut input, float3 texture_color) {
  float3 n = normalize(input.environment_normal);
  if (dot(n, -input.surface_position.xyz) < 0.0) n = -n;
  if (input.surface_position.w < 3.5) return n;
  // Surface gradients recover small-scale relief directly from the disc texture.
  // Derivatives keep the detail attached to the mesh as the camera moves.
  const float height = dot(texture_color, float3(0.2126, 0.7152, 0.0722));
  const float3 dpdx = ddx(input.surface_position.xyz);
  const float3 dpdy = ddy(input.surface_position.xyz);
  const float3 r1 = cross(dpdy, n);
  const float3 r2 = cross(n, dpdx);
  const float determinant = dot(dpdx, r1);
  const float3 gradient = (ddx(height) * r1 + ddy(height) * r2) /
      (abs(determinant) > 0.000001 ? determinant : 1.0);
  const bool road = abs(input.surface_position.w - 8.0) < 0.1;
  const bool wheel = abs(input.surface_position.w - 9.0) < 0.1;
  const float strength = wheel ? 0.004 : (road ? 0.002 : (input.surface_position.w > 11.5 ? 0.0 : 0.008));
  return normalize(n - gradient * min(strength, (road ? 0.05 : 0.18) / max(0.001, length(gradient))));
}
float3 shade_car(VSOut input, float3 texture_color) {
  const bool wheel = abs(input.surface_position.w - 9.0) < 0.1;
  float3 n = wheel ? material_normal(input, texture_color) : normalize(input.environment_normal);
  const float3 v = normalize(-input.surface_position.xyz);
  if (dot(n, v) < 0.0) n = -n;
  const bool glass = abs(input.surface_position.w - 3.0) < 0.1;
  const bool paint = input.surface_position.w < 1.5;
  // Decode palette and texture independently before multiplying material inputs.
  // The same diffuse-reflectance scale is used for car and world surfaces.
  const float3 albedo = srgb_to_linear(input.paint_albedo) *
      ((paint || glass) ? 1.0 : srgb_to_linear(texture_color)) * 0.65;
  // Smooth clear lacquer has a narrow, neutral highlight. The old broad lobe
  // made entire painted panels glow under a nearby track lamp.
  const float roughness = wheel ? 1.0 : (glass ? 0.18 : (paint ? 0.23 : 0.72));
  float3 diffuse, specular;
  const float f0 = 0.04;
  lighting_terms(input.surface_position.xyz, n, v, roughness, input.key_color.x, diffuse, specular, f0);
  // Bright pixels in the original wheel art are not a reliable metal mask.
  // Preserve that art under diffuse illumination, with no added gloss or probe
  // reflections on the tyre or wheel face.
  if (wheel) return albedo * diffuse;
  // Separate the coloured pigment beneath the lacquer from its reflection.
  // Attenuate the pigment, not the neutral lamp/environment reflection; retain
  // the original palette hue instead of desaturating the finished pixel.
  const float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(n, v)), 5.0);
  const float3 reflected = sample_local_probe(reflect(-v, n), input.surface_position.xyz);
  // Both interfaces of a thin pane contribute to reflection. Keep its source
  // tint in the transmitted term rather than colouring reflected surroundings.
  const float reflection_response = glass ? (2.0 * fresnel / (1.0 + fresnel)) : fresnel;
  const float pigment_response = glass ? 0.015 : (paint ? 0.32 * (1.0 - fresnel) : 1.0);
  // A restrained coloured basecoat reflection beneath the neutral lacquer
  // preserves the paint identity in reflected sky/walls without a constant fill.
  const float3 basecoat_reflection = paint ? reflected * albedo * (0.12 * (1.0 - fresnel)) : 0.0;
  return albedo * diffuse * pigment_response + specular + basecoat_reflection +
      reflected * reflection_response * ((glass || paint) ? 1.0 : 0.08);
}
float3 shade_world(VSOut input, float3 texture_color) {
  float3 n = material_normal(input, texture_color);
  const float3 v = normalize(-input.surface_position.xyz);
  if (dot(n, v) < 0.0) n = -n;
  const float authored_brightness = max(input.color.r, max(input.color.g, input.color.b));
  const float3 tint = lerp(1.0.xxx, input.color.rgb / max(0.08, authored_brightness), 0.3);
  const bool emitter = abs(input.surface_position.w - 7.0) < 0.1;
  const bool fixture = abs(input.surface_position.w - 5.0) < 0.1 || emitter;
  const bool asphalt = abs(input.surface_position.w - 8.0) < 0.1;
  const bool vegetation = abs(input.surface_position.w - 10.0) < 0.1;
  const bool mineral = abs(input.surface_position.w - 11.0) < 0.1;
  const bool glass = abs(input.surface_position.w - 12.0) < 0.1;
  const bool metal = abs(input.surface_position.w - 13.0) < 0.1;
  const bool untextured = abs(input.surface_position.w - 6.0) < 0.1;
  const float3 albedo = (untextured ? srgb_to_linear(input.color.rgb) :
      srgb_to_linear(texture_color) * srgb_to_linear(tint)) * 0.65;
  float3 diffuse, specular;
  const float roughness = glass ? 0.22 : metal ? 0.7 : fixture ? 0.4 : vegetation ? 1.0 : asphalt ? 0.98 : mineral ? 0.95 : 0.9;
  lighting_terms(input.surface_position.xyz, n, v, roughness,
                 saturate(authored_brightness * 1.6), diffuse, specular, metal ? 0.12 : glass || fixture ? 0.04 : 0.025);
  // Dry aggregate scatters light broadly. Preserve the disc asphalt and painted
  // markings instead of placing a reflective water layer over them.
  // Porous materials break up the grazing sheen of the old shared coating.
  const float specular_weight = glass || fixture ? 1.0 : vegetation ? 0.03 : asphalt ? 0.12 : mineral ? 0.18 : 0.3;
  float3 lit = albedo * diffuse * (glass ? 0.35 : 1.0) + specular * specular_weight;
  if (glass) {
    const float fresnel = 0.04 + 0.96 * pow(1.0 - saturate(dot(n, v)), 5.0);
    lit += sample_local_probe(reflect(-v, n), input.surface_position.xyz) * fresnel;
  }
  if (emitter)
    lit += srgb_to_linear(texture_color) * 5.0;
  return lit;
}
float4 pixel_fog(VSOut input) {
  float4 fog = input.fog;
  if (input.fog_parameters.z > 0.5 &&
      input.view_depth > input.fog_parameters.x) {
    const float normalized_cue =
        saturate((input.view_depth - input.fog_parameters.x) /
                 max(0.0001,
                     input.fog_parameters.y - input.fog_parameters.x));
    const float clear_fraction = 1.0 - normalized_cue;
    const float depth_cue = 1.0 - clear_fraction * clear_fraction;
    fog.a = max(fog.a, depth_cue);
  }
  return fog;
}
struct OpaqueResult {
  float4 color : SV_Target0;
  float4 surface : SV_Target1;
};
OpaqueResult ps_opaque(VSOut input) {
  const float4 texel = scene_texture.Sample(scene_sampler, surface_uv(input));
  float4 result = texel * input.color;
  if (lighting_control.x > 0.5) result.rgb = srgb_to_linear(result.rgb);
  if (input.surface_position.w > 0.5 && dot(input.environment_normal, input.environment_normal) > 0.000001)
    result.rgb = input.surface_position.w > 3.5 && abs(input.surface_position.w - 9.0) > 0.1 ? shade_world(input, texel.rgb) : shade_car(input, texel.rgb);
  const float4 fog = pixel_fog(input);
  result.rgb = lerp(result.rgb, lighting_control.x > 0.5 ? srgb_to_linear(fog.rgb) : fog.rgb, saturate(fog.a));
  clip(result.a - 0.5);
  OpaqueResult output;
  output.color = float4(result.rgb, 1.0);
  output.surface = 0.0;
  if (input.surface_position.w > 0.5 && dot(input.environment_normal, input.environment_normal) > 0.000001)
    output.surface = float4(material_normal(input, texel.rgb), input.surface_position.w);
  return output;
}
float4 ps_alpha(VSOut input) : SV_Target {
  float4 result = scene_texture.Sample(scene_sampler, surface_uv(input)) * input.color;
  if (lighting_control.x > 0.5) result.rgb = srgb_to_linear(result.rgb);
  const float4 fog = pixel_fog(input);
  result.rgb = lerp(result.rgb, lighting_control.x > 0.5 ? srgb_to_linear(fog.rgb) : fog.rgb, saturate(fog.a));
  clip(result.a - 0.00001);
  return result;
}
float4 ps_additive(VSOut input) : SV_Target {
  float4 result = scene_texture.Sample(scene_sampler, surface_uv(input)) * input.color;
  if (lighting_control.x > 0.5) result.rgb = srgb_to_linear(result.rgb) * 1.8;
  result.rgb *= 1.0 - saturate(pixel_fog(input).a);
  clip(result.a - 0.00001);
  return result;
}
float4 ps_tron(VSOut input) : SV_Target { return input.color; }

cbuffer ProjectionConstants : register(b0) {
  float4 output_and_projection;
  float4 view_position;
  float4 view_right;
  float4 view_up;
  float4 view_forward;
  float4 world_bounds;
  float4 cue_parameters;
  float4 cue_color;
  float4 vehicle_position;
  float4 body_basis_0;
  float4 body_basis_1;
  float4 body_basis_2;
  float4 source_0_center;
  float4 source_0_direction;
  float4 source_1_center;
  float4 source_1_direction;
  float4 cell_xz[18];
  float4 cell_y_valid[9];
};
Texture2D<float> scene_depth : register(t0);
Texture2D<float> post_depth : register(t1);
Texture2D<float4> surface_buffer : register(t2);
Texture2D<float4> history_color : register(t5);
Texture2D<float> history_depth : register(t6);

int2 post_pixel(float2 pixel) {
  return clamp(int2(pixel), int2(0, 0), int2(output_and_projection.xy) - 1);
}
float3 post_position(float2 pixel, float depth) {
  const float z = output_and_projection.w / max(0.0000001, 1.0 - depth);
  const float2 xy = (pixel - temporal_jitter.xy - output_and_projection.xy * 0.5) / output_and_projection.z;
  return float3(xy.x * z, -xy.y * z, z);
}
float post_occlusion(float2 pixel) {
  const float d = post_depth.Load(int3(post_pixel(pixel), 0));
  if (d >= 0.999999) return 1.0;
  const float3 p = post_position(pixel, d);
  const float3 dx = ddx(p);
  const float3 dy = ddy(p);
  float3 n = normalize(cross(dx, dy));
  if (dot(n, p) > 0.0) n = -n;
  const float radius = clamp(0.85 * output_and_projection.z / p.z, 2.0, 36.0);
  float occlusion = 0.0;
  [unroll] for (int i = 0; i < 16; ++i) {
    const float angle = i * 2.39996323;
    const float ring = (i + 1.0) / 16.0;
    const float2 qpixel = float2(post_pixel(pixel + float2(cos(angle), sin(angle)) * radius * sqrt(ring))) + 0.5;
    const float qd = post_depth.Load(int3(post_pixel(qpixel), 0));
    const float3 delta = post_position(qpixel, qd) - p;
    const float distance_squared = dot(delta, delta);
    const float facing = max(0.0, dot(n, delta) - 0.025);
    occlusion += facing * rsqrt(max(0.0001, distance_squared)) *
                 saturate(1.0 - distance_squared / 2.25) * (qd < 0.999999 ? 1.0 : 0.0);
  }
  return saturate(1.0 - occlusion * (1.8 / 16.0));
}
float3 post_bright(float2 pixel) {
  const float3 color = scene_texture.SampleLevel(scene_sampler,
      (float2(post_pixel(pixel)) + 0.5) / output_and_projection.xy, 0).rgb;
  const float peak = max(color.r, max(color.g, color.b));
  return color * saturate((peak - 0.8) / max(0.001, peak));
}
float4 post_reflection(float2 pixel) {
  const float4 surface = surface_buffer.Load(int3(post_pixel(pixel), 0));
  if (surface.w < 0.5 || surface.w > 3.5 || abs(surface.w - 2.0) < 0.1) return 0.0;
  const float d = post_depth.Load(int3(post_pixel(pixel), 0));
  const float3 p = post_position(pixel, d);
  const float3 n = normalize(surface.xyz);
  const float3 direction = reflect(normalize(p), n);
  float3 ray = p + n * 0.08 + direction * 0.12;
  [loop] for (int i = 0; i < 48; ++i) {
    const float stride = 0.12 + i * 0.045;
    ray += direction * stride;
    if (ray.z <= output_and_projection.w) break;
    const float2 q = output_and_projection.xy * 0.5 +
        float2(ray.x, -ray.y) * (output_and_projection.z / ray.z);
    if (any(q < 1.0) || any(q >= output_and_projection.xy - 1.0)) break;
    const float qd = post_depth.Load(int3(post_pixel(q), 0));
    const float z = output_and_projection.w / max(0.0000001, 1.0 - qd);
    const float separation = ray.z - z;
    if (i > 1 && qd < 0.999999 && separation > 0.0 && separation < 0.16 + stride * 0.65) {
      const float2 edge = min(q, output_and_projection.xy - q) / (output_and_projection.xy * 0.12);
      const float fade = saturate(min(edge.x, edge.y)) * (1.0 - i / 56.0);
      const float fresnel = pow(1.0 - saturate(abs(dot(n, normalize(-p)))), 5.0);
      const bool glass = surface.w > 2.5;
      const float weight = (glass ? 0.22 + fresnel * 0.4 : 0.04 + fresnel * 0.18) * fade;
      // Rough clear coat integrates a small footprint rather than copying a
      // single scene texel. Reduce distant-car reflections before they alias.
      const float radius = glass ? 0.6 : 2.0;
      float3 reflected = 0.0;
      [unroll] for (int tap = 0; tap < 5; ++tap) {
        const float2 offset = tap == 0 ? float2(0, 0) :
            (tap == 1 ? float2(-1, 0) : (tap == 2 ? float2(1, 0) :
            (tap == 3 ? float2(0, -1) : float2(0, 1))));
        reflected += scene_texture.SampleLevel(scene_sampler,
            (q + offset * radius) / output_and_projection.xy, 0).rgb * 0.2;
      }
      return float4(reflected, weight * saturate((60.0 - p.z) / 35.0));
    }
  }
  return 0.0;
}
float3 post_light_haze(float2 pixel) {
  const float depth = post_depth.Load(int3(post_pixel(pixel), 0));
  if (depth >= 0.999999) return 0.0;
  const float3 p = post_position(pixel, depth);
  const float ray_length = min(length(p), 120.0);
  const float3 ray = normalize(p);
  float3 haze = 0.0;
  // Analytic bounded integration through light volumes; receiver depth prevents
  // haze from being drawn over geometry in front of the light volume.
  [loop] for (int i = 0; i < min(12, (int)lighting_control.y); ++i) {
    const float3 light = lighting_positions[i].xyz;
    const float along = dot(light, ray);
    const float radius = min(lighting_positions[i].w * 0.18, 6.0);
    const float perpendicular2 = max(0.0, dot(light, light) - along * along);
    if (perpendicular2 >= radius * radius) continue;
    const float extent = sqrt(radius * radius - perpendicular2);
    const float segment = max(0.0, min(ray_length, along + extent) - max(0.0, along - extent));
    haze += lighting_colors[i].rgb * lighting_colors[i].w * segment *
        exp(-perpendicular2 / max(0.1, radius * radius * 0.18)) * 0.0007;
  }
  return haze;
}
float3 display_map(float3 color) {
  color *= max(0.1, lighting_control.z);
  const float luminance = max(0.00001, dot(color, float3(0.2126,0.7152,0.0722)));
  const float mapped = saturate((luminance*(2.51*luminance+0.03))/(luminance*(2.43*luminance+0.59)+0.14));
  color *= mapped/luminance;
  const float peak = max(color.r,max(color.g,color.b));
  const float chroma = peak > 0.98 ? saturate((0.98-mapped)/max(0.00001,peak-mapped)) : 1.0;
  return saturate(linear_to_srgb(lerp(mapped.xxx,color,chroma)));
}
float3 temporal_resolve(float2 pixel, float3 current) {
  if (history_origin.w < 0.5) return current;
  const float depth = post_depth.Load(int3(post_pixel(pixel),0));
  if (depth >= 0.999999) return current;
  const float3 p = post_position(pixel, depth);
  float3 world = camera_position.xyz + p.x*probe_right.xyz + p.y*probe_up.xyz + p.z*probe_forward.xyz;
  const float kind = surface_buffer.Load(int3(post_pixel(pixel),0)).w;
  const bool car = (kind > 0.5 && kind < 3.5) || abs(kind-9.0)<0.1;
  if (car) {
    const int probe = nearest_probe(p);
    if (history_car_origins[probe].w < 0.5) return current;
    const float3 origin = probe_origins[probe].xyz;
    const float3 car_world = camera_position.xyz + origin.x*probe_right.xyz + origin.y*probe_up.xyz + origin.z*probe_forward.xyz;
    const float3 motion = history_car_origins[probe].xyz - car_world;
    if (dot(motion,motion)>25.0) return current;
    const float3 relative = world-car_world;
    const float3 local = float3(dot(relative,car_right[probe].xyz),dot(relative,car_up[probe].xyz),dot(relative,car_forward[probe].xyz));
    if (car_right[probe].w > 0.0 && history_car_right[probe].w > 0.0)
      world = history_car_origins[probe].xyz + local.x*history_car_right[probe].xyz + local.y*history_car_up[probe].xyz + local.z*history_car_forward[probe].xyz;
    else world += motion;
  }
  const float3 delta = world-history_origin.xyz;
  const float3 previous = float3(dot(delta,history_right.xyz),dot(delta,history_up.xyz),dot(delta,history_forward.xyz));
  if (previous.z < history_projection.y) return current;
  const float2 q = history_projection.zw*0.5 + float2(previous.x,-previous.y)*history_projection.x/previous.z + temporal_jitter.zw;
  if (any(q<1.0) || any(q>=history_projection.zw-1.0)) return current;
  const float old_depth = history_depth.Load(int3(int2(q),0));
  const float old_z = history_projection.y/max(0.000001,1.0-old_depth);
  if (old_depth>=0.999999 || abs(old_z-previous.z)>0.05+previous.z*0.012) return current;
  float3 low=current, high=current;
  [unroll] for(int ty=-1;ty<=1;++ty) [unroll] for(int tx=-1;tx<=1;++tx) {
    const float3 neighbor=display_map(scene_texture.Load(int3(post_pixel(pixel+float2(tx,ty)),0)).rgb);
    low=min(low,neighbor); high=max(high,neighbor);
  }
  const float3 history=history_color.SampleLevel(scene_sampler,q/history_projection.zw,0).rgb;
  // Reprojection plus depth rejection and neighbourhood clipping, rather than
  // a screen-space trail. Moving car history uses a more conservative weight.
  const float3 clipped=clamp(history,low-0.015,high+0.015);
  const float weight=(car?0.55:0.82)*(1.0-0.65*saturate(length(q-pixel)/24.0));
  return lerp(current,clipped,weight);
}
float4 ps_post(VSOut input) : SV_Target {
  const float2 pixel = input.position.xy;
  const float4 source = scene_texture.Load(int3(post_pixel(pixel), 0));
  const float ao = post_occlusion(pixel);
  float3 bloom = 0.0;
  // Dense binomial blur: sparse wide rings repeat tiny headlights as detached
  // sparkles. Consecutive texels keep the glow connected to its source.
  static const float weights[7] = {1.0/64.0, 6.0/64.0, 15.0/64.0,
      20.0/64.0, 15.0/64.0, 6.0/64.0, 1.0/64.0};
  [unroll] for (int y = -3; y <= 3; ++y)
    [unroll] for (int x = -3; x <= 3; ++x)
      bloom += post_bright(pixel + float2(x, y)) * weights[x + 3] * weights[y + 3];
  const float4 reflection = post_reflection(pixel);
  float3 color = lerp(source.rgb, reflection.rgb, reflection.a) * ao;
  color += bloom * 0.65 + post_light_haze(pixel);
  color = temporal_resolve(pixel, display_map(color));
  // Keep SDL's coverage intact: sky and HUD are composed by separate owners.
  return float4(saturate(color), source.a);
}

float3 reconstruct_world(float2 pixel, float hardware_depth,
                         out float view_depth) {
  const float near_plane = output_and_projection.w;
  view_depth = near_plane / max(1.0e-7, 1.0 - hardware_depth);
  const float view_x = (pixel.x - output_and_projection.z) /
                       cue_parameters.w * view_depth;
  const float view_y = -(pixel.y - view_position.w) /
                       cue_parameters.w * view_depth;
  return view_position.xyz + view_right.xyz * view_x + view_up.xyz * view_y +
         view_forward.xyz * view_depth;
}
float edge2(float2 a, float2 b, float2 p) {
  return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}
bool triangle_contains(float2 a, float2 b, float2 c, float2 p) {
  const float e0 = edge2(a, b, p);
  const float e1 = edge2(b, c, p);
  const float e2 = edge2(c, a, p);
  const bool negative = e0 < -0.00001 || e1 < -0.00001 || e2 < -0.00001;
  const bool positive = e0 > 0.00001 || e1 > 0.00001 || e2 > 0.00001;
  return !(negative && positive);
}
float cue_factor(float depth) {
  if (cue_parameters.z < 0.5 || depth <= cue_parameters.y) return 0.0;
  const float normalized_cue =
      saturate((depth - cue_parameters.y) /
               max(0.0001, cue_parameters.x - cue_parameters.y));
  const float clear_fraction = 1.0 - normalized_cue;
  return 1.0 - clear_fraction * clear_fraction * clear_fraction;
}
float4 ps_shadow(VSOut input) : SV_Target {
  const int2 pixel = int2(input.position.xy);
  const float hardware_depth = scene_depth.Load(int3(pixel, 0));
  if (hardware_depth >= 0.9999999) discard;
  float view_depth;
  const float3 world = reconstruct_world(input.position.xy, hardware_depth,
                                          view_depth);
  if (world.x < world_bounds.x || world.x > world_bounds.y ||
      world.z < world_bounds.z || world.z > world_bounds.w) discard;
  bool accepted = false;
  [unroll] for (int cell = 0; cell < 9; ++cell) {
    if (cell_y_valid[cell].z < 0.5 || world.y < cell_y_valid[cell].x ||
        world.y > cell_y_valid[cell].y) continue;
    const float4 pair0 = cell_xz[cell * 2];
    const float4 pair1 = cell_xz[cell * 2 + 1];
    const float2 p = world.xz;
    if (triangle_contains(pair0.xy, pair0.zw, pair1.xy, p) ||
        triangle_contains(pair0.xy, pair1.xy, pair1.zw, p)) accepted = true;
  }
  if (!accepted) discard;
  const float cue = cue_factor(view_depth);
  if (vehicle_position.w > 0.5) {
    const float edge = min(min(world.x - world_bounds.x, world_bounds.y - world.x),
                           min(world.z - world_bounds.z, world_bounds.w - world.z));
    return float4(0.025.xxx, 0.72 * smoothstep(0.0, 0.28, edge) * (1.0 - cue));
  }
  return float4(lerp((10.0 / 255.0).xxx, cue_color.rgb, cue), 1.0);
}
static const float headlight_distances[7] =
    {0.35, 2.0, 5.0, 8.0, 11.0, 15.0, 20.0};
float4 ps_headlight(VSOut input) : SV_Target {
  const int2 pixel = int2(input.position.xy);
  const float hardware_depth = scene_depth.Load(int3(pixel, 0));
  if (hardware_depth >= 0.9999999) discard;
  float view_depth;
  const float3 world = reconstruct_world(input.position.xy, hardware_depth,
                                          view_depth);
  const float3 delta = world - vehicle_position.xyz;
  const float3 local = float3(dot(delta, body_basis_0.xyz),
                              dot(delta, body_basis_1.xyz),
                              dot(delta, body_basis_2.xyz));
  const float center_z = (source_0_center.z + source_1_center.z) * 0.5;
  const float direction_z =
      (source_0_direction.z + source_1_direction.z) * 0.5;
  if (abs(direction_z) < 1.0e-7) discard;
  const float distance = (local.z - center_z) / direction_z;
  if (distance < headlight_distances[0] || distance > headlight_distances[6])
    discard;
  const float left_x = source_0_center.x + source_0_direction.x * distance;
  const float right_x = source_1_center.x + source_1_direction.x * distance;
  const float width = right_x - left_x;
  if (abs(width) < 1.0e-7) discard;
  const float authored_lateral = (local.x - left_x) / width;
  const float lateral = (authored_lateral + 0.5) * 0.5;
  if (lateral < 0.0 || lateral > 1.0) discard;
  const float edge = saturate(1.0 - abs(lateral * 2.0 - 1.0));
  const float coverage = edge * edge * (3.0 - 2.0 * edge);
  const float remaining = saturate(1.0 - distance / headlight_distances[6]);
  const float fade_in = saturate((distance - headlight_distances[0]) /
                                 (headlight_distances[1] -
                                  headlight_distances[0]));
  float intensity = fade_in * remaining * remaining * coverage * 0.46;
  intensity *= 1.0 - cue_factor(view_depth);
  if (intensity <= 0.0) discard;
  if (source_1_direction.w > 0.5)
    return float4(float3(1.0, 0.82, 0.55) * intensity * 4.0, 1.0);
  return float4((150.0 / 255.0).xxx * intensity, 1.0);
}
)";

struct Vertex {
  float position[4]{};
  float color[4]{};
  float uv[2]{};
  float fog[4]{};
  float fog_parameters[4]{};
  float environment_normal[3]{};
  float paint_albedo[3]{};
  float surface_position[4]{};
  float world_up[3]{};
  float key_direction[3]{};
  float key_color[3]{};
};

struct ProjectionConstants {
  std::array<float, 4U> output_and_projection{};
  std::array<float, 4U> view_position{};
  std::array<float, 4U> view_right{};
  std::array<float, 4U> view_up{};
  std::array<float, 4U> view_forward{};
  std::array<float, 4U> world_bounds{};
  std::array<float, 4U> cue_parameters{};
  std::array<float, 4U> cue_color{};
  std::array<float, 4U> vehicle_position{};
  std::array<float, 4U> body_basis_0{};
  std::array<float, 4U> body_basis_1{};
  std::array<float, 4U> body_basis_2{};
  std::array<float, 4U> source_0_center{};
  std::array<float, 4U> source_0_direction{};
  std::array<float, 4U> source_1_center{};
  std::array<float, 4U> source_1_direction{};
  std::array<std::array<float, 4U>, 18U> cell_xz{};
  std::array<std::array<float, 4U>, 9U> cell_y_valid{};
};

std::array<float, 4U> vector4(const mh::game::CollisionVector3 &source,
                             const float fourth = 0.0F) {
  return {static_cast<float>(source[0U]), static_cast<float>(source[1U]),
          static_cast<float>(source[2U]), fourth};
}

D3D12_HEAP_PROPERTIES heap_properties(const D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  properties.CreationNodeMask = 1U;
  properties.VisibleNodeMask = 1U;
  return properties;
}

D3D12_RESOURCE_DESC buffer_description(const UINT64 bytes) {
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = bytes;
  description.Height = 1U;
  description.DepthOrArraySize = 1U;
  description.MipLevels = 1U;
  description.Format = DXGI_FORMAT_UNKNOWN;
  description.SampleDesc.Count = 1U;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return description;
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                const D3D12_RESOURCE_STATES before,
                const D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  list->ResourceBarrier(1U, &barrier);
}

} // namespace

struct D3D12Renderer::Impl {
  // Opt-in development preview; sampled once per renderer lifetime.
  bool enhanced = [] {
    const auto *value = std::getenv("MOTORHEAD_D3D12_ENHANCED");
    return value != nullptr && std::string_view(value) == "1";
  }();
  struct TextureResource {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    UINT descriptor = 0U;
  };
  struct Batch {
    UINT first = 0U;
    UINT count = 0U;
    const std::vector<mh::content::PamRgbaImage> *texture = nullptr;
    SceneBlendMode blend = SceneBlendMode::opaque;
    bool tron = false;
    bool wire = false;
    bool depth_test = true;
    std::optional<std::size_t> projection;
  };
  struct ProjectionDraw {
    UINT first = 0U;
    D3D12_RECT scissor{};
    bool headlight = false;
    ProjectionConstants constants{};
    bool post = false;
  };

  SDL_Renderer *owner = nullptr;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0U;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  ComPtr<ID3D12Resource> color;
  ComPtr<ID3D12Resource> post_source;
  ComPtr<ID3D12Resource> history_color, history_depth;
  bool history_valid = false;
  std::size_t temporal_frame = 0U;
  SceneLighting previous_lighting{};
  std::array<float, 2U> jitter{};
  ComPtr<ID3D12Resource> hdr_scene;
  ComPtr<ID3D12Resource> lighting_buffer;
  ComPtr<ID3D12Resource> probe_buffer, shadow_buffer;
  void *mapped_probes = nullptr, *mapped_shadows = nullptr;
  void *mapped_lighting = nullptr;
  ComPtr<ID3D12Resource> surfaces;
  SDL_Texture *wrapped_color = nullptr;
  UINT color_width = 0U;
  UINT color_height = 0U;
  UINT render_scale = 1U;
  bool color_first_use = true;
  ComPtr<ID3D12Resource> depth;
  UINT depth_width = 0U;
  UINT depth_height = 0U;
  ComPtr<ID3D12Resource> vertex_buffer;
  std::uint8_t *mapped_vertices = nullptr;
  std::size_t vertex_capacity = 0U;
  ComPtr<ID3D12Resource> projection_buffer;
  std::uint8_t *mapped_projections = nullptr;
  std::size_t projection_capacity = 0U;
  UINT descriptor_size = 0U;
  UINT next_descriptor = 6U;
  std::optional<bool> sampler_trilinear;
  DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
  std::array<ComPtr<ID3D12PipelineState>, 10U> pipelines;
  TextureResource white;
  std::unordered_map<const std::vector<mh::content::PamRgbaImage> *,
                     TextureResource> textures;
  std::vector<Vertex> vertices;
  std::vector<Batch> batches;
  std::vector<ProjectionDraw> projections;
  double render_ms = 0.0;

  ~Impl() {
    wait();
    if (probe_buffer && mapped_probes) probe_buffer.get()->Unmap(0U, nullptr);
    if (shadow_buffer && mapped_shadows) shadow_buffer.get()->Unmap(0U, nullptr);
    if (lighting_buffer && mapped_lighting != nullptr) lighting_buffer.get()->Unmap(0U, nullptr);
    if (wrapped_color != nullptr) SDL_DestroyTexture(wrapped_color);
    if (vertex_buffer && mapped_vertices != nullptr) vertex_buffer.get()->Unmap(0U, nullptr);
    if (projection_buffer && mapped_projections != nullptr)
      projection_buffer.get()->Unmap(0U, nullptr);
    if (fence_event != nullptr) CloseHandle(fence_event);
  }

  void wait() {
    if (!queue || !fence || fence_value == 0U ||
        fence.get()->GetCompletedValue() >= fence_value)
      return;
    fence.get()->SetEventOnCompletion(fence_value, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
  }

  void submit() {
    require_hr(list.get()->Close(), "close D3D12 scene command list");
    ID3D12CommandList *lists[]{list.get()};
    queue.get()->ExecuteCommandLists(1U, lists);
    ++fence_value;
    require_hr(queue.get()->Signal(fence.get(), fence_value),
               "signal D3D12 scene fence");
  }

  void create_root_signature(const bool trilinear_filtering) {
    root_signature.reset();
    for (auto &pipeline : pipelines) pipeline.reset();
    target_format = DXGI_FORMAT_UNKNOWN;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1U;
    range.BaseShaderRegister = 0U;
    range.OffsetInDescriptorsFromTableStart = 0U;
    auto depth_range = range;
    depth_range.BaseShaderRegister = 1U;
    auto surface_range = range;
    surface_range.BaseShaderRegister = 2U;
    std::array<D3D12_ROOT_PARAMETER, 9U> parameters{};
    auto history_range = range;
    history_range.BaseShaderRegister = 5U;
    auto history_depth_range = range;
    history_depth_range.BaseShaderRegister = 6U;
    parameters[7U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[7U].DescriptorTable = {1U, &history_range};
    parameters[7U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[8U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[8U].DescriptorTable = {1U, &history_depth_range};
    parameters[8U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT i = 5U; i < 7U; ++i) {
      parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      parameters[i].Descriptor.ShaderRegister = i - 2U;
      parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    parameters[4U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[4U].Descriptor.ShaderRegister = 1U;
    parameters[4U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3U].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[3U].DescriptorTable.pDescriptorRanges = &surface_range;
    parameters[3U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2U].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[2U].DescriptorTable.pDescriptorRanges = &depth_range;
    parameters[2U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[0U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0U].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[0U].DescriptorTable.pDescriptorRanges = &range;
    parameters[0U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1U].Descriptor.ShaderRegister = 0U;
    parameters[1U].Descriptor.RegisterSpace = 0U;
    parameters[1U].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = trilinear_filtering
                         ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                         : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (enhanced && trilinear_filtering) {
      sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    }
    sampler.MaxAnisotropy = enhanced && trilinear_filtering ? 16U : 1U;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = 1U;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    require_hr(serialize_root_signature(&description,
                                        D3D_ROOT_SIGNATURE_VERSION_1,
                                        serialized.put(), errors.put()),
               "serialize D3D12 scene root signature");
    require_hr(device.get()->CreateRootSignature(
                   0U, serialized.get()->GetBufferPointer(),
                   serialized.get()->GetBufferSize(), IID_PPV_ARGS(root_signature.put())),
               "create D3D12 scene root signature");
    sampler_trilinear = trilinear_filtering;
  }

  void create_descriptor_heaps() {
    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = 4096U;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    require_hr(device.get()->CreateDescriptorHeap(&srv, IID_PPV_ARGS(srv_heap.put())),
               "create D3D12 scene SRV heap");
    descriptor_size = device.get()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = 3U;
    require_hr(device.get()->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(rtv_heap.put())),
               "create D3D12 scene RTV heap");
    D3D12_DESCRIPTOR_HEAP_DESC dsv{};
    dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv.NumDescriptors = 1U;
    require_hr(device.get()->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(dsv_heap.put())),
               "create D3D12 scene DSV heap");
  }

  void create_pipelines(const DXGI_FORMAT format) {
    if (target_format == format && pipelines[0]) return;
    for (auto &pipeline : pipelines) pipeline.reset();
    const auto vs = compile_shader(shader_source, "vs_main", "vs_5_0");
    const auto ps_opaque = compile_shader(shader_source, "ps_opaque", "ps_5_0");
    const auto ps_alpha = compile_shader(shader_source, "ps_alpha", "ps_5_0");
    const auto ps_additive =
        compile_shader(shader_source, "ps_additive", "ps_5_0");
    const auto ps_tron = compile_shader(shader_source, "ps_tron", "ps_5_0");
    const auto ps_shadow = compile_shader(shader_source, "ps_shadow", "ps_5_0");
    const auto ps_headlight =
        compile_shader(shader_source, "ps_headlight", "ps_5_0");
    const auto ps_post = compile_shader(shader_source, "ps_post", "ps_5_0");
    constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 11U> layout{{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 0U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 16U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 32U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 1U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 40U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 2U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 56U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 72U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"COLOR", 1U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 84U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 4U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 96U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 5U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 112U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 6U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 124U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 7U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 136U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U}}};
    auto base = D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
    base.pRootSignature = root_signature.get();
    base.VS = {vs.get()->GetBufferPointer(), vs.get()->GetBufferSize()};
    base.PS = {ps_opaque.get()->GetBufferPointer(), ps_opaque.get()->GetBufferSize()};
    base.BlendState.AlphaToCoverageEnable = FALSE;
    base.BlendState.IndependentBlendEnable = TRUE;
    auto &target = base.BlendState.RenderTarget[0];
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_ZERO;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    base.SampleMask = UINT_MAX;
    base.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    base.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    base.RasterizerState.DepthClipEnable = TRUE;
    base.DepthStencilState.DepthEnable = TRUE;
    base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    base.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    base.DepthStencilState.StencilEnable = FALSE;
    base.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    base.NumRenderTargets = enhanced ? 2U : 1U;
    base.RTVFormats[0] = format;
    base.RTVFormats[1] = enhanced ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_UNKNOWN;
    base.BlendState.RenderTarget[1] = target;
    base.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    base.SampleDesc.Count = 1U;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[0].put())),
               "create opaque D3D12 scene pipeline");
    base.BlendState.RenderTarget[1].RenderTargetWriteMask = 0U;
    base.PS = {ps_alpha.get()->GetBufferPointer(), ps_alpha.get()->GetBufferSize()};
    base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    // The native D3D12 scene is subsequently composed as an SDL texture.  Its
    // alpha therefore describes scene coverage, not merely the current draw.
    // Source-over particles must preserve the opaque destination coverage;
    // replacing it with the card texel alpha punches a rectangular hole in the
    // completed scene during that final composition.
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[1].put())),
               "create alpha D3D12 scene pipeline");
    base.DepthStencilState.DepthEnable = FALSE;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[7].put())),
               "create depth-disabled alpha D3D12 scene pipeline");
    base.DepthStencilState.DepthEnable = TRUE;
    target.DestBlend = D3D12_BLEND_ONE;
    base.PS = {ps_additive.get()->GetBufferPointer(),
               ps_additive.get()->GetBufferSize()};
    // Additive light/spark cards contribute colour only.  Retain the coverage
    // already written by the opaque scene underneath them.
    target.SrcBlendAlpha = D3D12_BLEND_ZERO;
    target.DestBlendAlpha = D3D12_BLEND_ONE;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[2].put())),
               "create additive D3D12 scene pipeline");
    base.DepthStencilState.DepthEnable = FALSE;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[8].put())),
               "create depth-disabled additive D3D12 scene pipeline");
    base.DepthStencilState.DepthEnable = TRUE;
    base.PS = {ps_tron.get()->GetBufferPointer(), ps_tron.get()->GetBufferSize()};
    target.BlendEnable = FALSE;
    base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[3].put())),
               "create Tron-fill D3D12 scene pipeline");
    base.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[4].put())),
               "create Tron-line D3D12 scene pipeline");
    base.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    base.DepthStencilState.DepthEnable = FALSE;
    base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    base.PS = {ps_shadow.get()->GetBufferPointer(),
               ps_shadow.get()->GetBufferSize()};
    base.NumRenderTargets = 1U;
    base.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    target.BlendEnable = enhanced ? TRUE : FALSE;
    target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    target.SrcBlendAlpha = D3D12_BLEND_ZERO;
    target.DestBlendAlpha = D3D12_BLEND_ONE;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[5].put())),
               "create shadow D3D12 projection pipeline");
    base.PS = {ps_headlight.get()->GetBufferPointer(),
               ps_headlight.get()->GetBufferSize()};
    target.BlendEnable = TRUE;
    target.SrcBlend = enhanced ? D3D12_BLEND_DEST_COLOR : D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ONE;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[6].put())),
               "create headlight D3D12 projection pipeline");
    base.PS = {ps_post.get()->GetBufferPointer(), ps_post.get()->GetBufferSize()};
    base.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.BlendEnable = FALSE;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[9].put())),
               "create D3D12 lighting post-process pipeline");
    target_format = format;
  }

  void ensure_depth(const UINT width, const UINT height) {
    if (depth && width == depth_width && height == depth_height) return;
    wait();
    depth.reset();
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1U;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0F;
    require_hr(device.get()->CreateCommittedResource(
                   &heap, D3D12_HEAP_FLAG_NONE, &description,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                   IID_PPV_ARGS(depth.put())),
               "create D3D12 scene depth target");
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = DXGI_FORMAT_D32_FLOAT;
    depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device.get()->CreateDepthStencilView(
        depth.get(), &depth_view,
        dsv_heap.get()->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC sampled_depth{};
    sampled_depth.Format = DXGI_FORMAT_R32_FLOAT;
    sampled_depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sampled_depth.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sampled_depth.Texture2D.MipLevels = 1U;
    device.get()->CreateShaderResourceView(depth.get(), &sampled_depth,
                                            cpu_srv(1U));
    depth_width = width;
    depth_height = height;
  }

  void ensure_color(SDL_Renderer *renderer, const UINT width,
                    const UINT height) {
    if (color && color_width == width && color_height == height) return;
    wait();
    if (wrapped_color != nullptr) {
      SDL_DestroyTexture(wrapped_color);
      wrapped_color = nullptr;
    }
    color.reset();
    post_source.reset();
    hdr_scene.reset();
    surfaces.reset();
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1U;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = description.Format;
    require_hr(device.get()->CreateCommittedResource(
                   &heap, D3D12_HEAP_FLAG_NONE, &description,
                   D3D12_RESOURCE_STATE_COPY_DEST, &clear,
                   IID_PPV_ARGS(color.put())),
               "create D3D12 scene color target");
    if (enhanced) {
      auto hdr_description = description;
      hdr_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      D3D12_CLEAR_VALUE hdr_clear{};
      hdr_clear.Format = hdr_description.Format;
      require_hr(device.get()->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &hdr_description,
                     D3D12_RESOURCE_STATE_RENDER_TARGET, &hdr_clear,
                     IID_PPV_ARGS(hdr_scene.put())), "create HDR scene target");
      auto output_rtv = rtv_heap.get()->GetCPUDescriptorHandleForHeapStart();
      output_rtv.ptr += 2U * device.get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      device.get()->CreateRenderTargetView(color.get(), nullptr, output_rtv);
      auto post_description = description;
      post_description.Format = hdr_description.Format;
      post_description.Flags = D3D12_RESOURCE_FLAG_NONE;
      require_hr(device.get()->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &post_description,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                     IID_PPV_ARGS(post_source.put())),
                 "create D3D12 post-process source");
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = post_description.Format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1U;
      device.get()->CreateShaderResourceView(post_source.get(), &srv, cpu_srv(2U));
      history_valid = false;
      auto history_desc = post_description;
      history_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      require_hr(device.get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &history_desc,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(history_color.put())), "create temporal colour history");
      srv.Format = history_desc.Format;
      device.get()->CreateShaderResourceView(history_color.get(), &srv, cpu_srv(4U));
      history_desc.Format = DXGI_FORMAT_R32_FLOAT;
      require_hr(device.get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &history_desc,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(history_depth.put())), "create temporal depth history");
      srv.Format = history_desc.Format;
      device.get()->CreateShaderResourceView(history_depth.get(), &srv, cpu_srv(5U));
      auto surface_description = description;
      surface_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      D3D12_CLEAR_VALUE surface_clear{};
      surface_clear.Format = surface_description.Format;
      require_hr(device.get()->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &surface_description,
                     D3D12_RESOURCE_STATE_RENDER_TARGET, &surface_clear,
                     IID_PPV_ARGS(surfaces.put())),
                 "create D3D12 car surface buffer");
      srv.Format = surface_description.Format;
      device.get()->CreateShaderResourceView(surfaces.get(), &srv, cpu_srv(3U));
      auto surface_rtv = rtv_heap.get()->GetCPUDescriptorHandleForHeapStart();
      surface_rtv.ptr += device.get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      device.get()->CreateRenderTargetView(surfaces.get(), nullptr, surface_rtv);
    }
    auto properties = SDL_CreateProperties();
    if (properties == 0U)
      throw std::runtime_error("create D3D12 scene texture properties");
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                          SDL_PIXELFORMAT_RGBA32);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                          width);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                          height);
    SDL_SetPointerProperty(properties,
                           SDL_PROP_TEXTURE_CREATE_D3D12_TEXTURE_POINTER,
                           color.get());
    wrapped_color = SDL_CreateTextureWithProperties(renderer, properties);
    SDL_DestroyProperties(properties);
    if (wrapped_color == nullptr)
      throw std::runtime_error(std::string("wrap D3D12 scene texture: ") +
                               SDL_GetError());
    if (!SDL_SetTextureBlendMode(wrapped_color, SDL_BLENDMODE_BLEND) ||
        !SDL_SetTextureScaleMode(wrapped_color, SDL_SCALEMODE_LINEAR))
      throw std::runtime_error(std::string("configure D3D12 scene texture: ") +
                               SDL_GetError());
    color_width = width;
    color_height = height;
    color_first_use = true;
  }

  void ensure_vertex_buffer(const std::size_t count) {
    if (count <= vertex_capacity) return;
    wait();
    if (vertex_buffer && mapped_vertices != nullptr) vertex_buffer.get()->Unmap(0U, nullptr);
    mapped_vertices = nullptr;
    vertex_buffer.reset();
    vertex_capacity = std::max<std::size_t>(count, vertex_capacity * 2U + 32768U);
    const auto heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto description = buffer_description(
        static_cast<UINT64>(vertex_capacity) * sizeof(Vertex));
    require_hr(device.get()->CreateCommittedResource(
                   &heap, D3D12_HEAP_FLAG_NONE, &description,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                   IID_PPV_ARGS(vertex_buffer.put())),
               "create D3D12 scene vertex buffer");
    D3D12_RANGE read_range{0U, 0U};
    void *mapped = nullptr;
    require_hr(vertex_buffer.get()->Map(0U, &read_range, &mapped),
               "map D3D12 scene vertex buffer");
    mapped_vertices = static_cast<std::uint8_t *>(mapped);
  }

  static constexpr std::size_t projection_stride =
      (sizeof(ProjectionConstants) + 255U) & ~std::size_t{255U};

  void ensure_projection_buffer(const std::size_t count) {
    if (count <= projection_capacity) return;
    wait();
    if (projection_buffer && mapped_projections != nullptr)
      projection_buffer.get()->Unmap(0U, nullptr);
    mapped_projections = nullptr;
    projection_buffer.reset();
    projection_capacity = std::max<std::size_t>(
        count, projection_capacity * 2U + 32U);
    const auto heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto description =
        buffer_description(
            static_cast<UINT64>(projection_capacity) * projection_stride);
    require_hr(device.get()->CreateCommittedResource(
                   &heap, D3D12_HEAP_FLAG_NONE, &description,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                   IID_PPV_ARGS(projection_buffer.put())),
               "create D3D12 projection constant buffer");
    D3D12_RANGE read_range{0U, 0U};
    void *mapped = nullptr;
    require_hr(projection_buffer.get()->Map(0U, &read_range, &mapped),
               "map D3D12 projection constant buffer");
    mapped_projections = static_cast<std::uint8_t *>(mapped);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv(const UINT index) const {
    auto handle = srv_heap.get()->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptor_size;
    return handle;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv(const UINT index) const {
    auto handle = srv_heap.get()->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * descriptor_size;
    return handle;
  }

  TextureResource create_texture(
      const std::vector<mh::content::PamRgbaImage> &levels,
      const UINT descriptor) {
    if (levels.empty()) {
      throw std::runtime_error("D3D12 scene texture has no image levels");
    }
    const auto &image = levels.front();
    TextureResource result;
    result.descriptor = descriptor;
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = image.width;
    texture_description.Height = image.height;
    texture_description.DepthOrArraySize = 1U;
    texture_description.MipLevels = static_cast<UINT16>(levels.size());
    texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_description.SampleDesc.Count = 1U;
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    require_hr(device.get()->CreateCommittedResource(
                   &default_heap, D3D12_HEAP_FLAG_NONE, &texture_description,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                   IID_PPV_ARGS(result.texture.put())),
               "create D3D12 scene texture");
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels.size());
    std::vector<UINT> rows(levels.size());
    std::vector<UINT64> row_bytes(levels.size());
    UINT64 upload_bytes = 0U;
    device.get()->GetCopyableFootprints(
        &texture_description, 0U, static_cast<UINT>(levels.size()), 0U,
        footprints.data(), rows.data(), row_bytes.data(), &upload_bytes);
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto upload_description = buffer_description(upload_bytes);
    require_hr(device.get()->CreateCommittedResource(
                   &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_description,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                   IID_PPV_ARGS(result.upload.put())),
               "create D3D12 texture upload buffer");
    std::uint8_t *mapped = nullptr;
    D3D12_RANGE read_range{0U, 0U};
    require_hr(result.upload.get()->Map(0U, &read_range,
                                        reinterpret_cast<void **>(&mapped)),
               "map D3D12 texture upload buffer");
    for (std::size_t level = 0U; level < levels.size(); ++level) {
      const auto &level_image = levels[level];
      if (level_image.width == 0U || level_image.height == 0U ||
          level_image.rgba.size() !=
              static_cast<std::size_t>(level_image.width) *
                  level_image.height * 4U) {
        throw std::runtime_error("D3D12 scene texture level is invalid");
      }
      for (UINT y = 0U; y < rows[level]; ++y) {
        std::memcpy(
            mapped + footprints[level].Offset +
                static_cast<std::size_t>(y) *
                    footprints[level].Footprint.RowPitch,
            level_image.rgba.data() +
                static_cast<std::size_t>(y) * level_image.width * 4U,
            static_cast<std::size_t>(level_image.width) * 4U);
      }
    }
    result.upload.get()->Unmap(0U, nullptr);
    for (std::size_t level = 0U; level < levels.size(); ++level) {
      D3D12_TEXTURE_COPY_LOCATION destination{};
      destination.pResource = result.texture.get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      destination.SubresourceIndex = static_cast<UINT>(level);
      D3D12_TEXTURE_COPY_LOCATION source{};
      source.pResource = result.upload.get();
      source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      source.PlacedFootprint = footprints[level];
      list.get()->CopyTextureRegion(&destination, 0U, 0U, 0U, &source,
                                    nullptr);
    }
    transition(list.get(), result.texture.get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = static_cast<UINT>(levels.size());
    device.get()->CreateShaderResourceView(result.texture.get(), &view,
                                            cpu_srv(descriptor));
    return result;
  }

  TextureResource &texture_for(
      const std::vector<mh::content::PamRgbaImage> *levels) {
    if (levels == nullptr || levels->empty()) return white;
    if (const auto found = textures.find(levels); found != textures.end())
      return found->second;
    if (next_descriptor >= 4096U)
      throw std::runtime_error("D3D12 scene texture descriptor heap exhausted");
    auto texture = create_texture(*levels, next_descriptor++);
    return textures.emplace(levels, std::move(texture)).first->second;
  }

  Vertex make_vertex(const float x, const float y, const float depth,
                     const SDL_FColor color, const float u, const float v,
                     const SceneFrameView &frame,
                     const SDL_FColor vertex_fog = {}) const {
    const auto z = std::max<double>(depth, frame.near_plane);
    const auto normalized_x = x * 2.0 / frame.width - 1.0;
    const auto normalized_y = 1.0 - y * 2.0 / frame.height;
    Vertex result{};
    result.position[0] = static_cast<float>(normalized_x * z);
    result.position[1] = static_cast<float>(normalized_y * z);
    result.position[2] = static_cast<float>(z - frame.near_plane);
    result.position[3] = static_cast<float>(z);
    result.color[0] = color.r;
    result.color[1] = color.g;
    result.color[2] = color.b;
    result.color[3] = color.a;
    result.uv[0] = u;
    result.uv[1] = v;
    result.fog[0] = frame.fog_enabled ? frame.fog_color[0U] / 255.0F
                                      : vertex_fog.r;
    result.fog[1] = frame.fog_enabled ? frame.fog_color[1U] / 255.0F
                                      : vertex_fog.g;
    result.fog[2] = frame.fog_enabled ? frame.fog_color[2U] / 255.0F
                                      : vertex_fog.b;
    result.fog[3] = vertex_fog.a;
    result.fog_parameters[0] = static_cast<float>(frame.fog_start);
    result.fog_parameters[1] = static_cast<float>(frame.fog_end);
    result.fog_parameters[2] = frame.fog_enabled ? 1.0F : 0.0F;
    return result;
  }

  void append_quad(const std::array<SDL_FPoint, 4U> &points,
                   const std::array<float, 4U> &depths,
                   const SDL_FColor color, const SceneBlendMode blend,
                   const SceneFrameView &frame) {
    constexpr std::array<std::size_t, 6U> order{0U, 1U, 2U, 0U, 2U, 3U};
    Batch batch{static_cast<UINT>(vertices.size()), 6U, nullptr, blend, false,
                false, true, std::nullopt};
    for (const auto index : order) {
      vertices.push_back(make_vertex(points[index].x, points[index].y,
                                     depths[index], color, 0.0F, 0.0F, frame));
    }
    batches.push_back(batch);
  }

  ProjectionConstants projection_base(
      const PerspectiveView &view, const double render_distance,
      const double cue_start, const bool cue_enabled,
      const std::array<float, 3U> &color, const SceneFrameView &frame) const {
    ProjectionConstants constants;
    constants.output_and_projection = {
        static_cast<float>(frame.width * render_scale), static_cast<float>(frame.height * render_scale),
        view.center_x * render_scale, static_cast<float>(view.near_plane)};
    constants.view_position = vector4(view.position, view.center_y * render_scale);
    constants.view_right = vector4(view.right);
    constants.view_up = vector4(view.up);
    constants.view_forward = vector4(view.forward);
    constants.cue_parameters = {static_cast<float>(render_distance),
                                static_cast<float>(cue_start),
                                cue_enabled ? 1.0F : 0.0F,
                                view.focal_length * render_scale};
    constants.cue_color = {color[0U], color[1U], color[2U], 1.0F};
    return constants;
  }

  void append_projection(const int minimum_x, const int maximum_x,
                         const int minimum_y, const int maximum_y,
                         const bool headlight,
                         const ProjectionConstants &constants,
                         const SceneFrameView &frame) {
    const auto first = static_cast<UINT>(vertices.size());
    const auto left = static_cast<float>(minimum_x);
    const auto right = static_cast<float>(maximum_x + 1);
    const auto top = static_cast<float>(minimum_y);
    const auto bottom = static_cast<float>(maximum_y + 1);
    constexpr SDL_FColor white{1.0F, 1.0F, 1.0F, 1.0F};
    for (const auto &[x, y] :
         std::array<std::array<float, 2U>, 6U>{{
             {left, top}, {right, top}, {right, bottom},
             {left, top}, {right, bottom}, {left, bottom}}}) {
      vertices.push_back(make_vertex(x, y, static_cast<float>(frame.near_plane),
                                     white, 0.0F, 0.0F, frame));
    }
    projections.push_back(
        {first,
         {minimum_x * static_cast<LONG>(render_scale), minimum_y * static_cast<LONG>(render_scale),
          (maximum_x + 1) * static_cast<LONG>(render_scale), (maximum_y + 1) * static_cast<LONG>(render_scale)},
         headlight, constants});
  }

  void append_shadow(const VehicleShadowDepthProjection &projection,
                     const SceneFrameView &frame) {
    auto constants = projection_base(
        projection.view, projection.render_distance, projection.cue_start,
        projection.cue_enabled,
        {projection.cue_color[0U] / 255.0F,
         projection.cue_color[1U] / 255.0F,
         projection.cue_color[2U] / 255.0F},
        frame);
    constants.vehicle_position[3U] = enhanced ? 1.0F : 0.0F;
    constants.world_bounds = {static_cast<float>(projection.minimum_world_x),
                              static_cast<float>(projection.maximum_world_x),
                              static_cast<float>(projection.minimum_world_z),
                              static_cast<float>(projection.maximum_world_z)};
    for (std::size_t cell = 0U; cell < projection.cells.size(); ++cell) {
      const auto &source = projection.cells[cell];
      constants.cell_xz[cell * 2U] = {
          static_cast<float>(source.world_xz[0U][0U]),
          static_cast<float>(source.world_xz[0U][1U]),
          static_cast<float>(source.world_xz[1U][0U]),
          static_cast<float>(source.world_xz[1U][1U])};
      constants.cell_xz[cell * 2U + 1U] = {
          static_cast<float>(source.world_xz[2U][0U]),
          static_cast<float>(source.world_xz[2U][1U]),
          static_cast<float>(source.world_xz[3U][0U]),
          static_cast<float>(source.world_xz[3U][1U])};
      constants.cell_y_valid[cell] = {
          static_cast<float>(source.minimum_receiver_y),
          static_cast<float>(source.maximum_receiver_y),
          source.valid ? 1.0F : 0.0F, 0.0F};
    }
    append_projection(projection.minimum_x, projection.maximum_x,
                      projection.minimum_y, projection.maximum_y, false,
                      constants, frame);
  }

  void append_headlight(const HeadlightDepthProjection &projection,
                        const SceneFrameView &frame) {
    auto constants = projection_base(
        projection.view, projection.render_distance, projection.cue_start,
        projection.cue_enabled, {0.0F, 0.0F, 0.0F}, frame);
    constants.vehicle_position = vector4(projection.vehicle.world_position);
    constants.body_basis_0 = vector4(projection.vehicle.body_basis[0U]);
    constants.body_basis_1 = vector4(projection.vehicle.body_basis[1U]);
    constants.body_basis_2 = vector4(projection.vehicle.body_basis[2U]);
    constants.source_0_center = vector4(projection.sources[0U].center);
    constants.source_0_direction = vector4(projection.sources[0U].direction);
    constants.source_1_center = vector4(projection.sources[1U].center);
    constants.source_1_direction = vector4(projection.sources[1U].direction);
    constants.source_1_direction[3U] = enhanced ? 1.0F : 0.0F;
    append_projection(projection.minimum_x, projection.maximum_x,
                      projection.minimum_y, projection.maximum_y, true,
                      constants, frame);
  }

  void build_frame(const SceneFrameView &frame) {
    vertices.clear();
    batches.clear();
    projections.clear();
    constexpr std::array<std::size_t, 6U> quad{0U, 1U, 2U, 0U, 2U, 3U};
    for (const auto &command : *frame.commands) {
      if (command.shadow_projection_index.has_value()) {
        append_shadow(frame.shadow_projections->at(
                          *command.shadow_projection_index),
                      frame);
        Batch marker;
        marker.projection = projections.size() - 1U;
        batches.push_back(marker);
        continue;
      }
      if (command.vertex_count < 3U || command.vertex_count > 4U) continue;
      const auto count = command.vertex_count == 4U ? 6U : 3U;
      Batch batch{static_cast<UINT>(vertices.size()), static_cast<UINT>(count),
                  command.texture_levels, command.blend_mode,
                  frame.tron_hidden_line, false, command.depth_test,
                  std::nullopt};
      for (std::size_t i = 0; i < count; ++i) {
        const auto index = command.vertex_count == 4U ? quad[i] : i;
        const auto &source = command.vertices[index];
        vertices.push_back(make_vertex(
            source.position.x + jitter[0U] / render_scale, source.position.y + jitter[1U] / render_scale,
            static_cast<float>(command.view_depths[index]),
            frame.tron_hidden_line ? SDL_FColor{0.0F, 0.0F, 0.5F, 1.0F}
                                   : command.vertices[index].color,
            source.tex_coord.x, source.tex_coord.y, frame));
        if (enhanced) {
          std::copy(command.environment_normals[index].begin(),
                    command.environment_normals[index].end(),
                    vertices.back().environment_normal);
          auto &target = vertices.back();
          const auto &surface = command.car_surface;
          std::copy(surface.albedo.begin(), surface.albedo.end(), target.paint_albedo);
          std::copy(surface.view_positions[index].begin(), surface.view_positions[index].end(), target.surface_position);
          target.surface_position[3] = surface.kind;
          std::copy(surface.world_up.begin(), surface.world_up.end(), target.world_up);
          std::copy(surface.light_direction.begin(), surface.light_direction.end(), target.key_direction);
          std::copy(surface.light_color.begin(), surface.light_color.end(), target.key_color);
        }
      }
      const auto can_merge =
          !frame.tron_hidden_line && !batches.empty() &&
          !batches.back().projection.has_value() && !batches.back().wire &&
          batches.back().first + batches.back().count == batch.first &&
          batches.back().texture == batch.texture &&
          batches.back().blend == batch.blend &&
          batches.back().tron == batch.tron &&
          batches.back().depth_test == batch.depth_test;
      if (can_merge) {
        batches.back().count += batch.count;
      } else {
        batches.push_back(batch);
      }
      if (frame.tron_hidden_line) {
        auto wire = batch;
        wire.wire = true;
        batches.push_back(wire);
      }
    }
    for (const auto &projection : *frame.headlight_projections)
      append_headlight(projection, frame);
    if (enhanced && !frame.tron_hidden_line) {
      ProjectionConstants constants{};
      constants.output_and_projection = {
          static_cast<float>(frame.width * render_scale), static_cast<float>(frame.height * render_scale),
          (frame.focal_length > 0.0F ? frame.focal_length : static_cast<float>(frame.height)) * render_scale,
          static_cast<float>(frame.near_plane)};
      append_projection(0, frame.width - 1, 0, frame.height - 1, false, constants, frame);
      projections.back().post = true;
    }
  }
};

D3D12Renderer::D3D12Renderer() : impl_(std::make_unique<Impl>()) {}
D3D12Renderer::~D3D12Renderer() = default;

bool D3D12Renderer::initialize(SDL_Renderer *renderer) {
  impl_->owner = renderer;
  if (renderer == nullptr || std::string_view(SDL_GetRendererName(renderer)) !=
                                 "direct3d12")
    return false;
  auto properties = SDL_GetRendererProperties(renderer);
  auto *device = static_cast<ID3D12Device *>(SDL_GetPointerProperty(
      properties, SDL_PROP_RENDERER_D3D12_DEVICE_POINTER, nullptr));
  auto *queue = static_cast<ID3D12CommandQueue *>(SDL_GetPointerProperty(
      properties, SDL_PROP_RENDERER_D3D12_COMMAND_QUEUE_POINTER, nullptr));
  if (device == nullptr || queue == nullptr) return false;
  device->AddRef();
  queue->AddRef();
  impl_->device.reset(device);
  impl_->queue.reset(queue);
  require_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(impl_->allocator.put())),
             "create D3D12 scene command allocator");
  require_hr(device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        impl_->allocator.get(), nullptr,
                                        IID_PPV_ARGS(impl_->list.put())),
             "create D3D12 scene command list");
  require_hr(impl_->list.get()->Close(), "close initial D3D12 command list");
  require_hr(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(impl_->fence.put())),
             "create D3D12 scene fence");
  impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (impl_->fence_event == nullptr)
    throw std::runtime_error("create D3D12 scene fence event");
  impl_->create_descriptor_heaps();
  if (impl_->enhanced) {
    mh::common::log_runtime_info("D3D12 enhancement preview enabled: HDR, authored track lights, lamp materials, curved cars, paint/glass lighting, scene reflections, soft shadows, AO, bloom, supersampling up to 1080p");
  }
  return true;
}

bool D3D12Renderer::available() const noexcept {
  return bool(impl_->device) && bool(impl_->queue);
}

SDL_Texture *D3D12Renderer::render(SDL_Renderer *renderer,
                                   const SceneFrameView &frame) {
  if (!available() || renderer != impl_->owner || frame.commands == nullptr ||
      frame.shadow_projections == nullptr ||
      frame.headlight_projections == nullptr)
    return nullptr;
  if (!SDL_FlushRenderer(renderer)) return nullptr;
  const auto start = std::chrono::steady_clock::now();
  impl_->wait();
  require_hr(impl_->allocator.get()->Reset(), "reset D3D12 scene allocator");
  require_hr(impl_->list.get()->Reset(impl_->allocator.get(), nullptr),
             "reset D3D12 scene command list");
  if (!impl_->sampler_trilinear.has_value() ||
      *impl_->sampler_trilinear != frame.trilinear_filtering) {
    impl_->create_root_signature(frame.trilinear_filtering);
  }
  // Temporal AA already accumulates subpixel coverage. Stacking 2x-per-axis
  // supersampling on it quadrupled every lighting/shadow/post-process pass at
  // 1080p. Render at output resolution and let the temporal resolve handle AA.
  impl_->render_scale = 1U;
  const auto target_width = static_cast<UINT>(frame.width) * impl_->render_scale;
  const auto target_height = static_cast<UINT>(frame.height) * impl_->render_scale;
  impl_->ensure_color(renderer, target_width, target_height);
  const bool scene_hdr = impl_->enhanced && !frame.tron_hidden_line;
  auto *scene_color = scene_hdr ? impl_->hdr_scene.get() : impl_->color.get();
  impl_->create_pipelines(scene_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
  impl_->ensure_depth(target_width, target_height);
  transition(impl_->list.get(), impl_->color.get(),
             impl_->color_first_use ? D3D12_RESOURCE_STATE_COPY_DEST
                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto rtv = impl_->rtv_heap.get()->GetCPUDescriptorHandleForHeapStart();
  impl_->device.get()->CreateRenderTargetView(scene_color, nullptr, rtv);
  auto dsv = impl_->dsv_heap.get()->GetCPUDescriptorHandleForHeapStart();
  impl_->list.get()->OMSetRenderTargets(impl_->enhanced ? 2U : 1U, &rtv, TRUE, &dsv);
  constexpr std::array<float, 4U> transparent{0.0F, 0.0F, 0.0F, 0.0F};
  impl_->list.get()->ClearRenderTargetView(rtv, transparent.data(), 0U, nullptr);
  if (impl_->enhanced) {
    auto surface_rtv = rtv;
    surface_rtv.ptr += impl_->device.get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    impl_->list.get()->ClearRenderTargetView(surface_rtv, transparent.data(), 0U, nullptr);
  }
  impl_->list.get()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F,
                                            0U, 0U, nullptr);
  D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(target_width),
                          static_cast<float>(target_height), 0.0F, 1.0F};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(target_width), static_cast<LONG>(target_height)};
  impl_->list.get()->RSSetViewports(1U, &viewport);
  impl_->list.get()->RSSetScissorRects(1U, &scissor);
  impl_->list.get()->SetGraphicsRootSignature(impl_->root_signature.get());
  if (!impl_->lighting_buffer) {
    const auto heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto description = buffer_description((offsetof(SceneLighting, environment) + 255U) & ~std::size_t{255U});
    require_hr(impl_->device.get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(impl_->lighting_buffer.put())), "create scene lighting buffer");
    const auto create_environment_buffer = [&](auto &resource, void **mapped, std::size_t bytes) {
      const auto desc = buffer_description(bytes);
      require_hr(impl_->device.get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(resource.put())), "create environment upload buffer");
      const D3D12_RANGE range{0U, 0U};
      require_hr(resource.get()->Map(0U, &range, mapped), "map environment upload buffer");
    };
    create_environment_buffer(impl_->probe_buffer, &impl_->mapped_probes, sizeof(SceneEnvironmentData::probe_colors));
    create_environment_buffer(impl_->shadow_buffer, &impl_->mapped_shadows, sizeof(SceneEnvironmentData::shadow_depths));
    const D3D12_RANGE empty{0U, 0U};
    require_hr(impl_->lighting_buffer.get()->Map(0U, &empty, &impl_->mapped_lighting), "map scene lighting buffer");
  }
  auto lights = frame.lighting != nullptr ? *frame.lighting : SceneLighting{};
  lights.control[0U] = scene_hdr ? 1.0F : 0.0F;
  static constexpr std::array<std::array<float, 2U>, 8U> jitter_sequence{{
      {0.0F,-0.1666667F},{-0.25F,0.1666667F},{0.25F,-0.3888889F},{-0.375F,-0.0555556F},
      {0.125F,0.2777778F},{-0.125F,-0.2777778F},{0.375F,0.0555556F},{-0.4375F,0.3888889F}}};
  impl_->jitter = scene_hdr ? jitter_sequence[impl_->temporal_frame % jitter_sequence.size()] : std::array<float,2U>{};
  const auto &previous = impl_->previous_lighting;
  float movement2 = 0.0F, facing = 0.0F;
  for (std::size_t c = 0U; c < 3U; ++c) {
    const float delta = lights.camera_position[c] - previous.camera_position[c];
    movement2 += delta * delta;
    facing += lights.probe_forward[c] * previous.probe_forward[c];
  }
  lights.history_right = previous.probe_right;
  lights.history_up = previous.probe_up;
  lights.history_forward = previous.probe_forward;
  lights.history_origin = previous.camera_position;
  lights.history_origin[3U] = scene_hdr && impl_->history_valid && movement2 < 100.0F && facing > 0.8F ? 1.0F : 0.0F;
  lights.history_projection = previous.history_projection;
  lights.history_car_right = previous.car_right;
  lights.history_car_up = previous.car_up;
  lights.history_car_forward = previous.car_forward;
  lights.temporal_jitter = {impl_->jitter[0U], impl_->jitter[1U], previous.temporal_jitter[0U], previous.temporal_jitter[1U]};
  for (std::size_t i = 0U; i < 8U; ++i) {
    const auto &p = previous.probe_origins[i];
    for (std::size_t c = 0U; c < 3U; ++c)
      lights.history_car_origins[i][c] = previous.camera_position[c] + p[0U]*previous.probe_right[c] + p[1U]*previous.probe_up[c] + p[2U]*previous.probe_forward[c];
    lights.history_car_origins[i][3U] = p[3U];
  }
  std::memcpy(impl_->mapped_lighting, &lights, offsetof(SceneLighting, environment));
  if (lights.environment) {
    std::memcpy(impl_->mapped_probes, lights.environment->probe_colors.data(), sizeof(SceneEnvironmentData::probe_colors));
    std::memcpy(impl_->mapped_shadows, lights.environment->shadow_depths.data(), sizeof(SceneEnvironmentData::shadow_depths));
  }
  impl_->list.get()->SetGraphicsRootShaderResourceView(5U, impl_->probe_buffer.get()->GetGPUVirtualAddress());
  impl_->list.get()->SetGraphicsRootShaderResourceView(6U, impl_->shadow_buffer.get()->GetGPUVirtualAddress());
  impl_->list.get()->SetGraphicsRootConstantBufferView(4U, impl_->lighting_buffer.get()->GetGPUVirtualAddress());
  ID3D12DescriptorHeap *heaps[]{impl_->srv_heap.get()};
  impl_->list.get()->SetDescriptorHeaps(1U, heaps);
  if (!impl_->white.texture) {
    mh::content::PamRgbaImage white_image;
    white_image.width = 1U;
    white_image.height = 1U;
    white_image.rgba = {255U, 255U, 255U, 255U};
    impl_->white = impl_->create_texture({white_image}, 0U);
  }
  impl_->build_frame(frame);
  impl_->ensure_vertex_buffer(impl_->vertices.size());
  impl_->ensure_projection_buffer(impl_->projections.size());
  std::memcpy(impl_->mapped_vertices, impl_->vertices.data(),
              impl_->vertices.size() * sizeof(Vertex));
  for (std::size_t index = 0U; index < impl_->projections.size(); ++index) {
    std::memcpy(impl_->mapped_projections + index * Impl::projection_stride,
                &impl_->projections[index].constants,
                sizeof(impl_->projections[index].constants));
  }
  D3D12_VERTEX_BUFFER_VIEW vertex_view{
      impl_->vertex_buffer.get()->GetGPUVirtualAddress(),
      static_cast<UINT>(impl_->vertices.size() * sizeof(Vertex)),
      sizeof(Vertex)};
  impl_->list.get()->IASetVertexBuffers(0U, 1U, &vertex_view);
  impl_->list.get()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const auto projection_base_address =
      impl_->projections.empty()
          ? D3D12_GPU_VIRTUAL_ADDRESS{0U}
          : impl_->projection_buffer.get()->GetGPUVirtualAddress();
  const auto draw_projection = [&](const std::size_t index) {
    const auto &projection = impl_->projections.at(index);
    transition(impl_->list.get(), impl_->depth.get(),
               D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    impl_->list.get()->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
    impl_->list.get()->SetGraphicsRootDescriptorTable(0U, impl_->gpu_srv(1U));
    impl_->list.get()->RSSetScissorRects(1U, &projection.scissor);
    impl_->list.get()->SetPipelineState(
        impl_->pipelines[projection.headlight ? 6U : 5U].get());
    impl_->list.get()->SetGraphicsRootConstantBufferView(
        1U, projection_base_address +
                static_cast<UINT64>(index) * Impl::projection_stride);
    impl_->list.get()->DrawInstanced(6U, 1U, projection.first, 0U);
    transition(impl_->list.get(), impl_->depth.get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    impl_->list.get()->OMSetRenderTargets(impl_->enhanced ? 2U : 1U, &rtv, TRUE, &dsv);
    impl_->list.get()->RSSetScissorRects(1U, &scissor);
  };
  for (const auto &batch : impl_->batches) {
    if (batch.projection.has_value()) {
      if (!frame.tron_hidden_line) draw_projection(*batch.projection);
      continue;
    }
    auto &texture = impl_->texture_for(batch.texture);
    const auto pipeline =
        batch.wire ? 4U
        : batch.tron ? 3U
        : batch.blend == SceneBlendMode::opaque ? 0U
        : batch.blend == SceneBlendMode::alpha
            ? (batch.depth_test ? 1U : 7U)
            : (batch.depth_test ? 2U : 8U);
    impl_->list.get()->SetPipelineState(impl_->pipelines[pipeline].get());
    impl_->list.get()->SetGraphicsRootDescriptorTable(
        0U, impl_->gpu_srv(texture.descriptor));
    impl_->list.get()->DrawInstanced(batch.count, 1U, batch.first, 0U);
  }
  if (!frame.tron_hidden_line) {
    for (std::size_t index = 0U; index < impl_->projections.size(); ++index) {
      if (impl_->projections[index].headlight && !impl_->enhanced) draw_projection(index);
    }
  }
  if (impl_->enhanced && !frame.tron_hidden_line) {
    const auto index = impl_->projections.size() - 1U;
    const auto &post = impl_->projections[index];
    transition(impl_->list.get(), scene_color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(impl_->list.get(), impl_->post_source.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    impl_->list.get()->CopyResource(impl_->post_source.get(), scene_color);
    transition(impl_->list.get(), impl_->post_source.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(impl_->list.get(), scene_color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(impl_->list.get(), impl_->depth.get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto output_rtv = rtv;
    output_rtv.ptr += 2U * impl_->device.get()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    impl_->list.get()->OMSetRenderTargets(1U, &output_rtv, FALSE, nullptr);
    impl_->list.get()->SetGraphicsRootDescriptorTable(0U, impl_->gpu_srv(2U));
    impl_->list.get()->SetGraphicsRootDescriptorTable(2U, impl_->gpu_srv(1U));
    transition(impl_->list.get(), impl_->surfaces.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    impl_->list.get()->SetGraphicsRootDescriptorTable(3U, impl_->gpu_srv(3U));
    impl_->list.get()->SetGraphicsRootDescriptorTable(7U, impl_->gpu_srv(4U));
    impl_->list.get()->SetGraphicsRootDescriptorTable(8U, impl_->gpu_srv(5U));
    impl_->list.get()->SetGraphicsRootConstantBufferView(1U, projection_base_address + static_cast<UINT64>(index) * Impl::projection_stride);
    impl_->list.get()->SetPipelineState(impl_->pipelines[9].get());
    impl_->list.get()->DrawInstanced(6U, 1U, post.first, 0U);
    transition(impl_->list.get(), impl_->color.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(impl_->list.get(), impl_->history_color.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    impl_->list.get()->CopyResource(impl_->history_color.get(), impl_->color.get());
    transition(impl_->list.get(), impl_->history_color.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(impl_->list.get(), impl_->color.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(impl_->list.get(), impl_->depth.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(impl_->list.get(), impl_->history_depth.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    impl_->list.get()->CopyResource(impl_->history_depth.get(), impl_->depth.get());
    transition(impl_->list.get(), impl_->history_depth.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(impl_->list.get(), impl_->depth.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(impl_->list.get(), impl_->surfaces.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(impl_->list.get(), impl_->depth.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  transition(impl_->list.get(), impl_->color.get(),
             D3D12_RESOURCE_STATE_RENDER_TARGET,
             impl_->color_first_use ? D3D12_RESOURCE_STATE_COPY_DEST
                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  impl_->submit();
  impl_->color_first_use = false;
  impl_->history_valid = scene_hdr;
  impl_->previous_lighting = lights;
  impl_->previous_lighting.environment.reset();
  impl_->previous_lighting.history_projection = {
      (frame.focal_length > 0.0F ? frame.focal_length : static_cast<float>(frame.height)) * impl_->render_scale,
      static_cast<float>(frame.near_plane), static_cast<float>(target_width), static_cast<float>(target_height)};
  ++impl_->temporal_frame;
  impl_->render_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  return impl_->wrapped_color;
}

double D3D12Renderer::last_render_ms() const noexcept {
  return impl_->render_ms;
}

} // namespace mh::render::hardware
