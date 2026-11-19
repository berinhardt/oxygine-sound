#pragma once

#include <SDL.h>

namespace oxygine {

// SDL2 hands back a paused device: the callback thread runs, but the callback
// itself is not invoked until the device is started, and SDL2 has no
// open-and-running call. So this is the one device state change the engine
// makes outside its own pause and stop flags.
//
// It is deliberately kept out of src/sound/sdl. The backend's lifecycle
// sources must contain no device-lock call, because that lock has no timeout;
// locking a just-opened device would deadlock the very path the engine is
// trying to avoid. The check in test/android asserts that invariant by
// scanning that directory.
//
// open() calls this once, on the thread that just created the device, before
// any other thread can hold a reference to its id. At that point the device
// lock has no other holder, so the call is bounded.
void startSdlAudioDevice(SDL_AudioDeviceID device);

}  // namespace oxygine
