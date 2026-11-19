#include "SoundSDL.h"

#include "SoundHandleSDL.h"

namespace oxygine {

SoundSDL::SoundSDL(const SoundPCM& pcm) : _pcm(pcm) {
}

SoundSDL::~SoundSDL() {
}

int SoundSDL::getDuration() const {
   return _pcm.duration;
}

SoundHandle* SoundSDL::createSH() {
   return new SoundHandleSDL(this);
}

}  // namespace oxygine
