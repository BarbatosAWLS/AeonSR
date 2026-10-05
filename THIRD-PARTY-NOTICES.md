# Third-party notices

Aeon SR is released under the MIT licence (see `LICENSE`). It uses the
components below, which are not part of Aeon SR and remain under their own
terms. No vendor binary is committed to this repository: the vendor SDKs are git
submodules under `external/`, pinned to official releases, and the release
packages copy each vendor's licence into `Licenses/`.

---

## AMD FidelityFX: `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_loader_dx12.dll`

Shipped in the release packages, unmodified. Both files are AMD's own signed
binaries from the AMD FidelityFX SDK v2.3.0 (submodule `external/FidelityFX-SDK`,
`Kits/FidelityFX/signedbin/`,
<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.3.0>). The
SDK's `docs/license.md`, packaged as `Licenses/AMD_FidelityFX_LICENSE.md`, lists
both files under the MIT licence below.

| File | SHA-256 (first 16) |
| --- | --- |
| `amd_fidelityfx_upscaler_dx12.dll` | `D0DCCCC74A43C44B` |
| `amd_fidelityfx_loader_dx12.dll` | `E2D85AA05A9BD9ED` |

The FidelityFX API headers in `third_party/ffx/include` and the FidelityFX DX11
headers in `third_party/ffx_dx11/include` (from
<https://github.com/optiscaler/FidelityFX-SDK-DX11>) are under the same licence
and keep AMD's notice in each file.

Copyright (c) Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in the
Software without restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Aeon SR also loads a modified `amd_fidelityfx_upscaler_dx12.dll` that a user
places in `runtime/fsr4-int8/`. Such builds (for example community FSR 4 INT8
builds) are not included, are not covered by the licence above, and are used at
the user's own risk.

## NVIDIA DLSS SDK: `nvngx_dlss.dll`, NGX headers, `nvsdk_ngx_s.lib`

From the NVIDIA DLSS SDK, <https://github.com/NVIDIA/DLSS> (tag `v310.9.1`),
submodule `external/DLSS`. The headers and `nvsdk_ngx_s.lib` are used to build the
add-on; `nvngx_dlss.dll` is shipped unmodified in the release packages as part of
Aeon SR. They are governed by the **NVIDIA RTX SDKs License** that accompanies the
SDK as `LICENSE.txt`, packaged as `Licenses/NVIDIA_DLSS_LICENSE.txt`.

This application uses **NVIDIA DLSS** and the **NVIDIA NGX SDK**. NVIDIA, NVIDIA
RTX and NVIDIA GeForce RTX are trademarks and/or registered trademarks of NVIDIA
Corporation in the United States and other countries.

## NVIDIA DLSS Neural Rendering: `nvngx_dlssnr.dll`

Not included in the repository or the release packages. It is not part of the NVIDIA DLSS SDK and NVIDIA
does not publish it for redistribution. Aeon SR works without it; the neural
rendering pass is enabled only when the user supplies the file, official or a
community build, at their own risk (see `README.md`).

`src/ngx/ngx_shim.cpp`, built as `ngxshim/nvngx.dll`, is Aeon SR's own code, not
NVIDIA's.

## Intel XeSS: `libxess.dll`, `libxess_dx11.dll`

Shipped unmodified in the release packages, from the Intel XeSS SDK,
<https://github.com/intel/xess> (tag `v3.0.2`), submodule `external/xess`.
Copyright (c) Intel Corporation, distributed under the **Intel Simplified Software
License (Version October 2022)**, packaged as `Licenses/Intel_XeSS_LICENSE.txt`.

The declarations in `third_party/xess/include` are Aeon SR's own, written against
the public XeSS API.

## ReShade

Aeon SR is a ReShade add-on. ReShade (<https://github.com/crosire/reshade>, BSD
3-Clause licence) is the submodule `external/reshade`, with its ImGui (MIT),
MinHook (BSD 2-Clause) and SPIR-V headers (MIT-style Khronos licence)
submodules. Aeon SR compiles ReShade's add-on headers, its effect compiler
sources and MinHook into the add-on.

## Khronos OpenGL and Vulkan headers

The Vulkan headers (<https://github.com/KhronosGroup/Vulkan-Headers>, tag
`v1.4.362`, Apache License 2.0) are the submodule `external/Vulkan-Headers`.
`GL/glext.h`, `GL/wglext.h` and `KHR/khrplatform.h` (from
<https://github.com/KhronosGroup/OpenGL-Registry> and
<https://github.com/KhronosGroup/EGL-Registry>, MIT, notice kept in each file)
are in `third_party/khronos/include`. Declarations only; every
OpenGL and Vulkan entry point is resolved at runtime.
