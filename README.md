## Build and run

Install Visual Studio 2022 or Build Tools with **Desktop development with C++**, including the Windows SDK and C++/WinRT. Also install CMake 3.24 or later and the x64 Vulkan 1.3 or newer SDK with `glslc`. Make sure CMake is available on your terminal's PATH.

Open PowerShell or Command Prompt in the `prtxrtxpt` folder and run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The executable will be at `build\Release\prtxrtxpt.exe`.

## Load a map

1. Press **Esc** in the viewer. Paste the entire JSON into **Map code**.
2. Choose **Load map** or press **Ctrl+Enter**. The camera starts at the first valid spawn. Maps without a spawn use an overview calculated from their bounds or first occupied chunk.
3. If the code is invalid, the menu displays the error.

## Flight controls

| Input | Action |
| --- | --- |
| W / S | Fly forward / backward on the horizontal plane |
| A / D | Move left / right |
| E / Q | Move up / down |
| Hold Space | Fly faster |
| Esc | Open or close the map menu |
| F2 | Open or close rendering settings |
| Tab | Toggle diagnostics |
| F11 | Toggle fullscreen |

## Rendering setting Preset

| Quality preset | Internal scale per axis | Samples per pixel per frame | Total bounce limit | Traversal steps |
| --- | ---: | ---: | ---: | ---: |
| Low | 35% | 1 | 3 | 256 |
| Medium | 50% | 1 | 5 | 512 |
| High | 75% | 2 | 8 | 1024 |
| Ultra | 100% | 4 | 12 | 2048 |

The initial settings use 50% resolution, one sample per pixel, five total bounces and four diffuse bounces.