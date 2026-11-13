get_filename_component(MH_WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(MH_LLVM_MINGW_ROOT "${MH_WORKSPACE_ROOT}/.tools/llvm-mingw")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER "${MH_LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang.exe")
set(CMAKE_CXX_COMPILER "${MH_LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++.exe")
set(CMAKE_RC_COMPILER "${MH_LLVM_MINGW_ROOT}/bin/llvm-windres.exe")
