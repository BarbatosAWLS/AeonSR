cmake_minimum_required(VERSION 3.20)
project(AeonSR VERSION 1.0.1 LANGUAGES C CXX)

set(AEONSR_VERSION_STR "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "Aeon SR targets Windows only.")
endif()

set(AEONSR_EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(AEONSR_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(RESHADE_ROOT "${AEONSR_EXTERNAL}/reshade" CACHE PATH "ReShade source checkout")
set(AEONSR_DLSS_SDK "${AEONSR_EXTERNAL}/DLSS" CACHE PATH "NVIDIA DLSS SDK checkout")
set(AEONSR_XESS_SDK "${AEONSR_EXTERNAL}/xess" CACHE PATH "Intel XeSS SDK checkout")
set(AEONSR_FFX_SDK "${AEONSR_EXTERNAL}/FidelityFX-SDK" CACHE PATH "AMD FidelityFX SDK checkout")
set(FFX_INCLUDE_DIR "${AEONSR_THIRD_PARTY}/ffx/include" CACHE PATH "FidelityFX API headers")
set(FFX_DX11_INCLUDE_DIR "${AEONSR_THIRD_PARTY}/ffx_dx11/include" CACHE PATH "FidelityFX DX11 headers")
set(XESS_INCLUDE_DIR "${AEONSR_THIRD_PARTY}/xess/include" CACHE PATH "XeSS headers")
set(VULKAN_INCLUDE_DIR "${AEONSR_EXTERNAL}/Vulkan-Headers/include" CACHE PATH "Vulkan headers")
set(GL_INCLUDE_DIR "${AEONSR_THIRD_PARTY}/khronos/include")
set(VKGL_INCLUDE_DIR "${VULKAN_INCLUDE_DIR}" "${GL_INCLUDE_DIR}")
set(NGX_INCLUDE_DIR "${AEONSR_DLSS_SDK}/include")
set(NGX_LIB "${AEONSR_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")

set(AEONSR_RUNTIME_FILES
  "${AEONSR_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
  "${AEONSR_XESS_SDK}/bin/libxess.dll"
  "${AEONSR_XESS_SDK}/bin/libxess_dx11.dll"
  "${AEONSR_FFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
  "${AEONSR_FFX_SDK}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll")
set(AEONSR_LICENSE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE|AeonSR_LICENSE.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md|THIRD-PARTY-NOTICES.md"
  "${AEONSR_DLSS_SDK}/LICENSE.txt|NVIDIA_DLSS_LICENSE.txt"
  "${AEONSR_XESS_SDK}/LICENSE.txt|Intel_XeSS_LICENSE.txt"
  "${AEONSR_FFX_SDK}/docs/license.md|AMD_FidelityFX_LICENSE.md")
set(MINHOOK_DIR "${RESHADE_ROOT}/deps/minhook")

set(_aeonsr_hint "Run: git submodule update --init --depth 1 && git -C external/reshade submodule update --init --depth 1 deps/imgui deps/minhook deps/spirv")
foreach(_req
    "${RESHADE_ROOT}/include/reshade.hpp"
    "${RESHADE_ROOT}/deps/imgui/imgui.h"
    "${MINHOOK_DIR}/src/hook.c"
    "${NGX_INCLUDE_DIR}/nvsdk_ngx.h"
    "${NGX_LIB}"
    "${FFX_INCLUDE_DIR}/ffx_api.h"
    "${FFX_DX11_INCLUDE_DIR}/FidelityFX/host/ffx_fsr3upscaler.h"
    "${XESS_INCLUDE_DIR}/xess/xess.h"
    "${GL_INCLUDE_DIR}/GL/glext.h"
    "${VULKAN_INCLUDE_DIR}/vulkan/vulkan.h"
    ${AEONSR_RUNTIME_FILES})
  if(NOT EXISTS "${_req}")
    message(FATAL_ERROR "Missing ${_req}. ${_aeonsr_hint}")
  endif()
endforeach()

set(RESHADEFX_SOURCES
  "${RESHADE_ROOT}/source/effect_lexer.cpp"
  "${RESHADE_ROOT}/source/effect_expression.cpp"
  "${RESHADE_ROOT}/source/effect_symbol_table.cpp"
  "${RESHADE_ROOT}/source/effect_parser_exp.cpp"
  "${RESHADE_ROOT}/source/effect_parser_stmt.cpp"
  "${RESHADE_ROOT}/source/effect_codegen_spirv.cpp"
)
foreach(f IN LISTS RESHADEFX_SOURCES)
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "ReShade effect compiler source missing: ${f}")
  endif()
endforeach()

add_library(aeon_sr_minhook STATIC
  "${MINHOOK_DIR}/src/buffer.c"
  "${MINHOOK_DIR}/src/hook.c"
  "${MINHOOK_DIR}/src/trampoline.c"
  "${MINHOOK_DIR}/src/hde/hde32.c"
  "${MINHOOK_DIR}/src/hde/hde64.c")
target_include_directories(aeon_sr_minhook PUBLIC "${MINHOOK_DIR}/include")

set(AEONSR_SOURCES
  src/core/addon_main.cpp
  src/core/app.cpp
  src/upscalers/backend_dlss.cpp
  src/upscalers/backend_dlss_d3d11.cpp
  src/upscalers/backend_fsr_d3d11.cpp
  src/upscalers/backend_fsr_d3d12.cpp
  src/upscalers/fsr_warmup.cpp
  src/upscalers/backend_vsr.cpp
  src/upscalers/backend_xess.cpp
  src/interop/blit_d3d11.cpp
  src/interop/blit_d3d12.cpp
  src/interop/bridge_d3d10.cpp
  src/interop/bridge_d3d11.cpp
  src/interop/bridge_d3d9.cpp
  src/interop/bridge_opengl.cpp
  src/interop/bridge_staging.cpp
  src/interop/bridge_vulkan.cpp
  src/depth/d3d9_depth.cpp
  src/core/debug_view.cpp
  src/depth/depth_convention.cpp
  src/depth/depth_normalize.cpp
  src/depth/depth_on_grid.cpp
  src/core/diag_report.cpp
  src/core/diagnostics.cpp
  src/core/dxgi_format_util.cpp
  src/upscalers/ffx_dx11_loader.cpp
  src/upscalers/ffx_loader.cpp
  src/motion/flow_probe.cpp
  src/core/frame_clock.cpp
  src/core/frame_inputs.cpp
  src/core/frame_plan.cpp
  src/jitter/frame_shift.cpp
  src/core/frame_observer.cpp
  src/jitter/hud_restore.cpp
  src/interop/gpu12.cpp
  src/core/gpu_vendor.cpp
  src/motion/guide_builder.cpp
  src/core/input_probes.cpp
  src/interop/interop.cpp
  src/interop/interop_bridge_factory.cpp
  src/jitter/jitter.cpp
  src/motion/mv_probe.cpp
  src/motion/mv_stats.cpp
  src/upscalers/native_dlss.cpp
  src/ngx/neural_arch_hook.cpp
  src/ngx/neural_hardware.cpp
  src/ngx/ngx_common.cpp
  src/ngx/ngx_dlssnr.cpp
  src/ngx/ngx_runtime.cpp
  src/ngx/ngx_runtime_d3d12.cpp
  src/ngx/ngx_session.cpp
  src/motion/optical_flow.cpp
  src/core/panel.cpp
  src/interop/remote_engine.cpp
  src/interop/remote_protocol.cpp
  src/core/runtime_search.cpp
  src/jitter/scene_jitter.cpp
  src/jitter/scene_jitter_hooks.cpp
  src/jitter/scene_jitter_shaders.cpp
  src/jitter/shader_offset.cpp
  src/jitter/gl_jitter.cpp
  src/motion/scene_stability.cpp
  src/core/settings.cpp
  src/jitter/spirv_compile.cpp
  src/upscalers/upscaler_backend.cpp
  src/upscalers/upscaler_capture.cpp
  src/upscalers/upscaler_capture_format.cpp
  src/jitter/vulkan_viewport_hook.cpp
  src/upscalers/xess_loader.cpp
)

set(AEONSR_VENDOR_SOURCES
  src/upscalers/backend_dlss.cpp
  src/upscalers/backend_dlss_d3d11.cpp
  src/upscalers/backend_fsr_d3d11.cpp
  src/upscalers/backend_fsr_d3d12.cpp
  src/upscalers/fsr_warmup.cpp
  src/upscalers/backend_vsr.cpp
  src/upscalers/backend_xess.cpp
  src/depth/depth_on_grid.cpp
  src/upscalers/ffx_dx11_loader.cpp
  src/upscalers/ffx_loader.cpp
  src/ngx/neural_arch_hook.cpp
  src/ngx/ngx_dlssnr.cpp
  src/ngx/ngx_runtime.cpp
  src/ngx/ngx_runtime_d3d12.cpp
  src/ngx/ngx_session.cpp
  src/upscalers/xess_loader.cpp
)

set(AEONSR_COMMON_DEFINITIONS
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
  ImTextureID=ImU64 IMGUI_DISABLE_INCLUDE_IMCONFIG_H
  AEONSR_RESHADE_API_VERSION=18
  AEONSR_VERSION_STR="${AEONSR_VERSION_STR}")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(AEONSR_X64 ON)
  set(AEONSR_ADDON_EXT ".addon64")
  add_library(AeonSR SHARED ${AEONSR_SOURCES} ${RESHADEFX_SOURCES})
else()
  set(AEONSR_X64 OFF)
  set(AEONSR_ADDON_EXT ".addon32")
  list(REMOVE_ITEM AEONSR_SOURCES ${AEONSR_VENDOR_SOURCES})
  add_library(AeonSR SHARED ${AEONSR_SOURCES} ${RESHADEFX_SOURCES} src/upscalers/vendor_absent.cpp)
  target_compile_definitions(AeonSR PRIVATE AEONSR_NO_VENDOR=1)
endif()

set_target_properties(AeonSR PROPERTIES OUTPUT_NAME "AeonSR" PREFIX "" SUFFIX "${AEONSR_ADDON_EXT}")

target_include_directories(AeonSR PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${RESHADE_ROOT}/include"
  "${RESHADE_ROOT}/deps/imgui"
  "${NGX_INCLUDE_DIR}"
  "${FFX_INCLUDE_DIR}"
  "${FFX_DX11_INCLUDE_DIR}"
  "${XESS_INCLUDE_DIR}"
  ${VKGL_INCLUDE_DIR}
  "${RESHADE_ROOT}/source"
  "${RESHADE_ROOT}/deps/spirv/include/spirv/unified1")
target_compile_definitions(AeonSR PRIVATE ${AEONSR_COMMON_DEFINITIONS})
target_link_libraries(AeonSR PRIVATE aeon_sr_minhook
  d3d11 d3d12 d3d10 d3d9 dxgi d3dcompiler opengl32 gdi32 shell32 ole32)

if(AEONSR_X64)
  target_link_libraries(AeonSR PRIVATE "${NGX_LIB}")

  add_library(AeonSR_NgxShim SHARED src/ngx/ngx_shim.cpp)
  set_target_properties(AeonSR_NgxShim PROPERTIES OUTPUT_NAME "nvngx" PREFIX "")
  target_compile_definitions(AeonSR_NgxShim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

  add_executable(AeonSRHost WIN32
    host/host_main.cpp
    host/host_session.cpp
    src/upscalers/backend_dlss.cpp
    src/upscalers/backend_fsr_d3d12.cpp
    src/upscalers/fsr_warmup.cpp
    src/upscalers/backend_xess.cpp
    src/interop/blit_d3d12.cpp
    src/depth/depth_on_grid.cpp
    src/core/diagnostics.cpp
    src/core/dxgi_format_util.cpp
    src/upscalers/ffx_loader.cpp
    src/core/frame_inputs.cpp
    src/interop/gpu12.cpp
    src/core/gpu_vendor.cpp
    src/interop/interop.cpp
    src/ngx/neural_arch_hook.cpp
    src/ngx/neural_hardware.cpp
    src/ngx/ngx_common.cpp
    src/ngx/ngx_dlssnr.cpp
    src/ngx/ngx_runtime_d3d12.cpp
    src/ngx/ngx_session.cpp
    src/interop/remote_protocol.cpp
    src/core/runtime_search.cpp
    src/core/settings.cpp
    src/upscalers/upscaler_backend.cpp
    src/upscalers/xess_loader.cpp
  )
  target_include_directories(AeonSRHost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${RESHADE_ROOT}/include"
    "${RESHADE_ROOT}/deps/imgui"
    "${NGX_INCLUDE_DIR}"
    "${FFX_INCLUDE_DIR}"
    "${FFX_DX11_INCLUDE_DIR}"
    "${XESS_INCLUDE_DIR}"
    ${VKGL_INCLUDE_DIR})
  target_compile_definitions(AeonSRHost PRIVATE ${AEONSR_COMMON_DEFINITIONS}
    AEONSR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    AEONSR_VERSION_MINOR=${PROJECT_VERSION_MINOR})
  target_link_libraries(AeonSRHost PRIVATE
    d3d11 d3d12 dxgi d3dcompiler shell32 ole32 user32 advapi32 "${NGX_LIB}")

  add_executable(AeonSRPrebuild WIN32
    prebuild/prebuild_main.cpp
    src/upscalers/fsr_warmup.cpp
    src/upscalers/ffx_loader.cpp
  )
  target_include_directories(AeonSRPrebuild PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${FFX_INCLUDE_DIR}")
  target_compile_definitions(AeonSRPrebuild PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(AeonSRPrebuild PRIVATE d3d12 dxgi shell32 delayimp)
  target_link_options(AeonSRPrebuild PRIVATE /DELAYLOAD:dxgi.dll /DELAYLOAD:d3d12.dll)

  add_dependencies(AeonSR AeonSR_NgxShim AeonSRHost AeonSRPrebuild)
  set(AEONSR_TARGETS AeonSR AeonSR_NgxShim AeonSRHost AeonSRPrebuild)
else()
  set(AEONSR_TARGETS AeonSR)
endif()

if(MSVC)
  set_property(TARGET aeon_sr_minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  foreach(t IN LISTS AEONSR_TARGETS)
    target_compile_options(${t} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endforeach()
endif()

set(AEONSR_OUT "$<TARGET_FILE_DIR:AeonSR>")
if(AEONSR_X64)
  set(_aeonsr_copy_runtime)
  foreach(f IN LISTS AEONSR_RUNTIME_FILES)
    list(APPEND _aeonsr_copy_runtime COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "${AEONSR_OUT}/runtime/")
  endforeach()
  add_custom_command(TARGET AeonSR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AEONSR_OUT}/ngxshim" "${AEONSR_OUT}/runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:AeonSR_NgxShim>" "${AEONSR_OUT}/ngxshim/nvngx.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:AeonSRHost>" "${AEONSR_OUT}/AeonSRHost.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:AeonSRPrebuild>" "${AEONSR_OUT}/AeonSRPrebuild.exe"
    ${_aeonsr_copy_runtime}
    COMMENT "Placing the NGX shim, AeonSRHost.exe, AeonSRPrebuild.exe and the vendor runtimes beside the add-on")
endif()

install(FILES "$<TARGET_FILE:AeonSR>" DESTINATION .)
foreach(entry IN LISTS AEONSR_LICENSE_FILES)
  string(REPLACE "|" ";" pair "${entry}")
  list(GET pair 0 src)
  list(GET pair 1 name)
  install(FILES "${src}" DESTINATION Licenses RENAME "${name}")
endforeach()
if(AEONSR_X64)
  install(FILES "$<TARGET_FILE:AeonSRHost>" DESTINATION .)
  install(FILES "$<TARGET_FILE:AeonSRPrebuild>" DESTINATION .)
  install(FILES "$<TARGET_FILE:AeonSR_NgxShim>" DESTINATION ngxshim)
  install(FILES ${AEONSR_RUNTIME_FILES} DESTINATION runtime)
endif()

function(aeonsr_version_resource target filename description filetype)
  if(NOT TARGET ${target})
    return()
  endif()
  set(AEONSR_RC_FILENAME "${filename}")
  set(AEONSR_RC_DESCRIPTION "${description}")
  set(AEONSR_RC_FILETYPE "${filetype}")
  set(rc "${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.rc.in" "${rc}" @ONLY)
  target_sources(${target} PRIVATE "${rc}")
endfunction()
aeonsr_version_resource(AeonSR "AeonSR${AEONSR_ADDON_EXT}" "Aeon SR upscaling add-on for ReShade" VFT_DLL)
aeonsr_version_resource(AeonSR_NgxShim "nvngx.dll" "Aeon SR forwarder for the DLSS neural rendering runtime" VFT_DLL)
aeonsr_version_resource(AeonSRHost "AeonSRHost.exe" "Aeon SR engine host for 32-bit games" VFT_APP)
aeonsr_version_resource(AeonSRPrebuild "AeonSRPrebuild.exe" "Aeon SR shader pipeline prebuild" VFT_APP)
