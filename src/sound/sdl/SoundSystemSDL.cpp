#include "SoundSystemSDL.h"

#include <cstdio>
#include <cstring>

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
    : _device(0), _paused(false), _stopping(false) {
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
   }

   _scratch.clear();
   SDL_zero(_obtained);
   _stopping.store(false);
}

void SoundSystemSDL::release() {
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
}

Sound* SoundSystemSDL::createSound(const char* path, bool streaming) {
   (void)streaming;  // Every clip is a fully decoded one-shot.

   if (!path) return 0;

   file::handle fh = file::open(path, "srb", ep_show_warning);
   if (!fh) {
      logs::error("SoundSystemSDL: cannot open '%s'", path);
      return 0;
   }

   SoundPCM pcm;
   bool decoded = false;
   {
      OggStream ogg;
      if (ogg.init(fh, false)) decoded = decodeOggStream(ogg, pcm, path);
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
      char str[64];
      snprintf(str, sizeof(str), "channels: %d/%d", activeVoices(), Mixer::kMaxVoices);
      DebugActor::instance->addDebugString(str);
   }
}

int SoundSystemSDL::startVoice(const int16_t* pcm, int frames, int channels, int sampleRate,
                               float gain, bool loop) {
   std::lock_guard<std::mutex> lock(_mutex);
   return _mixer.start(pcm, frames, channels, sampleRate, gain, loop);
}

void SoundSystemSDL::stopVoice(int voiceId) {
   std::lock_guard<std::mutex> lock(_mutex);
   _mixer.stop(voiceId);
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

void SoundSystemSDL::audioCallback(void* userdata, Uint8* stream, int len) {
   static_cast<SoundSystemSDL*>(userdata)->render(stream, len);
}

void SoundSystemSDL::render(Uint8* stream, int len) {
   const int channelBytes = (int)sizeof(int16_t) * (int)_obtained.channels;
   if (!stream || len <= 0 || channelBytes <= 0) return;

   if (_paused.load(std::memory_order_relaxed) || _stopping.load(std::memory_order_relaxed)) {
      memset(stream, 0, (size_t)len);
      return;
   }

   const int frames = len / channelBytes;

   std::lock_guard<std::mutex> lock(_mutex);
   if (_stopping.load(std::memory_order_relaxed)) {
      memset(stream, 0, (size_t)len);
      return;
   }

   if (_obtained.channels == 2) {
      _mixer.render(reinterpret_cast<int16_t*>(stream), frames);
      return;
   }

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

}  // namespace oxygine
