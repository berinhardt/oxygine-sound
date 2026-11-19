#include "VoiceActivity.h"

namespace oxygine {

VoiceActivity::VoiceActivity() {
   for (int i = 0; i < Mixer::kMaxVoices; ++i) {
      slotIds_[i].store(0, std::memory_order_relaxed);
   }
}

void VoiceActivity::publish(const Mixer& mixer) {
   for (int i = 0; i < Mixer::kMaxVoices; ++i) {
      slotIds_[i].store(mixer.voiceIdAt(i), std::memory_order_relaxed);
   }
}

bool VoiceActivity::isActive(int voiceId) const {
   if (voiceId <= 0) return false;

   for (int i = 0; i < Mixer::kMaxVoices; ++i) {
      if (slotIds_[i].load(std::memory_order_relaxed) == voiceId) return true;
   }
   return false;
}

}  // namespace oxygine
