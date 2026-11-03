#include "call_media.hpp"
#include "call_tones.hpp"

namespace Contrail::Calls {
std::unique_ptr<Media> StartMedia(Parameters, Callbacks) { return nullptr; }
std::vector<std::string> Versions() { return {}; }
int MaxLayer() { return 92; }
std::vector<Camera> Cameras() { return {}; }
void WaitForStoppedMedia() {}
std::unique_ptr<TonePlayer> MakeTonePlayer() { return nullptr; }
}  // namespace Contrail::Calls
