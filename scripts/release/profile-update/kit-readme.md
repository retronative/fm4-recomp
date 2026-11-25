# FM4 Recomp 0.3.1 Beta

Open **FM4 Recomp.app** to play. On first use, the setup assistant can install your own Forza Motorsport 4 USA Disc 1 ISO. This build requires Apple silicon and macOS 26 or later. The app uses your FM4 Recomp production profile. To keep another installation alongside it, copy the app into its own folder.

Open **Pause → GRAPHICS** during a single-player race, or press **Command-comma (⌘,)**. On a controller, press **Up** from the first setting to reach the tabs. **Left** and **Right** switch tabs there. Press **Down** or **A** to enter the selected tab. In the settings list, **Up** and **Down** select a row and **Left** and **Right** change its value.

Press **Y (Save & Apply)** to use your choices. Browsing resolutions keeps the current picture on screen. The **Changing resolution…** indicator waits for a new frame at the requested size. The Graphics page stays open so you can choose when to resume.

| Setting | What it changes |
| --- | --- |
| Display Mode | Switch between fullscreen and a window. |
| Resolution | Choose the game's render size. The choices are 720p, 1080p (Full HD), 1440p (QHD) and 4K (2160p). A new profile starts at Full HD. You can choose 4K even on a smaller screen. |
| MSAA | Smooths the edges of 3D objects. Game uses 2× in races and 4× in the showroom. Off, 2× and 4× set both. Takes effect in the next scene. |
| HDR | Auto uses HDR on a compatible display. Off uses standard brightness. |
| Extended View | Matches the camera to the window shape. A wider window shows more at the sides and a taller window shows more above and below. |
| Scenery Shadows | Adds shadows for trackside cones and barriers during races. These can flicker near the edge of the screen. |
| Reflection Updates | Controls how often reflections on cars refresh. Console follows the original game. Every Race also updates them in 16-car and split-screen races. Double updates twice as many reflection faces per frame. |
| Mirror | Full refreshes the rear-view mirror every frame and can show up to 16 cars. Console shows up to six at half rate in 12- and 16-car races. |
| Soft Particles | Softens the edge of smoke and dust where they touch cars or the road. |
| Crowds | Every Race adds spectators to solo and 16-car races and uses more memory. Console follows the original game's race settings. |
| Motion Blur | Blurs the road surface as the car gains speed. |
| Depth of Field | Blurs areas outside the camera's focus in the showroom, pre-race sequences, replays and Photo Mode. Takes effect from the next screen. |
| Car Dirt | Shows dirt accumulating on the car during a race. |

**Automatic upscaling.** When the rendered picture is smaller than the area it fills in the game window, spatial FSR 1 enlarges it. At matching sizes the picture passes through at its rendered size. A larger rendered picture is scaled down. The comparison uses the visible game area in display pixels and excludes black bars. MSAA remains available at every resolution.

The default home is `~/Library/Application Support/FM4 Recomp/`. Graphics and input settings live under `config/`, and session logs under `logs/`. The game uses the save location selected during setup, normally `data/user-data/`. On the first launch the app imports compatible older career packages and settings from 0.3.0. Existing destination saves take priority, and both original profiles are preserved. Disc 1 and externally configured storage keep the paths recorded during setup.

The game executable contains code translated from the original game. Players supply their own game data. The [FM4 Recomp notices](licenses/THIRD_PARTY_NOTICES.md) and [engine notices](engines/shiftglue/licenses/reference-host-notices.md) accompany the kit.
