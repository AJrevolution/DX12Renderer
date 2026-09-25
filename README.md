# Modern DX12 Renderer

A personal C++ and HLSL renderer built with DirectX 12 and DXR. It includes rasterised and ray-traced rendering paths, denoising, and ReSTIR environment lighting.

For screenshots and technical walkthroughs, see the [portfolio project page](https://portfolio.arjannjanda.workers.dev/projects/dx12-renderer/).

## Build and run

Open `DX12Renderer.sln` in Visual Studio 2022 on Windows, select **Debug | x64**, then build and run. This is the supported configuration; Release is not currently supported. The ray-traced path requires a DXR-capable GPU.

Scenes are configured through `Assets/Scenes/default_scene.json`. The manifest selects models and sets the camera, lighting and environment. Press **F1** to reload supported scene settings after editing it.

## Controls

| Key | Action |
| --- | --- |
| **F** | Switch between free-roam and orbit camera controls |
| **WASD** | Move in free-roam mode; orbit and zoom in orbit mode |
| **Mouse** | Look around in free-roam mode |
| **O** | Toggle automatic orbit |
| **F9** | Toggle ray tracing |
| **0** | Return to the final shaded view |
| **F6 / F7** | Previous / next debug view |
| **F8** | List available debug views in the Visual Studio Output window |
| **R** | Toggle ray-tracing accumulation |
| **V** | Cycle ReSTIR validation mode |
| **M** | Cycle ReSTIR metrics capture point |
| **Q** | Print the current camera as manifest JSON |
