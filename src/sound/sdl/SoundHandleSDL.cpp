#include "SoundHandleSDL.h"

#include "../SoundSystem.h"
#include "SoundSDL.h"
#include "SoundSystemSDL.h"
#include "oxygine/core/oxygine.h"

namespace oxygine {

SoundHandleSDL::SoundHandleSDL(SoundSDL* sound)
    : _sound(sound), _voiceId(0), _positionMs(0), _tick(0) {
}

SoundHandleSDL::~SoundHandleSDL() {
   SoundSystemSDL* system = ss();
   if (_voiceId && system) system->stopVoice(_voiceId);
}

SoundSystemSDL* SoundHandleSDL::ss() const {
   return dynamic_cast<SoundSystemSDL*>(SoundSystem::get());
}

void SoundHandleSDL::_play() {
   if (_voiceId) {
      if (SoundSystemSDL* previous = ss()) previous->stopVoice(_voiceId);
      _voiceId = 0;
   }

   _positionMs = 0;
   _tick = getTimeMS();

   SoundSystemSDL* system = ss();
   if (!system || !system->isOpen() || !_sound || !_sound->getPCM().valid()) {
      _state = ended;
      return;
   }

   const SoundPCM& pcm = _sound->getPCM();
   _voiceId = system->startVoice(pcm.samples.data(), pcm.frames, pcm.channels,
                                 pcm.sampleRate, _volume, _looping);
   if (!_voiceId) _state = ended;
}

void SoundHandleSDL::_pause() {
   if (!_voiceId) return;

   // The base class changed the state before calling us, so claim the time
   // played since the last update here rather than in _getPosition().
   _positionMs += (int)(getTimeMS() - _tick);
   _tick = getTimeMS();

   if (SoundSystemSDL* system = ss()) system->pauseVoice(_voiceId);
}

void SoundHandleSDL::_resume() {
   if (!_voiceId) return;

   SoundSystemSDL* system = ss();
   if (!system || !system->isVoiceActive(_voiceId)) {
      _state = ended;
      _voiceId = 0;
      return;
   }

   _tick = getTimeMS();
   system->resumeVoice(_voiceId);
}

void SoundHandleSDL::_stop() {
   if (_voiceId) {
      if (SoundSystemSDL* system = ss()) system->stopVoice(_voiceId);
      _voiceId = 0;
   }
   _positionMs = 0;
}

void SoundHandleSDL::_update() {
   if (!_voiceId) return;

   SoundSystemSDL* system = ss();
   if (!system || !system->isVoiceActive(_voiceId)) {
      // The mixer freed the slot because the decoded PCM was exhausted.
      _positionMs = _sound ? _sound->getDuration() : 0;
      _voiceId = 0;
      _state = ended;
      return;
   }

   _positionMs = _getPosition();
   _tick = getTimeMS();
}

void SoundHandleSDL::_updateVolume() {
   // The gain is latched at start: a gain change after start does not apply.
}

void SoundHandleSDL::_setPosition(int tm) {
   // Seeking is not part of the SDL backend; the game never seeks a one-shot.
   (void)tm;
}

timeMS SoundHandleSDL::_getPosition() const {
   int pos = _positionMs;
   if (_voiceId && _state == playing) pos += (int)(getTimeMS() - _tick);

   const int duration = _sound ? _sound->getDuration() : 0;
   if (pos > duration) pos = duration;
   if (pos < 0) pos = 0;
   return pos;
}

}  // namespace oxygine
