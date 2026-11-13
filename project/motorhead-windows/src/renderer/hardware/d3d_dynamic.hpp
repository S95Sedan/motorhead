#pragma once

#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <d3dcompiler.h>

namespace mh::render::hardware {

HRESULT compile_shader(const void *source, SIZE_T source_size,
                       const char *source_name,
                       const D3D_SHADER_MACRO *defines,
                       ID3DInclude *include_handler, const char *entry,
                       const char *target, UINT flags1, UINT flags2,
                       ID3DBlob **code, ID3DBlob **errors) noexcept;

HRESULT serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC *description,
                                 D3D_ROOT_SIGNATURE_VERSION version,
                                 ID3DBlob **blob,
                                 ID3DBlob **errors) noexcept;

} // namespace mh::render::hardware
