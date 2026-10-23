# SDL3 controller runtime

Official SDL3 3.4.18 Windows VC development package; source URL and verified
archive SHA-256 are in `package.json`, redistribution terms in `LICENSE.txt`.
Headers enable the existing optional SDL3 backend automatically. The player
CMake target copies `lib/x64/SDL3.dll` next to the built executable.

XInput continues handling Xbox controllers. SDL HIDAPI handles native Sony
controllers and other supported non-XInput pads, with hot-plugging, rumble and
the existing touchpad mapping. Automatic prompts retain the original game
glyphs for PlayStation controllers.

Connect Sony controllers directly through USB or Bluetooth. DS4Windows, Steam
Input or another wrapper can expose them as virtual Xbox devices or hide them
from SDL; disable the wrapper for native identification, or use the explicit
PlayStation prompt setting. `PAD_SDL=0` disables the SDL backend.
