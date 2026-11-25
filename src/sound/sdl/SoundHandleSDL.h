#pragma once

#include "../SoundHandle.h"

namespace oxygine {

class SoundSDL;
class SoundSystemSDL;

// Handle for one playback of a fully decoded sound. The gain and loop flag
// are latched when the voice starts; later gain changes are ignored so a
// later play at another volume cannot alter a sound already ringing.
class SoundHandleSDL : public SoundHandle {
   public:
   explicit SoundHandleSDL(SoundSDL* sound);
   ~SoundHandleSDL();

   protected:
   void _update() override;
   void _play() override;
   void _pause() override;
   void _resume() override;
   void _stop() override;
   void _updateVolume() override;
   void _setPosition(int tm) override;

   timeMS _getPosition() const override;

   private:
   SoundSystemSDL* ss() const;

   SoundSDL* _sound;
   int _voiceId;
   int _positionMs;
   timeMS _tick;
};

}  // namespace oxygine
