#include "SdlDeviceStart.h"

namespace oxygine {

void startSdlAudioDevice(SDL_AudioDeviceID device) {
   if (device) SDL_PauseAudioDevice(device, 0);
}

}  // namespace oxygine
