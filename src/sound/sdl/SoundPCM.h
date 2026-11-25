#pragma once

#include <cstdint>
#include <vector>

#include "../SoundStream.h"

namespace oxygine {

// A sound decoded eagerly to native-rate, signed 16-bit PCM. `samples`
// interleaves `channels` (1 or 2) samples per frame; `frames` is the source's
// frame count and `duration` is derived from it, never from wall clock.
struct SoundPCM {
   std::vector<int16_t> samples;
   int frames = 0;
   int channels = 0;
   int sampleRate = 0;
   int duration = 0;  // milliseconds

   bool valid() const {
      return frames > 0 && (channels == 1 || channels == 2) && sampleRate > 0 &&
             samples.size() == (size_t)frames * (size_t)channels;
   }
};

// Reads the whole stream into `out`. Returns false for an ended, empty or
// unsupported stream, in which case `out` is left empty. This unit does no
// SDL, device or thread work, so the host test target can compile it.
bool decodeSoundPCM(SoundStream& stream, SoundPCM& out);

}  // namespace oxygine
