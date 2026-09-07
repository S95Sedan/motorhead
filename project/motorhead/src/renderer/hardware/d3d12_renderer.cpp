#include <renderer/hardware/d3d12_renderer.hpp>
#include <renderer/hardware/d3d_dynamic.hpp>

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
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mh::render::hardware {
namespace {

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
  float4 position : POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
  float4 fog : TEXCOORD1;
  float4 fog_parameters : TEXCOORD2;
};
struct VSOut {
  float4 position : SV_Position;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
  float4 fog : TEXCOORD1;
  float view_depth : TEXCOORD2;
  float4 fog_parameters : TEXCOORD3;
};
VSOut vs_main(VSIn input) {
  VSOut output;
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
float4 ps_opaque(VSOut input) : SV_Target {
  float4 result = scene_texture.Sample(scene_sampler, input.uv) * input.color;
  const float4 fog = pixel_fog(input);
  result.rgb = lerp(result.rgb, fog.rgb, saturate(fog.a));
  clip(result.a - 0.5);
  return float4(result.rgb, 1.0);
}
float4 ps_alpha(VSOut input) : SV_Target {
  float4 result = scene_texture.Sample(scene_sampler, input.uv) * input.color;
  const float4 fog = pixel_fog(input);
  result.rgb = lerp(result.rgb, fog.rgb, saturate(fog.a));
  clip(result.a - 0.00001);
  return result;
}
float4 ps_additive(VSOut input) : SV_Target {
  float4 result = scene_texture.Sample(scene_sampler, input.uv) * input.color;
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
  return float4((150.0 / 255.0).xxx * intensity, 1.0);
}
)";

struct Vertex {
  float position[4]{};
  float color[4]{};
  float uv[2]{};
  float fog[4]{};
  float fog_parameters[4]{};
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
  SDL_Texture *wrapped_color = nullptr;
  UINT color_width = 0U;
  UINT color_height = 0U;
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
  UINT next_descriptor = 2U;
  std::optional<bool> sampler_trilinear;
  DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
  std::array<ComPtr<ID3D12PipelineState>, 9U> pipelines;
  TextureResource white;
  std::unordered_map<const std::vector<mh::content::PamRgbaImage> *,
                     TextureResource> textures;
  std::vector<Vertex> vertices;
  std::vector<Batch> batches;
  std::vector<ProjectionDraw> projections;
  double render_ms = 0.0;

  ~Impl() {
    wait();
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
    std::array<D3D12_ROOT_PARAMETER, 2U> parameters{};
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
    rtv.NumDescriptors = 1U;
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
    constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 5U> layout{{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 0U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 16U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 32U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 1U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 40U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 2U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 56U,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U}}};
    auto base = D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
    base.pRootSignature = root_signature.get();
    base.VS = {vs.get()->GetBufferPointer(), vs.get()->GetBufferSize()};
    base.PS = {ps_opaque.get()->GetBufferPointer(), ps_opaque.get()->GetBufferSize()};
    base.BlendState.AlphaToCoverageEnable = FALSE;
    base.BlendState.IndependentBlendEnable = FALSE;
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
    base.NumRenderTargets = 1U;
    base.RTVFormats[0] = format;
    base.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    base.SampleDesc.Count = 1U;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[0].put())),
               "create opaque D3D12 scene pipeline");
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
    target.BlendEnable = FALSE;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[5].put())),
               "create shadow D3D12 projection pipeline");
    base.PS = {ps_headlight.get()->GetBufferPointer(),
               ps_headlight.get()->GetBufferSize()};
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ONE;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    require_hr(device.get()->CreateGraphicsPipelineState(
                   &base, IID_PPV_ARGS(pipelines[6].put())),
               "create headlight D3D12 projection pipeline");
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
        static_cast<float>(frame.width), static_cast<float>(frame.height),
        view.center_x, static_cast<float>(view.near_plane)};
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
         {minimum_x, minimum_y, maximum_x + 1, maximum_y + 1},
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
            source.position.x, source.position.y,
            static_cast<float>(command.view_depths[index]),
            frame.tron_hidden_line ? SDL_FColor{0.0F, 0.0F, 0.5F, 1.0F}
                                   : command.vertices[index].color,
            source.tex_coord.x, source.tex_coord.y, frame));
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
  impl_->ensure_color(renderer, static_cast<UINT>(frame.width),
                      static_cast<UINT>(frame.height));
  impl_->create_pipelines(DXGI_FORMAT_R8G8B8A8_UNORM);
  impl_->ensure_depth(static_cast<UINT>(frame.width),
                      static_cast<UINT>(frame.height));
  transition(impl_->list.get(), impl_->color.get(),
             impl_->color_first_use ? D3D12_RESOURCE_STATE_COPY_DEST
                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto rtv = impl_->rtv_heap.get()->GetCPUDescriptorHandleForHeapStart();
  impl_->device.get()->CreateRenderTargetView(impl_->color.get(), nullptr, rtv);
  auto dsv = impl_->dsv_heap.get()->GetCPUDescriptorHandleForHeapStart();
  impl_->list.get()->OMSetRenderTargets(1U, &rtv, FALSE, &dsv);
  constexpr std::array<float, 4U> transparent{0.0F, 0.0F, 0.0F, 0.0F};
  impl_->list.get()->ClearRenderTargetView(rtv, transparent.data(), 0U, nullptr);
  impl_->list.get()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F,
                                            0U, 0U, nullptr);
  D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(frame.width),
                          static_cast<float>(frame.height), 0.0F, 1.0F};
  D3D12_RECT scissor{0, 0, frame.width, frame.height};
  impl_->list.get()->RSSetViewports(1U, &viewport);
  impl_->list.get()->RSSetScissorRects(1U, &scissor);
  impl_->list.get()->SetGraphicsRootSignature(impl_->root_signature.get());
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
    impl_->list.get()->OMSetRenderTargets(1U, &rtv, FALSE, &dsv);
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
      if (impl_->projections[index].headlight) draw_projection(index);
    }
  }
  transition(impl_->list.get(), impl_->color.get(),
             D3D12_RESOURCE_STATE_RENDER_TARGET,
             impl_->color_first_use ? D3D12_RESOURCE_STATE_COPY_DEST
                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  impl_->submit();
  impl_->color_first_use = false;
  impl_->render_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  return impl_->wrapped_color;
}

double D3D12Renderer::last_render_ms() const noexcept {
  return impl_->render_ms;
}

} // namespace mh::render::hardware
