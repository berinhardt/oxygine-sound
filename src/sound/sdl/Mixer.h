#pragma once

#include <cstdint>

namespace oxygine {

// Plain voice mixer core for the SDL audio backend.
//
// It owns a fixed pool of 32 voice slots, each playing fully decoded
// native-rate S16 PCM (mono or stereo). Voices are linearly resampled to
// 44.1 kHz, summed in 32-bit and saturated into an interleaved S16 stereo
// buffer. A non-looping voice ends when its PCM is exhausted, frees its slot
// and leaves only silence behind.
//
// The unit does no device, file, clock or thread work and allocates nothing
// after construction. Commands and render() are not thread-safe; the caller
// owns the lock that keeps them apart.
class Mixer {
  public:
   enum { kOutputRate = 44100, kMaxVoices = 32 };

   Mixer();
   ~Mixer();

   // Starts a voice from decoded PCM. `pcm` interleaves `channels` (1 or 2)
   // samples per frame and must outlive the voice. `sampleRate` is the
   // source's native rate. The gain is latched for the life of the voice.
   // Returns the voice id, or 0 if the source is invalid. When the pool is
   // full the oldest active voice is stolen.
   int start(const int16_t* pcm, int frames, int channels, int sampleRate, float gain, bool loop);

   // Stops one voice, or every voice. A stopped voice leaves its slot free.
   void stop(int voiceId);
   void stopAll();

   // Freezes one voice in place: it renders silence without advancing, and
   // resume() continues from the same sample.
   void pause(int voiceId);
   void resume(int voiceId);

   // Freezes every voice in place. Idempotent and safe in any order.
   void pauseAll();
   void resumeAll();

   // Writes `frames` interleaved stereo frames into `out`.
   void render(int16_t* out, int frames);

   // Occupied slots, paused voices included. Never above kMaxVoices.
   int activeVoices() const;
   // Highest active count seen since construction.
   int peakVoices() const;
   // Number of one-shots that have reached their end and freed their slot.
   int finishedVoices() const;

   bool isActive(int voiceId) const;
   bool isPaused(int voiceId) const;

  private:
   struct Voice {
      const int16_t* pcm = nullptr;
      int frames = 0;
      int channels = 1;
      int sampleRate = 0;
      double position = 0.0;  // position in source frames
      float gain = 1.0f;
      uint32_t sequence = 0;
      int id = 0;
      bool active = false;
      bool paused = false;
      bool loop = false;
   };

   Voice* find(int voiceId);
   const Voice* find(int voiceId) const;
   Voice& allocate();
   void release(Voice& voice);
   void updatePeak();
   void endVoice(Voice& voice);
   void mixVoice(Voice& voice, int32_t& left, int32_t& right);

   Voice voices_[kMaxVoices];
   int nextId_ = 1;
   uint32_t sequence_ = 0;
   int active_ = 0;
   int peak_ = 0;
   int finished_ = 0;
   bool paused_ = false;
};

}  // namespace oxygine
