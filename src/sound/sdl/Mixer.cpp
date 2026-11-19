#include "Mixer.h"

#include <algorithm>

namespace oxygine {
namespace {

int32_t saturate(int32_t sample) {
   if (sample > 32767) return 32767;
   if (sample < -32768) return -32768;
   return sample;
}

}  // namespace

Mixer::Mixer() = default;

Mixer::~Mixer() = default;

Mixer::Voice* Mixer::find(int voiceId) {
   if (voiceId <= 0) return nullptr;
   for (int i = 0; i < kMaxVoices; ++i) {
      Voice& voice = voices_[i];
      if (voice.active && voice.id == voiceId) return &voice;
   }
   return nullptr;
}

const Mixer::Voice* Mixer::find(int voiceId) const {
   if (voiceId <= 0) return nullptr;
   for (int i = 0; i < kMaxVoices; ++i) {
      const Voice& voice = voices_[i];
      if (voice.active && voice.id == voiceId) return &voice;
   }
   return nullptr;
}

Mixer::Voice& Mixer::allocate() {
   for (int i = 0; i < kMaxVoices; ++i) {
      if (!voices_[i].active) return voices_[i];
   }

   // The pool is fixed: steal the oldest active voice.
   int oldest = 0;
   for (int i = 1; i < kMaxVoices; ++i) {
      if (voices_[i].sequence < voices_[oldest].sequence) oldest = i;
   }
   release(voices_[oldest]);
   return voices_[oldest];
}

void Mixer::release(Voice& voice) {
   if (!voice.active) return;
   voice.active = false;
   voice.paused = false;
   voice.id = 0;
   voice.pcm = nullptr;
   voice.frames = 0;
   voice.position = 0.0;
   --active_;
}

void Mixer::endVoice(Voice& voice) {
   if (!voice.active) return;
   release(voice);
   ++finished_;
}

void Mixer::updatePeak() {
   if (active_ > peak_) peak_ = active_;
}

int Mixer::start(const int16_t* pcm, int frames, int channels, int sampleRate, float gain, bool loop) {
   if (!pcm || frames <= 0 || (channels != 1 && channels != 2) || sampleRate <= 0) return 0;

   Voice& voice = allocate();
   voice.pcm = pcm;
   voice.frames = frames;
   voice.channels = channels;
   voice.sampleRate = sampleRate;
   voice.position = 0.0;
   voice.gain = gain;
   voice.loop = loop;
   voice.paused = false;
   voice.active = true;
   voice.id = nextId_++;
   voice.sequence = ++sequence_;
   ++active_;
   updatePeak();
   return voice.id;
}

void Mixer::stop(int voiceId) {
   Voice* voice = find(voiceId);
   if (voice) release(*voice);
}

void Mixer::stopAll() {
   for (int i = 0; i < kMaxVoices; ++i) release(voices_[i]);
}

void Mixer::pause(int voiceId) {
   Voice* voice = find(voiceId);
   if (voice) voice->paused = true;
}

void Mixer::resume(int voiceId) {
   Voice* voice = find(voiceId);
   if (voice) voice->paused = false;
}

void Mixer::pauseAll() {
   paused_ = true;
}

void Mixer::resumeAll() {
   paused_ = false;
}

void Mixer::mixVoice(Voice& voice, int32_t& left, int32_t& right) {
   if (voice.position >= (double)voice.frames) {
      if (voice.loop) {
         while (voice.position >= (double)voice.frames) voice.position -= (double)voice.frames;
      } else {
         endVoice(voice);
         return;
      }
   }

   const int index = (int)voice.position;
   const int next = index + 1 < voice.frames ? index + 1 : (voice.loop ? 0 : voice.frames - 1);
   const float fraction = (float)(voice.position - (double)index);

   const int16_t* frame = voice.pcm + (size_t)index * (size_t)voice.channels;
   const int16_t* nextFrame = voice.pcm + (size_t)next * (size_t)voice.channels;

   const float left0 = (float)frame[0];
   const float left1 = (float)nextFrame[0];
   float sampleLeft = left0 + (left1 - left0) * fraction;

   float sampleRight = sampleLeft;
   if (voice.channels == 2) {
      const float right0 = (float)frame[1];
      const float right1 = (float)nextFrame[1];
      sampleRight = right0 + (right1 - right0) * fraction;
   }

   left += (int32_t)(sampleLeft * voice.gain);
   right += (int32_t)(sampleRight * voice.gain);

   voice.position += (double)voice.sampleRate / (double)kOutputRate;
   if (voice.loop) {
      while (voice.position >= (double)voice.frames) voice.position -= (double)voice.frames;
   }
}

void Mixer::render(int16_t* out, int frames) {
   if (!out || frames <= 0) return;

   for (int f = 0; f < frames; ++f) {
      int32_t left = 0;
      int32_t right = 0;
      if (!paused_) {
         for (int i = 0; i < kMaxVoices; ++i) {
            Voice& voice = voices_[i];
            if (voice.active && !voice.paused) mixVoice(voice, left, right);
         }
      }
      out[f * 2] = (int16_t)saturate(left);
      out[f * 2 + 1] = (int16_t)saturate(right);
   }
}

int Mixer::activeVoices() const {
   return active_;
}

int Mixer::peakVoices() const {
   return peak_;
}

int Mixer::finishedVoices() const {
   return finished_;
}

bool Mixer::isActive(int voiceId) const {
   return find(voiceId) != nullptr;
}

bool Mixer::isPaused(int voiceId) const {
   const Voice* voice = find(voiceId);
   return voice != nullptr && voice->paused;
}

int Mixer::voiceIdAt(int slot) const {
   if (slot < 0 || slot >= kMaxVoices) return 0;
   const Voice& voice = voices_[slot];
   return voice.active ? voice.id : 0;
}

}  // namespace oxygine
