#include <renderer/hardware/d3d_dynamic.hpp>

#include <SDL3/SDL.h>

#include <array>

namespace mh::render::hardware {
namespace {

using CompileShader = HRESULT(WINAPI *)(
    const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *,
    const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

template <typename Function, std::size_t Count>
Function load_function(const std::array<const char *, Count> &libraries,
                       const char *name) noexcept {
  for (const auto *library : libraries) {
    if (SDL_SharedObject *module = SDL_LoadObject(library); module != nullptr) {
      if (SDL_FunctionPointer function = SDL_LoadFunction(module, name);
          function != nullptr) {
        return reinterpret_cast<Function>(function);
      }
      SDL_UnloadObject(module);
    }
  }
  return nullptr;
}

} // namespace

HRESULT compile_shader(const void *source, const SIZE_T source_size,
                       const char *source_name,
                       const D3D_SHADER_MACRO *defines,
                       ID3DInclude *include_handler, const char *entry,
                       const char *target, const UINT flags1,
                       const UINT flags2, ID3DBlob **code,
                       ID3DBlob **errors) noexcept {
  static const auto function = load_function<CompileShader>(
      std::array{"D3DCompiler_47.dll", "D3DCompiler_46.dll",
                 "D3DCompiler_45.dll", "D3DCompiler_44.dll",
                 "D3DCompiler_43.dll"},
      "D3DCompile");
  return function != nullptr
             ? function(source, source_size, source_name, defines,
                        include_handler, entry, target, flags1, flags2, code,
                        errors)
             : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

HRESULT serialize_root_signature(
    const D3D12_ROOT_SIGNATURE_DESC *description,
    const D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob,
    ID3DBlob **errors) noexcept {
  static const auto function = load_function<
      PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
      std::array{"d3d12.dll"}, "D3D12SerializeRootSignature");
  return function != nullptr ? function(description, version, blob, errors)
                             : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

} // namespace mh::render::hardware
