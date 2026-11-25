## Build and run

Install Visual Studio 2022 or Build Tools with **Desktop development with C++**, including the Windows SDK and C++/WinRT. Also install CMake 3.24 or later and the x64 Vulkan SDK with `glslc`. Make sure CMake is available on your terminal's PATH.

Open PowerShell or Command Prompt in the `prtxrtxpt` folder and run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The executable will be at `build\Release\prtxrtxpt.exe`. Run it with:

```powershell
.\build\Release\prtxrtxpt.exe
```
If CMake cannot locate Vulkan, set `$env:VULKAN_SDK` to the installed SDK directory containing `Include`, `Lib`, and `Bin`, then repeat the configure command.


## Load a map

1. Press **Esc** in the viewer. Paste the entire JSON export into **Map code**.
2. Choose **Load map** or press **Ctrl+Enter**. The camera starts at the first valid spawn. Maps without a spawn use an overview calculated from their bounds or first occupied chunk.
3. If the code is invalid, the menu displays the error.

## Flight controls

| Input | Action |
| --- | --- |
| W / S | Fly forward / backward on the horizontal plane |
| A / D | Move left / right |
| E / Q | Move up / down |
| Hold Space | Fly faster |
| Esc | Open or Close the menu inside the viewer |
| Tab during viewing | Toggle rendered FPS in the title bar |
| F11 | Toggle Fullscreen |
