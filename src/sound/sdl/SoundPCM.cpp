#include "SoundPCM.h"

namespace oxygine {

bool decodeSoundPCM(SoundStream& stream, SoundPCM& out) {
   out = SoundPCM();

   if (stream.isStreamEnded()) return false;

   const int frames = stream.getPCM();
   const int channels = stream.getNumChannels();
   const int sampleRate = stream.getRate();

   if (frames <= 0 || (channels != 1 && channels != 2) || sampleRate <= 0) return false;

   const size_t count = (size_t)frames * (size_t)channels;
   out.samples.assign(count, 0);
   stream.decodeAll(out.samples.data(), (int)(count * sizeof(int16_t)));

   out.frames = frames;
   out.channels = channels;
   out.sampleRate = sampleRate;
   out.duration = (int)(((int64_t)frames * 1000) / (int64_t)sampleRate);

   return out.valid();
}

}  // namespace oxygine
