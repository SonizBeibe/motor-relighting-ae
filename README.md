# RelightFX

A native After Effects plugin (Windows, C++, AE SDK) that relights 2D manga/anime
line-art in real time: it derives a normal map from the artwork itself and shades
it against a movable 3D light rig, no manual painting or pretrained model required.

See [JULES_TASK.md](JULES_TASK.md) for the current implementation task and the
exact algorithm to build.

## Status

A first working version (`src/RelightFX`) compiles and runs in After Effects. It
computes a Sobel-based normal map directly from the layer's own ink lines and
shades it against a 3D light rig (`Light Position` / `Point of Interest`, same
convention as AE's own Light layers). Known limitation being worked on: pure
edge-based relief only bulges at ink lines, not across large flat regions
(a face, a plain jacket) — see JULES_TASK.md for the fix in progress.

## Requirements

- Windows 10/11
- Visual Studio 2022 or newer, "Desktop development with C++" workload
- The Adobe After Effects SDK (not included in this repo — proprietary, download
  from https://developer.adobe.com/console/servicesandapis/ae and unzip anywhere)
- OpenCV (for `cv::CascadeClassifier`, `cv::distanceTransform`, `cv::Sobel`,
  `cv::GaussianBlur` — see JULES_TASK.md). Not included in this repo.
- After Effects 2026 (or any version whose SDK matches) to test the compiled
  `.aex`

## Building

1. Download the After Effects SDK and note its path.
2. Open `src/RelightFX/Win/RelightFX.vcxproj` and update the `AE_SDK_DIR` user
   macro (currently hardcoded to the original author's machine) to point at
   your own SDK's `Examples` folder.
3. Set the `AE_PLUGIN_BUILD_DIR` environment variable to wherever you want the
   compiled `.aex` to land, e.g.:
   ```
   set AE_PLUGIN_BUILD_DIR=C:\path\to\this\repo\build
   ```
4. Build `src/RelightFX/Win/RelightFX.sln` (Debug|x64 or Release|x64) with
   MSBuild or Visual Studio.
5. Copy the resulting `RelightFX.aex` into After Effects' plugin folder, e.g.:
   ```
   "<After Effects install>\Support Files\Plug-ins\"
   ```
   (this usually needs administrator rights)
6. Restart After Effects. The effect appears under the "Anime Relight"
   category in Effects & Presets.

## Project layout

- `src/RelightFX/` — plugin source (`.cpp`/`.h`), the PiPL resource, and the
  Win32 project/solution.
- `tools/` — scratch space for research spikes (gitignored; not part of the
  shipped plugin).
