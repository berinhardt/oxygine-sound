#pragma once

#include "../Sound.h"
#include "SoundPCM.h"

namespace oxygine {

// One fully decoded one-shot. The PCM is owned here and outlives every voice
// the mixer starts from it; the backend never decodes during playback.
class SoundSDL : public Sound {
   public:
   explicit SoundSDL(const SoundPCM& pcm);
   ~SoundSDL();

   int getDuration() const override;
   SoundHandle* createSH() override;

   const SoundPCM& getPCM() const { return _pcm; }

   private:
   SoundPCM _pcm;
};

}  // namespace oxygine
