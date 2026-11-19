#pragma once

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../SoundSystem.h"
#include "Mixer.h"

namespace oxygine {

class SoundSDL;

// SDL2 playback device and mixer wiring behind the unchanged SoundSystem
// seam. It opens one S16 stereo device at 44.1 kHz, latches a voice's gain
// when it starts, and removes a one-shot when its decoded PCM is exhausted.
class SoundSystemSDL : public SoundSystem {
   public:
   SoundSystemSDL();
   ~SoundSystemSDL();

   // Opens the device. Returns false when the device cannot be opened; the
   // caller then installs the silent backend.
   bool open();
   bool isOpen() const { return _device != 0; }

   // Pause and stop are engine-level flags the callback reads. None of these
   // paths takes SDL's device lock: the lock has no timeout and a lifecycle
   // thread can block in it forever.
   void release() override;
   void pause() override;
   void resume() override;
   void stop() override;

   Sound* createSound(const char* file, bool streaming) override;
   Sound* createSound(std::vector<unsigned char>& data, bool swap) override;

   void update() override;

   // Voice commands. The audio callback holds the same mutex only for the
   // length of one render.
   int startVoice(const int16_t* pcm, int frames, int channels, int sampleRate,
                  float gain, bool loop);
   void stopVoice(int voiceId);
   void pauseVoice(int voiceId);
   void resumeVoice(int voiceId);
   bool isVoiceActive(int voiceId) const;

   int activeVoices() const;
   int peakVoices() const;

   private:
   static void audioCallback(void* userdata, Uint8* stream, int len);
   void render(Uint8* stream, int len);
   void resetDevice();

   SDL_AudioDeviceID _device;
   SDL_AudioSpec _obtained;

   mutable std::mutex _mutex;
   Mixer _mixer;
   std::atomic<bool> _paused;
   std::atomic<bool> _stopping;

   // Callback scratch for a mono device; sized once when the device opens so
   // the callback allocates nothing.
   std::vector<int16_t> _scratch;
};

}  // namespace oxygine
