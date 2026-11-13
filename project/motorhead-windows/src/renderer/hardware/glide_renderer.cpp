#include <renderer/hardware/glide_renderer.hpp>
#include <renderer/hardware/d3d_dynamic.hpp>

#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mh::render::hardware {
namespace {

template <typename T> class ComPointer {
public:
  ComPointer() = default;
  explicit ComPointer(T *value) : value_(value) {}
  ~ComPointer() { reset(); }
  ComPointer(const ComPointer &) = delete;
  ComPointer &operator=(const ComPointer &) = delete;
  ComPointer(ComPointer &&other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  ComPointer &operator=(ComPointer &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] T *get() const noexcept { return value_; }
  [[nodiscard]] T **put() noexcept {
    reset();
    return &value_;
  }
  void reset(T *value = nullptr) noexcept {
    if (value_ != nullptr) {
      value_->Release();
    }
    value_ = value;
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }

private:
  T *value_ = nullptr;
};

void require_hresult(const HRESULT result, const char *operation) {
  if (FAILED(result)) {
    throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
                             std::to_string(static_cast<unsigned long>(result)) +
                             ")");
  }
}

constexpr const char scene_shader_source[] = R"(
cbuffer SceneConstants : register(b0) {
  float output_width;
  float output_height;
  float near_plane;
  float fog_enabled;
  float4 distance_fog_color;
  float4 distance_fog_range;
};

struct VertexInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float2 texture_coordinate : TEXCOORD0;
  float4 fog : TEXCOORD1;
};

struct VertexOutput {
  float4 position : SV_Position;
  float4 color : COLOR0;
  float2 texture_coordinate : TEXCOORD0;
  float4 fog : TEXCOORD1;
  float view_depth : TEXCOORD2;
};

VertexOutput vs_main(VertexInput input) {
  VertexOutput output;
  const float depth = max(input.position.z, near_plane);
  const float2 normalized =
      float2(input.position.x * 2.0 / output_width - 1.0,
             1.0 - input.position.y * 2.0 / output_height);
  output.position = float4(normalized * depth, depth - near_plane, depth);
  output.color = input.color;
  output.texture_coordinate = input.texture_coordinate;
  output.fog = input.fog;
  output.view_depth = depth;
  return output;
}

float4 pixel_fog(VertexOutput input) {
  float4 fog = input.fog;
  if (fog_enabled > 0.5 && input.view_depth > distance_fog_range.x) {
    const float normalized_cue =
        saturate((input.view_depth - distance_fog_range.x) /
                 max(0.0001, distance_fog_range.y - distance_fog_range.x));
    const float clear_fraction = 1.0 - normalized_cue;
    const float depth_cue = 1.0 - clear_fraction * clear_fraction;
    fog = float4(distance_fog_color.rgb, max(fog.a, depth_cue));
  }
  return fog;
}

Texture2D scene_texture : register(t0);
SamplerState scene_sampler : register(s0);

float4 shade_scene(VertexOutput input, bool additive) {
  // Emulate the finite texture-coordinate precision and mip-point sampling
  // of the original Glide-class raster path instead of inheriting the full
  // precision of the D3D11 host API.
  const float2 glide_uv =
      floor(input.texture_coordinate * 256.0 + 0.5) / 256.0;
  const float4 texel = scene_texture.Sample(scene_sampler, glide_uv);
  const float alpha = texel.a * input.color.a;
  float3 base = texel.rgb * input.color.rgb;
  const float4 fog = pixel_fog(input);
  if (additive) {
    base *= 1.0 - saturate(fog.a);
  } else {
    base = lerp(base, fog.rgb, saturate(fog.a));
  }
  return float4(base, alpha);
}

float3 apply_glide_output(float3 color, float2 pixel) {
  // Four-by-four ordered dither followed by a real RGB565 quantization.  The
  // previous two-bit perturbation was almost invisible at modern output
  // sizes and left Glide indistinguishable from the D3D11 path.
  const int2 position = int2(pixel) & 3;
  static const float bayer[16] = {
       0.0,  8.0,  2.0, 10.0,
      12.0,  4.0, 14.0,  6.0,
       3.0, 11.0,  1.0,  9.0,
      15.0,  7.0, 13.0,  5.0};
  const float threshold = bayer[position.y * 4 + position.x] / 16.0 - 0.5;
  const float dither = threshold / 31.0;
  const float3 levels = float3(31.0, 63.0, 31.0);
  const float3 limited = floor(saturate(color) * 255.0 + 0.5) / 255.0;
  return floor(saturate(limited + dither) * levels + 0.5) / levels;
}

float4 ps_opaque(VertexOutput input) : SV_Target {
  const float4 result = shade_scene(input, false);
  clip(result.a - 0.5);
  return float4(apply_glide_output(result.rgb, input.position.xy), 1.0);
}

float4 ps_alpha(VertexOutput input) : SV_Target {
  const float4 result = shade_scene(input, false);
  clip(result.a - 0.00001);
  return float4(apply_glide_output(result.rgb, input.position.xy), result.a);
}

float4 ps_additive(VertexOutput input) : SV_Target {
  const float4 result = shade_scene(input, true);
  clip(result.a - 0.00001);
  return float4(apply_glide_output(result.rgb, input.position.xy), result.a);
}

float4 ps_tron_fill(VertexOutput input) : SV_Target {
  return float4(0.0, 0.0, 0.5, 1.0);
}

float4 ps_tron_line(VertexOutput input) : SV_Target {
  return float4(0.44, 0.58, 1.0, 1.0);
}
)";

constexpr const char projection_shader_source[] = R"(
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

struct ProjectionVertexInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float2 texture_coordinate : TEXCOORD0;
  float4 fog : TEXCOORD1;
};

struct ProjectionVertexOutput {
  float4 position : SV_Position;
};

ProjectionVertexOutput vs_projection(ProjectionVertexInput input) {
  ProjectionVertexOutput output;
  const float2 normalized =
      float2(input.position.x * 2.0 / output_and_projection.x - 1.0,
             1.0 - input.position.y * 2.0 / output_and_projection.y);
  output.position = float4(normalized, 0.0, 1.0);
  return output;
}

Texture2D<float> scene_depth : register(t0);

float3 reconstruct_world(float2 pixel, float hardware_depth,
                         out float view_depth) {
  const float near_plane = output_and_projection.w;
  view_depth = near_plane / max(1.0e-7, 1.0 - hardware_depth);
  const float view_x =
      (pixel.x - output_and_projection.x * 0.0 - output_and_projection.z) /
      cue_parameters.w * view_depth;
  const float view_y =
      -(pixel.y - view_position.w) / cue_parameters.w * view_depth;
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
  if (cue_parameters.z < 0.5 || depth <= cue_parameters.y) {
    return 0.0;
  }
  const float normalized_cue =
      saturate((depth - cue_parameters.y) /
               max(0.0001, cue_parameters.x - cue_parameters.y));
  const float clear_fraction = 1.0 - normalized_cue;
  return 1.0 - clear_fraction * clear_fraction * clear_fraction;
}

float4 ps_shadow(ProjectionVertexOutput input) : SV_Target {
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
        triangle_contains(pair0.xy, pair1.xy, pair1.zw, p)) {
      accepted = true;
    }
  }
  if (!accepted) discard;
  const float cue = cue_factor(view_depth);
  return float4(lerp(float3(10.0 / 255.0, 10.0 / 255.0, 10.0 / 255.0),
                     cue_color.rgb, cue), 1.0);
}

static const float headlight_distances[7] =
    {0.35, 2.0, 5.0, 8.0, 11.0, 15.0, 20.0};

float4 ps_headlight(ProjectionVertexOutput input) : SV_Target {
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
  const float average_center_z =
      (source_0_center.z + source_1_center.z) * 0.5;
  const float average_direction_z =
      (source_0_direction.z + source_1_direction.z) * 0.5;
  if (abs(average_direction_z) < 1.0e-7) discard;
  const float distance = (local.z - average_center_z) / average_direction_z;
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
  return float4((150.0 / 255.0).xxx * intensity, 1.0);
}
)";

struct HardwareVertex {
  float x;
  float y;
  float depth;
  float red;
  float green;
  float blue;
  float alpha;
  float u;
  float v;
  float fog_red;
  float fog_green;
  float fog_blue;
  float fog_alpha;
};

struct SceneConstants {
  float width;
  float height;
  float near_plane;
  float fog_enabled;
  std::array<float, 4U> distance_fog_color{};
  std::array<float, 4U> distance_fog_range{};
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

class D3D11StateGuard {
public:
  explicit D3D11StateGuard(ID3D11DeviceContext *context) : context_(context) {
    context_->IAGetInputLayout(&input_layout_);
    context_->IAGetVertexBuffers(0U, 1U, &vertex_buffer_, &vertex_stride_,
                                 &vertex_offset_);
    context_->IAGetIndexBuffer(&index_buffer_, &index_format_, &index_offset_);
    context_->IAGetPrimitiveTopology(&topology_);
    context_->VSGetShader(&vertex_shader_, nullptr, nullptr);
    context_->VSGetConstantBuffers(0U, 1U, &vertex_constant_);
    context_->PSGetShader(&pixel_shader_, nullptr, nullptr);
    context_->PSGetConstantBuffers(0U, 1U, &pixel_constant_);
    context_->PSGetShaderResources(0U, shader_resources_.size(),
                                   shader_resources_.data());
    context_->PSGetSamplers(0U, samplers_.size(), samplers_.data());
    context_->RSGetState(&rasterizer_);
    viewport_count_ = viewports_.size();
    context_->RSGetViewports(&viewport_count_, viewports_.data());
    scissor_count_ = scissors_.size();
    context_->RSGetScissorRects(&scissor_count_, scissors_.data());
    context_->OMGetRenderTargets(render_targets_.size(), render_targets_.data(),
                                 &depth_target_);
    context_->OMGetBlendState(&blend_state_, blend_factor_, &sample_mask_);
    context_->OMGetDepthStencilState(&depth_state_, &stencil_reference_);
  }

  D3D11StateGuard(const D3D11StateGuard &) = delete;
  D3D11StateGuard &operator=(const D3D11StateGuard &) = delete;

  ~D3D11StateGuard() {
    context_->IASetInputLayout(input_layout_);
    context_->IASetVertexBuffers(0U, 1U, &vertex_buffer_, &vertex_stride_,
                                 &vertex_offset_);
    context_->IASetIndexBuffer(index_buffer_, index_format_, index_offset_);
    context_->IASetPrimitiveTopology(topology_);
    context_->VSSetShader(vertex_shader_, nullptr, 0U);
    context_->VSSetConstantBuffers(0U, 1U, &vertex_constant_);
    context_->PSSetShader(pixel_shader_, nullptr, 0U);
    context_->PSSetConstantBuffers(0U, 1U, &pixel_constant_);
    context_->PSSetShaderResources(0U, shader_resources_.size(),
                                   shader_resources_.data());
    context_->PSSetSamplers(0U, samplers_.size(), samplers_.data());
    context_->RSSetState(rasterizer_);
    context_->RSSetViewports(viewport_count_, viewports_.data());
    context_->RSSetScissorRects(scissor_count_, scissors_.data());
    context_->OMSetRenderTargets(render_targets_.size(), render_targets_.data(),
                                 depth_target_);
    context_->OMSetBlendState(blend_state_, blend_factor_, sample_mask_);
    context_->OMSetDepthStencilState(depth_state_, stencil_reference_);
    release(input_layout_);
    release(vertex_buffer_);
    release(index_buffer_);
    release(vertex_shader_);
    release(vertex_constant_);
    release(pixel_shader_);
    release(pixel_constant_);
    for (auto *resource : shader_resources_) release(resource);
    for (auto *sampler : samplers_) release(sampler);
    release(rasterizer_);
    for (auto *target : render_targets_) release(target);
    release(depth_target_);
    release(blend_state_);
    release(depth_state_);
  }

private:
  template <typename T> static void release(T *value) {
    if (value != nullptr) value->Release();
  }

  ID3D11DeviceContext *context_ = nullptr;
  ID3D11InputLayout *input_layout_ = nullptr;
  ID3D11Buffer *vertex_buffer_ = nullptr;
  UINT vertex_stride_ = 0U;
  UINT vertex_offset_ = 0U;
  ID3D11Buffer *index_buffer_ = nullptr;
  DXGI_FORMAT index_format_ = DXGI_FORMAT_UNKNOWN;
  UINT index_offset_ = 0U;
  D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11VertexShader *vertex_shader_ = nullptr;
  ID3D11Buffer *vertex_constant_ = nullptr;
  ID3D11PixelShader *pixel_shader_ = nullptr;
  ID3D11Buffer *pixel_constant_ = nullptr;
  std::array<ID3D11ShaderResourceView *, 4U> shader_resources_{};
  std::array<ID3D11SamplerState *, 4U> samplers_{};
  ID3D11RasterizerState *rasterizer_ = nullptr;
  std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
      viewports_{};
  UINT viewport_count_ = 0U;
  std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
      scissors_{};
  UINT scissor_count_ = 0U;
  std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
      render_targets_{};
  ID3D11DepthStencilView *depth_target_ = nullptr;
  ID3D11BlendState *blend_state_ = nullptr;
  FLOAT blend_factor_[4U]{};
  UINT sample_mask_ = 0xffffffffU;
  ID3D11DepthStencilState *depth_state_ = nullptr;
  UINT stencil_reference_ = 0U;
};

ComPointer<ID3DBlob> compile_shader(const char *source, const char *entry,
                                    const char *target) {
  ComPointer<ID3DBlob> bytecode;
  ComPointer<ID3DBlob> errors;
  const auto result = mh::render::hardware::compile_shader(
      source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
      D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U,
      bytecode.put(), errors.put());
  if (FAILED(result)) {
    const auto message = errors
                             ? std::string(static_cast<const char *>(
                                               errors.get()->GetBufferPointer()),
                                           errors.get()->GetBufferSize())
                             : std::string("unknown shader compiler error");
    throw std::runtime_error(std::string("compile hardware scene shader: ") +
                             message);
  }
  return bytecode;
}

template <std::size_t Size>
std::array<float, 4U> vector4(const std::array<double, Size> &source,
                             const float fourth = 0.0F) {
  static_assert(Size == 3U);
  return {static_cast<float>(source[0U]), static_cast<float>(source[1U]),
          static_cast<float>(source[2U]), fourth};
}

} // namespace

struct GlideRenderer::Impl {
  struct TextureResource {
    ComPointer<ID3D11Texture2D> texture;
    ComPointer<ID3D11ShaderResourceView> view;
  };

  struct DrawBatch {
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
    const std::vector<mh::content::PamRgbaImage> *texture_levels = nullptr;
    SceneBlendMode blend_mode = SceneBlendMode::opaque;
    bool trilinear = false;
    bool depth_bias = false;
    bool depth_test = true;
    std::optional<std::size_t> shadow;
  };

  ComPointer<ID3D11Device> device;
  ComPointer<ID3D11DeviceContext> context;
  ComPointer<ID3D11Texture2D> color_texture;
  ComPointer<ID3D11RenderTargetView> color_target;
  ComPointer<ID3D11Texture2D> depth_texture;
  ComPointer<ID3D11DepthStencilView> depth_target;
  ComPointer<ID3D11ShaderResourceView> depth_view;
  SDL_Texture *wrapped_texture = nullptr;
  int width = 0;
  int height = 0;

  ComPointer<ID3D11VertexShader> scene_vertex_shader;
  ComPointer<ID3D11VertexShader> projection_vertex_shader;
  ComPointer<ID3D11PixelShader> opaque_shader;
  ComPointer<ID3D11PixelShader> alpha_shader;
  ComPointer<ID3D11PixelShader> additive_shader;
  ComPointer<ID3D11PixelShader> tron_fill_shader;
  ComPointer<ID3D11PixelShader> tron_line_shader;
  ComPointer<ID3D11PixelShader> shadow_shader;
  ComPointer<ID3D11PixelShader> headlight_shader;
  ComPointer<ID3D11InputLayout> input_layout;
  ComPointer<ID3D11Buffer> vertex_buffer;
  std::size_t vertex_capacity = 0U;
  ComPointer<ID3D11Buffer> scene_constants;
  ComPointer<ID3D11Buffer> projection_constants;
  ComPointer<ID3D11SamplerState> linear_sampler;
  ComPointer<ID3D11SamplerState> trilinear_sampler;
  ComPointer<ID3D11DepthStencilState> depth_write_state;
  ComPointer<ID3D11DepthStencilState> depth_read_state;
  ComPointer<ID3D11DepthStencilState> depth_disabled_state;
  ComPointer<ID3D11BlendState> opaque_blend;
  ComPointer<ID3D11BlendState> alpha_blend;
  ComPointer<ID3D11BlendState> additive_blend;
  ComPointer<ID3D11RasterizerState> solid_rasterizer;
  ComPointer<ID3D11RasterizerState> biased_rasterizer;
  ComPointer<ID3D11RasterizerState> wire_rasterizer;
  std::unordered_map<const std::vector<mh::content::PamRgbaImage> *,
                     TextureResource>
      textures;
  TextureResource white_texture;
  std::vector<HardwareVertex> vertices;
  std::vector<DrawBatch> batches;
  double render_ms = 0.0;
  bool initialized = false;
  ~Impl() {
    if (wrapped_texture != nullptr) {
      SDL_DestroyTexture(wrapped_texture);
    }
  }

  void create_static_resources() {
    const auto vertex_code = compile_shader(scene_shader_source, "vs_main",
                                            "vs_5_0");
    require_hresult(device.get()->CreateVertexShader(
                        vertex_code.get()->GetBufferPointer(),
                        vertex_code.get()->GetBufferSize(), nullptr,
                        scene_vertex_shader.put()),
                    "create scene vertex shader");
    const auto projection_vertex_code = compile_shader(
        projection_shader_source, "vs_projection", "vs_5_0");
    require_hresult(device.get()->CreateVertexShader(
                        projection_vertex_code.get()->GetBufferPointer(),
                        projection_vertex_code.get()->GetBufferSize(), nullptr,
                        projection_vertex_shader.put()),
                    "create projection vertex shader");
    const auto make_pixel = [this](const char *source, const char *entry,
                                   ComPointer<ID3D11PixelShader> &target) {
      const auto code = compile_shader(source, entry, "ps_5_0");
      require_hresult(device.get()->CreatePixelShader(
                          code.get()->GetBufferPointer(),
                          code.get()->GetBufferSize(), nullptr, target.put()),
                      "create scene pixel shader");
    };
    make_pixel(scene_shader_source, "ps_opaque", opaque_shader);
    make_pixel(scene_shader_source, "ps_alpha", alpha_shader);
    make_pixel(scene_shader_source, "ps_additive", additive_shader);
    make_pixel(scene_shader_source, "ps_tron_fill", tron_fill_shader);
    make_pixel(scene_shader_source, "ps_tron_line", tron_line_shader);
    make_pixel(projection_shader_source, "ps_shadow", shadow_shader);
    make_pixel(projection_shader_source, "ps_headlight", headlight_shader);

    constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4U> elements{{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U,
         static_cast<UINT>(offsetof(HardwareVertex, x)),
         D3D11_INPUT_PER_VERTEX_DATA, 0U},
        {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U,
         static_cast<UINT>(offsetof(HardwareVertex, red)),
         D3D11_INPUT_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U,
         static_cast<UINT>(offsetof(HardwareVertex, u)),
         D3D11_INPUT_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 1U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U,
         static_cast<UINT>(offsetof(HardwareVertex, fog_red)),
         D3D11_INPUT_PER_VERTEX_DATA, 0U},
    }};
    require_hresult(device.get()->CreateInputLayout(
                        elements.data(), static_cast<UINT>(elements.size()),
                        vertex_code.get()->GetBufferPointer(),
                        vertex_code.get()->GetBufferSize(), input_layout.put()),
                    "create scene input layout");

    const auto make_constant_buffer = [this](const std::size_t size,
                                             ComPointer<ID3D11Buffer> &target) {
      D3D11_BUFFER_DESC description{};
      description.ByteWidth = static_cast<UINT>((size + 15U) & ~15U);
      description.Usage = D3D11_USAGE_DYNAMIC;
      description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      require_hresult(device.get()->CreateBuffer(&description, nullptr,
                                                 target.put()),
                      "create scene constant buffer");
    };
    make_constant_buffer(sizeof(SceneConstants), scene_constants);
    make_constant_buffer(sizeof(ProjectionConstants), projection_constants);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    require_hresult(device.get()->CreateSamplerState(&sampler,
                                                     linear_sampler.put()),
                    "create scene sampler");
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    require_hresult(device.get()->CreateSamplerState(&sampler,
                                                     trilinear_sampler.put()),
                    "create trilinear scene sampler");

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS;
    require_hresult(device.get()->CreateDepthStencilState(
                        &depth, depth_write_state.put()),
                    "create depth-write state");
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    require_hresult(device.get()->CreateDepthStencilState(
                        &depth, depth_read_state.put()),
                    "create depth-read state");
    depth.DepthEnable = FALSE;
    require_hresult(device.get()->CreateDepthStencilState(
                        &depth, depth_disabled_state.put()),
                    "create disabled-depth state");

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0U].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    require_hresult(device.get()->CreateBlendState(&blend, opaque_blend.put()),
                    "create opaque blend state");
    blend.RenderTarget[0U].BlendEnable = TRUE;
    blend.RenderTarget[0U].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0U].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0U].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0U].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0U].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0U].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    require_hresult(device.get()->CreateBlendState(&blend, alpha_blend.put()),
                    "create alpha blend state");
    blend.RenderTarget[0U].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0U].DestBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0U].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0U].DestBlendAlpha = D3D11_BLEND_ONE;
    require_hresult(device.get()->CreateBlendState(&blend,
                                                   additive_blend.put()),
                    "create additive blend state");

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.ScissorEnable = TRUE;
    rasterizer.DepthClipEnable = TRUE;
    require_hresult(device.get()->CreateRasterizerState(
                        &rasterizer, solid_rasterizer.put()),
                    "create scene rasterizer");
    rasterizer.DepthBias = -1;
    rasterizer.SlopeScaledDepthBias = -0.25F;
    require_hresult(device.get()->CreateRasterizerState(
                        &rasterizer, biased_rasterizer.put()),
                    "create biased scene rasterizer");
    rasterizer.DepthBias = 0;
    rasterizer.SlopeScaledDepthBias = 0.0F;
    rasterizer.FillMode = D3D11_FILL_WIREFRAME;
    require_hresult(device.get()->CreateRasterizerState(
                        &rasterizer, wire_rasterizer.put()),
                    "create wireframe scene rasterizer");

    mh::content::PamRgbaImage white;
    white.width = 1U;
    white.height = 1U;
    white.rgba = {255U, 255U, 255U, 255U};
    const std::vector<mh::content::PamRgbaImage> levels{white};
    white_texture = create_texture(levels);
  }

  TextureResource create_texture(
      const std::vector<mh::content::PamRgbaImage> &levels) {
    if (levels.empty()) {
      throw std::runtime_error("hardware scene texture has no image levels");
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = levels.front().width;
    description.Height = levels.front().height;
    description.MipLevels = static_cast<UINT>(levels.size());
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1U;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> initial(levels.size());
    for (std::size_t level = 0U; level < levels.size(); ++level) {
      const auto &image = levels[level];
      if (image.width == 0U || image.height == 0U ||
          image.rgba.size() !=
              static_cast<std::size_t>(image.width) * image.height * 4U) {
        throw std::runtime_error("hardware scene texture level is invalid");
      }
      initial[level].pSysMem = image.rgba.data();
      initial[level].SysMemPitch = image.width * 4U;
    }
    TextureResource result;
    require_hresult(device.get()->CreateTexture2D(&description, initial.data(),
                                                  result.texture.put()),
                    "create hardware scene texture");
    require_hresult(device.get()->CreateShaderResourceView(
                        result.texture.get(), nullptr, result.view.put()),
                    "create hardware scene texture view");
    return result;
  }

  TextureResource &texture_for(
      const std::vector<mh::content::PamRgbaImage> *levels) {
    if (levels == nullptr || levels->empty()) {
      return white_texture;
    }
    const auto found = textures.find(levels);
    if (found != textures.end()) {
      return found->second;
    }
    return textures.emplace(levels, create_texture(*levels)).first->second;
  }

  void create_targets(SDL_Renderer *renderer, const int new_width,
                      const int new_height) {
    if (wrapped_texture != nullptr) {
      SDL_DestroyTexture(wrapped_texture);
      wrapped_texture = nullptr;
    }
    color_target.reset();
    color_texture.reset();
    depth_view.reset();
    depth_target.reset();
    depth_texture.reset();

    D3D11_TEXTURE2D_DESC color{};
    color.Width = static_cast<UINT>(new_width);
    color.Height = static_cast<UINT>(new_height);
    color.MipLevels = 1U;
    color.ArraySize = 1U;
    color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color.SampleDesc.Count = 1U;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    require_hresult(device.get()->CreateTexture2D(&color, nullptr,
                                                  color_texture.put()),
                    "create hardware scene color target");
    require_hresult(device.get()->CreateRenderTargetView(
                        color_texture.get(), nullptr, color_target.put()),
                    "create hardware scene color view");

    D3D11_TEXTURE2D_DESC depth = color;
    depth.Format = DXGI_FORMAT_R32_TYPELESS;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    require_hresult(device.get()->CreateTexture2D(&depth, nullptr,
                                                  depth_texture.put()),
                    "create hardware scene depth target");
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_target_description{};
    depth_target_description.Format = DXGI_FORMAT_D32_FLOAT;
    depth_target_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    require_hresult(device.get()->CreateDepthStencilView(
                        depth_texture.get(), &depth_target_description,
                        depth_target.put()),
                    "create hardware scene depth view");
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_view_description{};
    depth_view_description.Format = DXGI_FORMAT_R32_FLOAT;
    depth_view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depth_view_description.Texture2D.MipLevels = 1U;
    require_hresult(device.get()->CreateShaderResourceView(
                        depth_texture.get(), &depth_view_description,
                        depth_view.put()),
                    "create hardware scene depth sampler");

    const auto properties = SDL_CreateProperties();
    if (properties == 0U) {
      throw std::runtime_error(std::string("create scene texture properties: ") +
                               SDL_GetError());
    }
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                          SDL_PIXELFORMAT_RGBA32);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                          new_width);
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                          new_height);
    SDL_SetPointerProperty(properties,
                           SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,
                           color_texture.get());
    wrapped_texture = SDL_CreateTextureWithProperties(renderer, properties);
    SDL_DestroyProperties(properties);
    if (wrapped_texture == nullptr) {
      throw std::runtime_error(std::string("wrap hardware scene texture: ") +
                               SDL_GetError());
    }
    if (!SDL_SetTextureBlendMode(wrapped_texture, SDL_BLENDMODE_BLEND) ||
        !SDL_SetTextureScaleMode(wrapped_texture, SDL_SCALEMODE_LINEAR)) {
      throw std::runtime_error(std::string("configure hardware scene texture: ") +
                               SDL_GetError());
    }
    width = new_width;
    height = new_height;
  }

  void ensure_vertex_buffer(const std::size_t count) {
    if (count <= vertex_capacity) {
      return;
    }
    vertex_capacity = std::max(count, vertex_capacity * 2U + 4096U);
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(
        vertex_capacity * sizeof(HardwareVertex));
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    require_hresult(device.get()->CreateBuffer(&description, nullptr,
                                               vertex_buffer.put()),
                    "create hardware scene vertex buffer");
  }

  static HardwareVertex convert_vertex(const SceneRasterCommand &command,
                                       const std::size_t vertex) {
    const auto &source = command.vertices[vertex];
    return {source.position.x,
            source.position.y,
            static_cast<float>(command.view_depths[vertex]),
            source.color.r,
            source.color.g,
            source.color.b,
            source.color.a,
            source.tex_coord.x,
            source.tex_coord.y,
            0.0F,
            0.0F,
            0.0F,
            0.0F};
  }

  void build_batches(const SceneFrameView &frame) {
    vertices.clear();
    batches.clear();
    for (const auto &command : *frame.commands) {
      if (command.shadow_projection_index.has_value()) {
        DrawBatch marker;
        marker.shadow = command.shadow_projection_index;
        batches.push_back(marker);
        continue;
      }
      if (command.vertex_count < 3U || command.vertex_count > 4U) {
        continue;
      }
      const auto first = static_cast<std::uint32_t>(vertices.size());
      constexpr std::array<std::size_t, 6U> quad_indices{0U, 1U, 2U,
                                                        0U, 2U, 3U};
      const auto count = command.vertex_count == 4U ? 6U : 3U;
      for (std::size_t index = 0U; index < count; ++index) {
        const auto vertex = command.vertex_count == 4U ? quad_indices[index]
                                                       : index;
        vertices.push_back(convert_vertex(command, vertex));
      }
      const auto compatible = !batches.empty() && !batches.back().shadow &&
                              batches.back().first_vertex +
                                      batches.back().vertex_count ==
                                  first &&
                              batches.back().texture_levels ==
                                  command.texture_levels &&
                              batches.back().blend_mode == command.blend_mode &&
                              batches.back().trilinear ==
                                  command.trilinear_filtering &&
                              batches.back().depth_bias ==
                                  command.coplanar_depth_bias &&
                              batches.back().depth_test == command.depth_test;
      if (compatible) {
        batches.back().vertex_count += static_cast<std::uint32_t>(count);
      } else {
        DrawBatch batch;
        batch.first_vertex = first;
        batch.vertex_count = static_cast<std::uint32_t>(count);
        batch.texture_levels = command.texture_levels;
        batch.blend_mode = command.blend_mode;
        batch.trilinear = command.trilinear_filtering;
        batch.depth_bias = command.coplanar_depth_bias;
        batch.depth_test = command.depth_test;
        batches.push_back(batch);
      }
    }
  }

  template <typename Constants>
  void update_constants(ID3D11Buffer *buffer, const Constants &constants) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require_hresult(context.get()->Map(buffer, 0U, D3D11_MAP_WRITE_DISCARD, 0U,
                                       &mapped),
                    "map hardware scene constants");
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context.get()->Unmap(buffer, 0U);
  }

  ProjectionConstants projection_base(const PerspectiveView &view,
                                      const double render_distance,
                                      const double cue_start,
                                      const bool cue_enabled,
                                      const std::array<float, 3U> &color) {
    ProjectionConstants constants;
    constants.output_and_projection =
        {static_cast<float>(width), static_cast<float>(height), view.center_x,
         static_cast<float>(view.near_plane)};
    constants.view_position = vector4(view.position, view.center_y);
    constants.view_right = vector4(view.right);
    constants.view_up = vector4(view.up);
    constants.view_forward = vector4(view.forward);
    constants.cue_parameters = {static_cast<float>(render_distance),
                                static_cast<float>(cue_start),
                                cue_enabled ? 1.0F : 0.0F,
                                view.focal_length};
    constants.cue_color = {color[0U], color[1U], color[2U], 1.0F};
    return constants;
  }

  void draw_projection_quad(const int minimum_x, const int maximum_x,
                            const int minimum_y, const int maximum_y) {
    const auto first = static_cast<std::uint32_t>(vertices.size());
    const auto left = static_cast<float>(minimum_x);
    const auto right = static_cast<float>(maximum_x + 1);
    const auto top = static_cast<float>(minimum_y);
    const auto bottom = static_cast<float>(maximum_y + 1);
    auto put = [this](const float x, const float y) {
      HardwareVertex vertex{};
      vertex.x = x;
      vertex.y = y;
      vertex.depth = 1.0F;
      vertices.push_back(vertex);
    };
    put(left, top);
    put(right, top);
    put(right, bottom);
    put(left, top);
    put(right, bottom);
    put(left, bottom);

    ensure_vertex_buffer(vertices.size());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require_hresult(context.get()->Map(vertex_buffer.get(), 0U,
                                       D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                    "map projection vertices");
    std::memcpy(mapped.pData, vertices.data(),
                vertices.size() * sizeof(HardwareVertex));
    context.get()->Unmap(vertex_buffer.get(), 0U);
    context.get()->Draw(6U, first);
    vertices.resize(first);
  }

  void bind_projection_state(ID3D11PixelShader *shader,
                             const D3D11_RECT &scissor,
                             ID3D11BlendState *blend) {
    ID3D11RenderTargetView *target = color_target.get();
    context.get()->OMSetRenderTargets(1U, &target, nullptr);
    context.get()->OMSetDepthStencilState(depth_disabled_state.get(), 0U);
    context.get()->OMSetBlendState(blend, nullptr, 0xffffffffU);
    context.get()->RSSetState(solid_rasterizer.get());
    context.get()->RSSetScissorRects(1U, &scissor);
    context.get()->VSSetShader(projection_vertex_shader.get(), nullptr, 0U);
    context.get()->PSSetShader(shader, nullptr, 0U);
    ID3D11Buffer *constant = projection_constants.get();
    context.get()->PSSetConstantBuffers(0U, 1U, &constant);
    ID3D11ShaderResourceView *sampled_depth = depth_view.get();
    context.get()->PSSetShaderResources(0U, 1U, &sampled_depth);
  }

  void draw_shadow(const VehicleShadowDepthProjection &projection) {
    auto constants = projection_base(
        projection.view, projection.render_distance, projection.cue_start,
        projection.cue_enabled,
        {projection.cue_color[0U] / 255.0F,
         projection.cue_color[1U] / 255.0F,
         projection.cue_color[2U] / 255.0F});
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
    update_constants(projection_constants.get(), constants);
    const D3D11_RECT scissor{projection.minimum_x, projection.minimum_y,
                             projection.maximum_x + 1,
                             projection.maximum_y + 1};
    bind_projection_state(shadow_shader.get(), scissor, opaque_blend.get());
    draw_projection_quad(projection.minimum_x, projection.maximum_x,
                         projection.minimum_y, projection.maximum_y);
    ID3D11ShaderResourceView *none = nullptr;
    context.get()->PSSetShaderResources(0U, 1U, &none);
  }

  void draw_headlight(const HeadlightDepthProjection &projection) {
    auto constants = projection_base(projection.view,
                                     projection.render_distance,
                                     projection.cue_start,
                                     projection.cue_enabled,
                                     {0.0F, 0.0F, 0.0F});
    constants.vehicle_position = vector4(projection.vehicle.world_position);
    constants.body_basis_0 = vector4(projection.vehicle.body_basis[0U]);
    constants.body_basis_1 = vector4(projection.vehicle.body_basis[1U]);
    constants.body_basis_2 = vector4(projection.vehicle.body_basis[2U]);
    constants.source_0_center = vector4(projection.sources[0U].center);
    constants.source_0_direction = vector4(projection.sources[0U].direction);
    constants.source_1_center = vector4(projection.sources[1U].center);
    constants.source_1_direction = vector4(projection.sources[1U].direction);
    update_constants(projection_constants.get(), constants);
    const D3D11_RECT scissor{projection.minimum_x, projection.minimum_y,
                             projection.maximum_x + 1,
                             projection.maximum_y + 1};
    bind_projection_state(headlight_shader.get(), scissor,
                          additive_blend.get());
    draw_projection_quad(projection.minimum_x, projection.maximum_x,
                         projection.minimum_y, projection.maximum_y);
    ID3D11ShaderResourceView *none = nullptr;
    context.get()->PSSetShaderResources(0U, 1U, &none);
  }

  void bind_geometry_targets() {
    ID3D11RenderTargetView *target = color_target.get();
    context.get()->OMSetRenderTargets(1U, &target, depth_target.get());
  }

  void draw_geometry_batch(const DrawBatch &batch, const bool tron,
                           const bool wire = false) {
    auto &texture = texture_for(batch.texture_levels);
    ID3D11ShaderResourceView *view = texture.view.get();
    context.get()->PSSetShaderResources(0U, 1U, &view);
    ID3D11SamplerState *sampler =
        batch.trilinear ? trilinear_sampler.get() : linear_sampler.get();
    context.get()->PSSetSamplers(0U, 1U, &sampler);
    context.get()->RSSetState(wire ? wire_rasterizer.get()
                                  : (batch.depth_bias
                                         ? biased_rasterizer.get()
                                         : solid_rasterizer.get()));
    if (wire) {
      context.get()->PSSetShader(tron_line_shader.get(), nullptr, 0U);
      context.get()->OMSetDepthStencilState(depth_read_state.get(), 0U);
      context.get()->OMSetBlendState(opaque_blend.get(), nullptr, 0xffffffffU);
    } else if (tron) {
      context.get()->PSSetShader(tron_fill_shader.get(), nullptr, 0U);
      context.get()->OMSetDepthStencilState(depth_write_state.get(), 0U);
      context.get()->OMSetBlendState(opaque_blend.get(), nullptr, 0xffffffffU);
    } else if (batch.blend_mode == SceneBlendMode::opaque) {
      context.get()->PSSetShader(opaque_shader.get(), nullptr, 0U);
      context.get()->OMSetDepthStencilState(
          batch.depth_test ? depth_write_state.get()
                           : depth_disabled_state.get(),
          0U);
      context.get()->OMSetBlendState(opaque_blend.get(), nullptr, 0xffffffffU);
    } else if (batch.blend_mode == SceneBlendMode::alpha) {
      context.get()->PSSetShader(alpha_shader.get(), nullptr, 0U);
      context.get()->OMSetDepthStencilState(
          batch.depth_test ? depth_read_state.get()
                           : depth_disabled_state.get(),
          0U);
      context.get()->OMSetBlendState(alpha_blend.get(), nullptr, 0xffffffffU);
    } else {
      context.get()->PSSetShader(additive_shader.get(), nullptr, 0U);
      context.get()->OMSetDepthStencilState(
          batch.depth_test ? depth_read_state.get()
                           : depth_disabled_state.get(),
          0U);
      context.get()->OMSetBlendState(additive_blend.get(), nullptr,
                                     0xffffffffU);
    }
    context.get()->Draw(batch.vertex_count, batch.first_vertex);
  }

  SDL_Texture *render(SDL_Renderer *renderer, const SceneFrameView &frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.commands == nullptr ||
        frame.shadow_projections == nullptr ||
        frame.headlight_projections == nullptr) {
      throw std::runtime_error("hardware scene frame is incomplete");
    }
    if (width != frame.width || height != frame.height ||
        wrapped_texture == nullptr) {
      create_targets(renderer, frame.width, frame.height);
    }
    if (!SDL_FlushRenderer(renderer)) {
      throw std::runtime_error(std::string("flush SDL before hardware scene: ") +
                               SDL_GetError());
    }
    D3D11StateGuard state_guard(context.get());
    const auto started = std::chrono::steady_clock::now();
    build_batches(frame);
    ensure_vertex_buffer(vertices.size() + 6U);
    if (!vertices.empty()) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      require_hresult(context.get()->Map(vertex_buffer.get(), 0U,
                                         D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                      "map hardware scene vertices");
      std::memcpy(mapped.pData, vertices.data(),
                  vertices.size() * sizeof(HardwareVertex));
      context.get()->Unmap(vertex_buffer.get(), 0U);
    }

    const float transparent[4U]{0.0F, 0.0F, 0.0F, 0.0F};
    context.get()->ClearRenderTargetView(color_target.get(), transparent);
    context.get()->ClearDepthStencilView(depth_target.get(),
                                         D3D11_CLEAR_DEPTH, 1.0F, 0U);
    bind_geometry_targets();
    const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width),
                                  static_cast<float>(height), 0.0F, 1.0F};
    context.get()->RSSetViewports(1U, &viewport);
    const D3D11_RECT full_scissor{0, 0, width, height};
    context.get()->RSSetScissorRects(1U, &full_scissor);
    const UINT stride = sizeof(HardwareVertex);
    const UINT offset = 0U;
    ID3D11Buffer *buffer = vertex_buffer.get();
    context.get()->IASetVertexBuffers(0U, 1U, &buffer, &stride, &offset);
    context.get()->IASetInputLayout(input_layout.get());
    context.get()->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.get()->VSSetShader(scene_vertex_shader.get(), nullptr, 0U);
    const SceneConstants scene{
        static_cast<float>(width), static_cast<float>(height),
        static_cast<float>(frame.near_plane), frame.fog_enabled ? 1.0F : 0.0F,
        {frame.fog_color[0U] / 255.0F, frame.fog_color[1U] / 255.0F,
         frame.fog_color[2U] / 255.0F, 1.0F},
        {static_cast<float>(frame.fog_start),
         static_cast<float>(frame.fog_end), 0.0F, 0.0F}};
    update_constants(scene_constants.get(), scene);
    ID3D11Buffer *constant = scene_constants.get();
    context.get()->VSSetConstantBuffers(0U, 1U, &constant);
    context.get()->PSSetConstantBuffers(0U, 1U, &constant);

    for (const auto &batch : batches) {
      if (batch.shadow.has_value()) {
        if (!frame.tron_hidden_line &&
            *batch.shadow < frame.shadow_projections->size()) {
          draw_shadow(frame.shadow_projections->at(*batch.shadow));
          bind_geometry_targets();
          context.get()->VSSetShader(scene_vertex_shader.get(), nullptr, 0U);
          context.get()->VSSetConstantBuffers(0U, 1U, &constant);
          context.get()->PSSetConstantBuffers(0U, 1U, &constant);
          context.get()->RSSetScissorRects(1U, &full_scissor);
        }
        continue;
      }
      draw_geometry_batch(batch, frame.tron_hidden_line);
    }
    if (frame.tron_hidden_line) {
      bind_geometry_targets();
      for (const auto &batch : batches) {
        if (!batch.shadow) {
          draw_geometry_batch(batch, true, true);
        }
      }
    } else {
      for (const auto &projection : *frame.headlight_projections) {
        draw_headlight(projection);
      }
    }
    ID3D11ShaderResourceView *none = nullptr;
    context.get()->PSSetShaderResources(0U, 1U, &none);
    context.get()->OMSetRenderTargets(0U, nullptr, nullptr);
    render_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    return wrapped_texture;
  }
};

GlideRenderer::GlideRenderer() : impl_(std::make_unique<Impl>()) {}
GlideRenderer::~GlideRenderer() = default;

bool GlideRenderer::initialize(SDL_Renderer *renderer) {
  if (impl_->initialized) {
    return impl_->device && impl_->context;
  }
  impl_->initialized = true;
  if (renderer == nullptr) {
    return false;
  }
  const auto properties = SDL_GetRendererProperties(renderer);
  auto *device = static_cast<ID3D11Device *>(SDL_GetPointerProperty(
      properties, SDL_PROP_RENDERER_D3D11_DEVICE_POINTER, nullptr));
  if (device == nullptr) {
    return false;
  }
  device->AddRef();
  impl_->device.reset(device);
  ID3D11DeviceContext *context = nullptr;
  device->GetImmediateContext(&context);
  impl_->context.reset(context);
  impl_->create_static_resources();
  return true;
}

bool GlideRenderer::available() const noexcept {
  return impl_->device && impl_->context;
}

SDL_Texture *GlideRenderer::render(SDL_Renderer *renderer,
                                   const SceneFrameView &frame) {
  if (!available()) {
    return nullptr;
  }
  return impl_->render(renderer, frame);
}

double GlideRenderer::last_render_ms() const noexcept {
  return impl_->render_ms;
}

} // namespace mh::render::hardware
