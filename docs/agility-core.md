# The engine device and the Direct3D 12 core

Aeon SR runs its upscalers on a Direct3D 12 device of its own, whatever API the game uses. FSR 4's
model is SM 6.6 compute: 8-bit integer dot products, 16-bit types, wave operations. The core that
Windows 10 ships does not offer that, and FSR 4 faults inside `D3D12Core.dll` on its first dispatch.

Measured on Windows 10 19044, RX 6600 XT, driver 32.0.21045.5002 (a probe against both cores, same
adapter, same process):

| Device made through | Highest shader model |
|---|---|
| `D3D12CreateDevice` (the core in `System32`) | 6.5 |
| `ID3D12DeviceFactory::CreateDevice` on the Agility core 1.619 | **6.6** |

So the fix is not a file to copy next to the game: a `D3D12Core.dll` in the folder is inert unless
the caller asks for it. Windows loads another core only when

- the executable exports `D3D12SDKVersion` and `D3D12SDKPath` (a game's business, not ours), or
- the code calls `D3D12GetInterface(CLSID_D3D12SDKConfiguration)`, takes
  `ID3D12SDKConfiguration1::CreateDeviceFactory(version, path, …)`, and creates the device on the
  returned `ID3D12DeviceFactory`.

Aeon SR now does the second, in `include/aeon_sr/interop/agility_core.hpp`, for the engine device
(`EngineDevice::finish_device`) and for `AeonSRPrebuild.exe`, which builds FSR's pipelines outside the
game and crashed the same way. When no core is bundled, both fall back to the core of the OS exactly
as before, so nothing changes on a machine that has none.

## Shipping the core

`tools\fetch_agility_sdk.ps1` fetches `D3D12Core.dll` (Microsoft.Direct3D.D3D12 1.619.x, signed by
Microsoft, redistributable under that package's licence) into `external\D3D12Core`. The build copies
it to `runtime\` beside the add-on, which is the first folder the device creation looks in.

The build does not require it: without the file, FSR 4 is refused on Windows 10 and FSR 3.1 is
unaffected. The log says which core the engine device was made on.

## What this does not cover

A Direct3D 12 game that hands Aeon SR its own device: that device belongs to the game and is on
whatever core the game asked for. FSR 4 needs a game whose device reports SM 6.6, or the engine has
to be bridged instead of borrowed.
