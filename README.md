# Aeon SR

A ReShade add-on that brings temporal anti-aliasing through NVIDIA DLSS, AMD FSR
and Intel XeSS to games that never shipped with them. It runs on every graphics
API ReShade supports, and motion vectors come from a built-in optical flow
estimator, so the game does not need to provide them.

The game keeps rendering at its own resolution. The quality modes below native
hand DLSS, FSR and XeSS less of the frame to work from; they do not make the game
run faster.

Author: **Barbatos AWLS**

## Features

- **Temporal anti-aliasing** through NVIDIA DLSS, AMD FSR (FSR 4 where the GPU
  supports it, FSR 3.1 otherwise) and Intel XeSS
- **FSR 4.1.2 Lite**: AMD's lighter FSR 4 model, on any Direct3D 12 GPU with Shader
  Model 6.6, with a library from AMD's driver you supply
- **Built-in optical flow** that produces the motion vectors DLSS, FSR and XeSS need
- **No native motion vectors required** from the game
- **Sub-pixel jitter** drawn into the game's own rendering
- **Every graphics API**: DirectX 8, 9, 10, 11 and 12, OpenGL and Vulkan
- **32-bit and 64-bit games**
- **The panel in your language**: it follows the language chosen in ReShade, in all
  sixteen languages ReShade offers
- **Frame generation** (experimental): Aeon SR's own generator, AMD FSR, Intel XeSS or
  NVIDIA DLSS Frame Generation, shown in a window of its own over the game
- **Low latency**: Aeon SR's own mode or NVIDIA Reflex, on every graphics API
- **Even frame pacing** for games that run faster than the screen
- **The game's own render scale**, used for real in DirectX 10 and 11 games that have one
- **DLSS Neural Rendering** as an optional pass, with a runtime you supply
- **Let OptiScaler upscale**: with OptiScaler in a 64-bit game, Aeon SR's DLSS call
  can go to OptiScaler, which runs the upscaler picked in its own menu
- **RTX Video Super Resolution** as an experiment: a spatial upscaler, on every
  graphics API, that rarely improves on the temporal ones

## Download

Ready-to-install packages are on the [Releases](../../releases) page. Each one
holds the add-on, `AeonSR.addonfx`, `AeonSRHost.exe`, `AeonSRPrebuild.exe`, `ngxshim\`, `runtime\`
with the vendor DLLs listed below, and `Licenses\` with every vendor's licence.

## Installing

1. Install ReShade **with add-on support** for the game.
2. Copy the add-on (`AeonSR.addon64`, or `AeonSR.addon32` for a 32-bit game),
   `AeonSR.addonfx`, `AeonSRHost.exe`, `AeonSRPrebuild.exe`, `ngxshim\` and `runtime\`
   next to the game's executable.
3. Open the ReShade overlay and configure Aeon SR in its Add-ons tab.

Do not use Aeon SR in games protected by anti-cheat.

## Runtimes

Aeon SR drives the vendors' own runtime DLLs. They are not part of Aeon SR and
keep their own licences (see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)).
None of them is committed to this repository: each vendor SDK is a git submodule
pinned to an official release, and the build copies the DLLs from there. Microsoft's
Direct3D 12 core comes from its NuGet package through `tools/fetch_agility_sdk.ps1`
(see [Building](#building)).

| Runtime | File | Source | In the release package |
| --- | --- | --- | --- |
| NVIDIA DLSS Super Resolution | `nvngx_dlss.dll` | [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) v310.9.1 | yes |
| AMD FSR | `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_loader_dx12.dll` | [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v2.3.0, AMD-signed | yes |
| Intel XeSS | `libxess.dll`, `libxess_dx11.dll` | [Intel XeSS SDK](https://github.com/intel/xess) v3.0.2 | yes |
| NVIDIA DLSS Frame Generation | `nvngx_dlssg.dll` | [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) v310.9.1 | yes |
| AMD FSR Frame Generation | `amd_fidelityfx_framegeneration_dx12.dll` | [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v2.3.0, AMD-signed | yes |
| Intel XeSS Frame Generation | `libxess_fg.dll`, `libxell.dll` | [Intel XeSS SDK](https://github.com/intel/xess) v3.0.2 | yes |
| Microsoft Direct3D 12 Agility SDK core | `D3D12Core.dll` | [Microsoft.Direct3D.D3D12](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.0) 1.619.0, Microsoft-signed | yes |
| NVIDIA DLSS Neural Rendering | `nvngx_dlssnr.dll` | none official | **no**, see below |
| AMD FSR 4.1.2 Lite | `amdxcffx64.dll` | AMD's graphics driver (FidelityFX 2.4.0), AMD-signed | **no**, see below |

### DLSS Neural Rendering

`nvngx_dlssnr.dll` is not in the DLSS SDK and there is no official channel to
redistribute it, so neither the repository nor the releases carry it. Everything
else works without it; the neural rendering pass stays off until the file is
present. To enable it, place a copy you obtained yourself in `runtime/` next to
the add-on (or in `runtime/dlss5-allgpu/`, which is searched first).

### FSR 4.1.2 Lite

`amdxcffx64.dll` is part of AMD's graphics driver, not of the FidelityFX SDK, so
neither the repository nor the releases carry it. With a copy you obtained yourself
in `runtime/` next to the add-on, FSR's version row offers 4.1.2 Lite and FSR runs it
by default; without it, FSR runs as before. Only version 2.4.0 of the library
(FSR 4.1.2) is accepted.

## Community runtimes (at your own risk)

Aeon SR is built and tested with the official runtimes above. Some users choose
community builds of these runtimes, for example to run FSR 4 on GPUs AMD's own
build does not cover. Aeon SR does not distribute them, and a release never
contains one. If you choose to use one, it comes from your own source and is your
own risk: such builds are not made, signed or supported by NVIDIA or AMD, are not
supported by this project, and may be flagged by antivirus or anti-cheat software.

A file placed in one of these folders is used in place of the official runtime,
which stays untouched as the fallback:

| Runtime | Folder |
| --- | --- |
| FSR 4 community build | `runtime/fsr4-int8/amd_fidelityfx_upscaler_dx12.dll` |
| DLSS Neural Rendering for RTX 20/30/40 | `runtime/dlss5-allgpu/nvngx_dlssnr.dll` |

Removing the file from that folder returns to the official runtime.

## Architecture

```mermaid
flowchart TB
    jitter["Jitter<br/>sub-pixel offset"] -. "drawn into the game's rendering" .-> game
    game["Game frame<br/>D3D8 to D3D12, OpenGL, Vulkan"] -- "colour, depth" --> bridge["Bridge<br/>one per graphics API"]
    bridge --> engine
    subgraph engine["Engine device (Direct3D 12)"]
        direction LR
        flow["Optical flow<br/>motion vectors"] --> up["Anti-aliasing<br/>DLSS / FSR / XeSS"]
        depth["Depth<br/>normalised"] --> up
        up -.-> nr["DLSS Neural Rendering<br/>optional"]
    end
    engine -- "back through the bridge" --> out["Game's back buffer<br/>anti-aliased frame"]
```

- **One engine, one bridge per API.** FSR 4 and DLSS Neural Rendering exist only
  for Direct3D 12, so all the work runs on a single Direct3D 12 engine device on
  the game's GPU. In a Direct3D 12 game the engine is the game's own device and
  nothing is copied. On every other API a bridge shares the game's colour and
  depth with the engine (shared textures on Direct3D 9 to 11, external memory on
  OpenGL and Vulkan) and returns the finished frame. A DirectX 8 game reaches
  ReShade as DirectX 9 through d3d8to9.
- **Motion vectors.** The optical flow estimator runs on the engine device and
  produces dense motion vectors from the colour buffer, with a camera model fitted
  through depth for thin geometry such as wires and fences.
- **Jitter.** The sub-pixel offset DLSS, FSR and XeSS need is drawn into the game's own
  rendering (its shaders, its projection or its viewport, depending on the API).
  The add-on then tries to put the interface back as the game drew it. How well
  that works depends on the game: in some, Earth Defense Force 6 among them, the
  interface can still shake with the jitter.
- **32-bit games.** The vendor runtimes are 64-bit only. `AeonSR.addon32` hands
  the frame to `AeonSRHost.exe`, a 64-bit process that holds the engine and runs
  DLSS, FSR, XeSS, RTX VSR, DLSS Neural Rendering and the vendors' frame generation.
- **RTX Video Super Resolution** is an experiment. It runs on a Direct3D 11 device
  of the add-on's own, so it works on every graphics API, needs no motion vectors
  and, being spatial, rarely improves on DLSS, FSR and XeSS.
- **`runtime\D3D12Core.dll`** is Microsoft's Direct3D 12 Agility SDK core. FSR 4
  needs Shader Model 6.6, which the core Windows 10 ships does not offer, so where
  the system's core lacks it the engine device is made on this one instead
  ([docs/agility-core.md](docs/agility-core.md)). Windows 11 keeps its own core.
- **`ngxshim\nvngx.dll`** is a small forwarder through which the add-on loads the
  DLSS Neural Rendering runtime. It sits in its own folder so it is never mistaken
  for NVIDIA's NGX core.
- **`AeonSRPrebuild.exe`** builds FSR 4's shader pipelines in a separate process,
  so a new configuration's driver compile does not stall the game.

The code for each part is under its module in `src/`, listed in
[Source layout](#source-layout).

## Building

Requirements: Windows, Visual Studio 2022 (or Build Tools) with the C++ workload,
CMake 3.20+ and Git.

```powershell
git clone https://github.com/BarbatosAWLS/AeonSR.git
cd AeonSR
git submodule update --init --depth 1
git -C external/reshade submodule update --init --depth 1 deps/imgui deps/minhook deps/spirv
powershell -ExecutionPolicy Bypass -File tools\fetch_agility_sdk.ps1
cmake -S . -B build\x64 -G "Visual Studio 17 2022" -A x64
cmake --build build\x64 --config Release
cmake -S . -B build\x86 -G "Visual Studio 17 2022" -A Win32
cmake --build build\x86 --config Release
```

The submodules under `external/` are ReShade (headers, ImGui, MinHook, the effect
compiler), the NVIDIA DLSS SDK, the Intel XeSS SDK, the AMD FidelityFX SDK and the
Vulkan headers. The FidelityFX API and DX11 headers, the OpenGL headers and the
XeSS declarations the add-on compiles against are in `third_party/`.
`tools\fetch_agility_sdk.ps1` downloads Microsoft's `D3D12Core.dll` into
`external\D3D12Core\` and checks its signature and SHA-256; without it the build
still succeeds, and FSR 4 is then refused on Windows 10.

The x64 build produces, in `build\x64\Release\`, `AeonSR.addon64`,
`AeonSR.addonfx`, `AeonSRHost.exe` (the 64-bit process that runs DLSS, FSR and XeSS for 32-bit
games), `ngxshim\nvngx.dll` and `runtime\` with the vendor DLLs. The Win32 build
produces `AeonSR.addon32`; a 32-bit game also needs `AeonSRHost.exe` and
`runtime\` from the x64 build next to it.

`cmake --install build\x64 --config Release --prefix package` followed by the
same for `build\x86` assembles the release layout, `Licenses\` included.

## Source layout

The add-on lives in `src/<module>/`, its headers in `include/aeon_sr/<module>/`:

| Module | What it holds |
| --- | --- |
| `core` | Entry point, `App`, settings, the panel, diagnostics, the frame plan |
| `upscalers` | The DLSS, FSR, XeSS and RTX VSR backends, their loaders, upscaler capture |
| `ngx` | NGX runtime and sessions, the neural pass, the NGX shim |
| `interop` | The bridge for each graphics API, the engine device, the link to `AeonSRHost.exe` |
| `motion` | The optical flow estimator and its shaders |
| `depth` | Depth normalisation, depth for Direct3D 9 and Vulkan |
| `jitter` | Sub-pixel jitter drawn into the game, its hooks, the interface restore |
| `framegen` | Frame generation: Aeon SR's own and the vendors', pacing, the window it shows in |
| `games` | What Aeon SR knows of particular games (Project DIVA Mega Mix+'s render scale) |

`host/` is `AeonSRHost.exe` and `prebuild/` is `AeonSRPrebuild.exe`.

## Licence

Aeon SR is released under the [MIT licence](LICENSE). Third-party components
are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
