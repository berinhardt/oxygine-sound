#include "SoundSystemSDL.h"

#include <cstdio>
#include <cstring>
#include <time.h>

#include "../OggStream.h"
#include "../SdlDeviceStart.h"
#include "../null/SoundSystemNull.h"
#include "SoundPCM.h"
#include "SoundSDL.h"
#include "oxygine/actor/DebugActor.h"
#include "oxygine/core/file.h"
#include "oxygine/core/log.h"

namespace oxygine {
namespace {

const char* formatName(Uint16 format) {
   if (format == AUDIO_S16SYS) return "S16SYS";
   if (format == AUDIO_F32SYS) return "F32SYS";
   if (format == AUDIO_U8) return "U8";
   if (format == AUDIO_S8) return "S8";
   return "other";
}

// CPU time consumed by this thread. It excludes time the thread was
// descheduled, so a loaded host cannot turn the engine's underrun counter
// into a scheduler-noise counter. If the clock is unavailable, the callback
// simply reports no duration.
unsigned long long threadCpuNs() {
   timespec ts;
   if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
   return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

bool decodeOggStream(SoundStream& stream, SoundPCM& pcm, const char* name) {
   if (!decodeSoundPCM(stream, pcm) || !pcm.valid()) {
      logs::error("SoundSystemSDL: cannot decode '%s'", name ? name : "memory");
      return false;
   }
   return true;
}

}  // namespace

SoundSystem* SoundSystem::create() {
   if (!SoundSystem::instance) {
      SoundSystemSDL* sdl = new SoundSystemSDL;
      if (sdl->open()) {
         SoundSystem::instance = sdl;
      } else {
         // open() already logged the one reason it failed. Do not add a
         // second line: the no-device path is limited to one error line.
         delete sdl;
         SoundSystem::instance = new SoundSystemNull;
      }
   }

   return SoundSystem::instance;
}

SoundSystemSDL::SoundSystemSDL()
    : _device(0),
      _paused(false),
      _stopping(false),
      _activeVoices(0),
      _underruns(0),
      _mixedBytes(0),
      _callbacks(0),
      _callbackMaxNs(0) {
   SDL_zero(_obtained);
}

SoundSystemSDL::~SoundSystemSDL() {
}

bool SoundSystemSDL::open() {
   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      logs::error("SoundSystemSDL: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
      return false;
   }

   SDL_AudioSpec want;
   SDL_zero(want);
   want.freq = Mixer::kOutputRate;
   want.format = AUDIO_S16SYS;
   want.channels = 2;
   want.samples = 1024;
   want.callback = &SoundSystemSDL::audioCallback;
   want.userdata = this;

   // allowed_changes = 0: no frequency, format or channel conversion.
   _device = SDL_OpenAudioDevice(0, 0, &want, &_obtained, 0);
   if (!_device) {
      logs::error("SoundSystemSDL: SDL_OpenAudioDevice failed: %s", SDL_GetError());
      SDL_zero(_obtained);
      return false;
   }

   if (_obtained.format != AUDIO_S16SYS || (_obtained.channels != 1 && _obtained.channels != 2) ||
       _obtained.freq != Mixer::kOutputRate) {
      logs::error("SoundSystemSDL: device granted an unusable spec: freq=%d format=%s channels=%d",
                  (int)_obtained.freq, formatName(_obtained.format), (int)_obtained.channels);
      SDL_CloseAudioDevice(_device);
      _device = 0;
      SDL_zero(_obtained);
      return false;
   }

   _scratch.assign((size_t)_obtained.samples * 2, 0);

   // A fresh device is always audible: it starts with the engine-level flags
   // clear, and the one-time start lives outside this directory so the
   // lifecycle sources never take the device lock.
   _paused.store(false);
   _stopping.store(false);
   startSdlAudioDevice(_device);

   // One line per device open, so the selected driver and the obtained spec
   // can be asserted from `adb logcat -s SDL` on the device.
   char line[256];
   snprintf(line, sizeof(line), "SoundSystemSDL init: driver=%s freq=%d format=%s channels=%d samples=%d",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
            (int)_obtained.freq, formatName(_obtained.format),
            (int)_obtained.channels, (int)_obtained.samples);
   logs::out(line);

   return true;
}

void SoundSystemSDL::resetDevice() {
   _stopping.store(true);

   if (_device) {
      // Blocks until the callback is done; nothing is freed before this
      // returns. Best-effort teardown is accepted by the spec.
      SDL_CloseAudioDevice(_device);
      _device = 0;
   }

   {
      std::lock_guard<std::mutex> lock(_mutex);
      _mixer.stopAll();
      _activeVoices.store(0, std::memory_order_relaxed);
   }

   _scratch.clear();
   SDL_zero(_obtained);
   _stopping.store(false);
}

void SoundSystemSDL::release() {
   // Decision 15's remaining surface: peak voices, the worst callback duration
   // as a fraction of the buffer period, and the underrun count, reported once
   // per device run. It is outside the audio callback and outside the fixed
   // `channels:` line, and it is unconditional because Android never defines
   // OX_DEBUG (the init line from the same decision is unconditional too).
   // SoundSystem::free() calls release() after an explicit release(); the
   // closed device skips the second line.
   if (_device) {
      const DebugStats stats = debugStats();
      char line[192];
      snprintf(line, sizeof(line),
               "SoundSystemSDL release: peak_voices=%d callback_fraction=%.3f underruns=%llu",
               stats.peakVoices, stats.callbackFraction, stats.underruns);
      logs::out(line);
   }

   resetDevice();
}

void SoundSystemSDL::pause() {
   _paused.store(true);
}

void SoundSystemSDL::resume() {
   _paused.store(false);
}

void SoundSystemSDL::stop() {
   std::lock_guard<std::mutex> lock(_mutex);
   _mixer.stopAll();
   _activeVoices.store(0, std::memory_order_relaxed);
}

Sound* SoundSystemSDL::createSound(const char* path, bool streaming) {
   (void)streaming;  // Every clip is a fully decoded one-shot.

   if (!path) {
      logs::error("SoundSystemSDL: cannot open a null sound path");
      return 0;
   }

   // file::open warns once on failure (oxygine.cpp), so this branch adds no
   // second line: decision 7 allows exactly one per open failure.
   file::handle fh = file::open(path, "srb", ep_show_warning);
   if (!fh) return 0;

   SoundPCM pcm;
   bool decoded = false;
   {
      OggStream ogg;
      if (!ogg.init(fh, false)) {
         logs::error("SoundSystemSDL: cannot decode '%s'", path);
      } else {
         decoded = decodeOggStream(ogg, pcm, path);
      }
   }
   file::close(fh);

   if (!decoded) return 0;

   return new SoundSDL(pcm);
}

Sound* SoundSystemSDL::createSound(std::vector<unsigned char>& data, bool swap) {
   if (swap) {
      std::vector<unsigned char> empty;
      empty.swap(data);
   }

   if (data.empty()) {
      logs::error("SoundSystemSDL: cannot decode an empty buffer");
      return 0;
   }

   SoundPCM pcm;
   OggStream ogg;
   if (!ogg.init(data.data(), data.size())) {
      logs::error("SoundSystemSDL: cannot decode an in-memory sound");
      return 0;
   }
   if (!decodeOggStream(ogg, pcm, 0)) return 0;

   return new SoundSDL(pcm);
}

void SoundSystemSDL::update() {
   if (!_device) return;

   if (DebugActor::instance) {
      // Decision 9: the game thread holds the audio mutex only while applying
      // a command, so this per-frame line reads the count the command paths
      // keep current instead of locking.
      char str[64];
      snprintf(str, sizeof(str), "channels: %d/%d",
               _activeVoices.load(std::memory_order_relaxed), Mixer::kMaxVoices);
      DebugActor::instance->addDebugString(str);
   }
}

int SoundSystemSDL::startVoice(const int16_t* pcm, int frames, int channels, int sampleRate,
                               float gain, bool loop) {
   std::lock_guard<std::mutex> lock(_mutex);
   const int id = _mixer.start(pcm, frames, channels, sampleRate, gain, loop);
   _activeVoices.store(_mixer.activeVoices(), std::memory_order_relaxed);
   return id;
}

void SoundSystemSDL::stopVoice(int voiceId) {
   std::lock_guard<std::mutex> lock(_mutex);
   _mixer.stop(voiceId);
   _activeVoices.store(_mixer.activeVoices(), std::memory_order_relaxed);
}

void SoundSystemSDL::pauseVoice(int voiceId) {
   std::lock_guard<std::mutex> lock(_mutex);
   _mixer.pause(voiceId);
}

void SoundSystemSDL::resumeVoice(int voiceId) {
   std::lock_guard<std::mutex> lock(_mutex);
   _mixer.resume(voiceId);
}

bool SoundSystemSDL::isVoiceActive(int voiceId) const {
   std::lock_guard<std::mutex> lock(_mutex);
   return _mixer.isActive(voiceId);
}

int SoundSystemSDL::activeVoices() const {
   std::lock_guard<std::mutex> lock(_mutex);
   return _mixer.activeVoices();
}

int SoundSystemSDL::peakVoices() const {
   std::lock_guard<std::mutex> lock(_mutex);
   return _mixer.peakVoices();
}

SoundSystemSDL::DebugStats SoundSystemSDL::debugStats() const {
   DebugStats stats;
   const char* driver = SDL_GetCurrentAudioDriver();
   stats.driver = driver ? driver : "unknown";
   stats.format = formatName(_obtained.format);
   stats.freq = (int)_obtained.freq;
   stats.channels = (int)_obtained.channels;
   stats.samples = (int)_obtained.samples;

   {
      std::lock_guard<std::mutex> lock(_mutex);
      stats.activeVoices = _mixer.activeVoices();
      stats.peakVoices = _mixer.peakVoices();
   }

   stats.underruns = _underruns.load(std::memory_order_relaxed);
   stats.mixedBytes = _mixedBytes.load(std::memory_order_relaxed);
   stats.callbacks = _callbacks.load(std::memory_order_relaxed);

   const unsigned long long maxNs = _callbackMaxNs.load(std::memory_order_relaxed);
   const unsigned long long periodNs =
       (_obtained.freq > 0) ? (unsigned long long)_obtained.samples * 1000000000ull / _obtained.freq
                            : 0;
   stats.callbackFraction = periodNs ? (double)maxNs / (double)periodNs : 0.0;
   return stats;
}

void SoundSystemSDL::resetDebugStats() {
   _underruns.store(0, std::memory_order_relaxed);
   _mixedBytes.store(0, std::memory_order_relaxed);
   _callbacks.store(0, std::memory_order_relaxed);
   _callbackMaxNs.store(0, std::memory_order_relaxed);
}

void SoundSystemSDL::audioCallback(void* userdata, Uint8* stream, int len) {
   static_cast<SoundSystemSDL*>(userdata)->render(stream, len);
}

void SoundSystemSDL::render(Uint8* stream, int len) {
   const int channelBytes = (int)sizeof(int16_t) * (int)_obtained.channels;
   if (!stream || len <= 0 || channelBytes <= 0) return;

   // The probe reports a callback duration as a fraction of the buffer
   // period, so measure the whole render. This is thread CPU time: a
   // descheduled callback costs no CPU and must not be reported as the
   // engine failing to feed the device. The clock neither allocates nor
   // takes the device lock.
   const unsigned long long startNs = threadCpuNs();

   if (_paused.load(std::memory_order_relaxed) || _stopping.load(std::memory_order_relaxed)) {
      memset(stream, 0, (size_t)len);
      return;
   }

   const int frames = len / channelBytes;
   bool voicesActive = false;

   {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_stopping.load(std::memory_order_relaxed)) {
         memset(stream, 0, (size_t)len);
         return;
      }

      voicesActive = _mixer.activeVoices() > 0;

      if (_obtained.channels == 2) {
         _mixer.render(reinterpret_cast<int16_t*>(stream), frames);
      } else {
         const int scratchFrames = (int)(_scratch.size() / 2);
         const int renderFrames = frames < scratchFrames ? frames : scratchFrames;
         _mixer.render(_scratch.data(), renderFrames);

         int16_t* out = reinterpret_cast<int16_t*>(stream);
         for (int f = 0; f < renderFrames; ++f) {
            out[f] = (int16_t)((_scratch[(size_t)f * 2] + _scratch[(size_t)f * 2 + 1]) / 2);
         }
         if (renderFrames < frames) {
            memset(out + renderFrames, 0, (size_t)(frames - renderFrames) * sizeof(int16_t));
         }
      }

      // A one-shot that reached its end in this buffer freed its slot here.
      _activeVoices.store(_mixer.activeVoices(), std::memory_order_relaxed);
   }

   const unsigned long long endNs = threadCpuNs();
   _callbacks.fetch_add(1, std::memory_order_relaxed);
   // Bytes are "mixed" only when a voice was active for the buffer; a callback
   // that had nothing to play emitted silence and is not counted.
   if (voicesActive) _mixedBytes.fetch_add((unsigned long long)len, std::memory_order_relaxed);

   const unsigned long long periodNs =
       (_obtained.freq > 0) ? (unsigned long long)_obtained.samples * 1000000000ull / _obtained.freq
                            : 0;
   if (!periodNs || endNs < startNs) return;

   const unsigned long long durationNs = endNs - startNs;
   unsigned long long current = _callbackMaxNs.load(std::memory_order_relaxed);
   while (durationNs > current &&
          !_callbackMaxNs.compare_exchange_weak(current, durationNs, std::memory_order_relaxed)) {
   }

   // An underrun is the engine failing to produce the next buffer in time:
   // the callback's own mixing work consumed a whole buffer period of CPU.
   // That is independent of how long the process was descheduled, so a
   // loaded host cannot manufacture one. Wall-clock lateness is not counted;
   // it cannot be told apart from scheduling noise without a device-owned
   // queue counter, which SDL does not expose.
   if (durationNs >= periodNs) _underruns.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace oxygine
