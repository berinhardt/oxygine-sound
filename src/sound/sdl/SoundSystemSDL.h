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

   // Debug counters for a device run. The device probe reads them so a build
   // without a debugger can report what the audio thread did. `mixedBytes`
   // counts the bytes of callbacks where at least one voice was active;
   // callbacks that only emitted silence because nothing was playing, or
   // because the pause or stop flag was set, are not counted. `underruns`
   // counts callbacks whose own mixing work used a whole buffer period of CPU
   // time (the engine could not produce the buffer in time), and
   // `callbackFraction` is the worst callback CPU duration divided by the
   // buffer period. Wall-clock lateness is deliberately excluded: it reports
   // process descheduling, not the engine failing to keep the device fed.
   struct DebugStats {
      const char* driver;
      const char* format;
      int freq;
      int channels;
      int samples;
      int activeVoices;
      int peakVoices;
      unsigned long long underruns;
      unsigned long long mixedBytes;
      unsigned long long callbacks;
      double callbackFraction;
   };

   DebugStats debugStats() const;
   void resetDebugStats();

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

   // Occupied voice slots for the per-frame debug line. Every path that
   // changes the mixer's active set stores the new count while it holds
   // _mutex; update() reads it without taking the mutex.
   std::atomic<int> _activeVoices;

   // Written by the audio callback, read by debugStats(). Plain counters
   // would tear when the probe read them mid-callback.
   std::atomic<unsigned long long> _underruns;
   std::atomic<unsigned long long> _mixedBytes;
   std::atomic<unsigned long long> _callbacks;
   std::atomic<unsigned long long> _callbackMaxNs;

   // Callback scratch for a mono device; sized once when the device opens so
   // the callback allocates nothing.
   std::vector<int16_t> _scratch;
};

}  // namespace oxygine
