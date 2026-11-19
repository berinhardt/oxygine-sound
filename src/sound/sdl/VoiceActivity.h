#pragma once

#include <atomic>

#include "Mixer.h"

namespace oxygine {

// Lock-free mirror of which voice ids occupy the mixer's slot pool.
//
// The mixer is guarded by the device mutex: commands and the audio callback
// take it, and the game thread must not hold it just to ask whether a voice is
// still playing. Every path that changes the mixer's active set republishes
// this mirror while it already holds that mutex (start, stop, stop-all, device
// reset, and the render that ends a one-shot); readers then answer without a
// lock. The Mixer itself stays free of atomics: this is backend state.
class VoiceActivity {
   public:
   VoiceActivity();

   // Copies the mixer's slot ids. The caller must hold the mutex that guards
   // `mixer`.
   void publish(const Mixer& mixer);

   // True while `voiceId` occupies a slot in the last published snapshot.
   // Takes no lock.
   bool isActive(int voiceId) const;

   private:
   std::atomic<int> slotIds_[Mixer::kMaxVoices];
};

}  // namespace oxygine
