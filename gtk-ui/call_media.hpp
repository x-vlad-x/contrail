// The media of a 1:1 call: sound, video and the connection to the other side. TDLib only carries the signaling
// and gives the keys and servers; tgcalls (on Telegram's WebRTC, tg_owt) does the rest. Nothing of tgcalls or TDLib
// shows here, so the interface builds without them, and the scenario has media of its own (see MakeScenarioMedia).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Contrail::Calls {

// A relay or a STUN/TURN server of callStateReady, as TDLib gives it.
struct Server {
  std::int64_t id = 0;
  std::string ipv4;
  std::string ipv6;
  int port = 0;
  bool reflector = false;  // Telegram's own relay; otherwise a WebRTC server.
  std::string peerTag;     // A reflector's, 16 bytes.
  bool tcp = false;        // A reflector over TCP.
  bool stun = false;
  bool turn = false;
  std::string username;
  std::string password;
};

struct Proxy {
  std::string host;
  int port = 0;
  std::string username;
  std::string password;
};

struct Parameters {
  std::string version;      // The protocol both sides chose (callStateReady.protocol.library_versions[0]).
  std::string key;          // 256 bytes.
  bool outgoing = false;
  bool allowP2p = true;
  int maxLayer = 92;
  std::vector<Server> servers;
  std::string customParameters;  // JSON from the server.
  bool video = false;            // Starts with the camera on.
  std::string camera;            // Which camera, empty: the first.
  std::string statsLogPath;      // tgcalls writes the call's routes there when it stops.
  double connectTimeout = 30.;
  double packetTimeout = 10.;
  Proxy proxy;
};

enum class State { Connecting, Established, Reconnecting, Failed };

// A picture to show: BGRA (GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, opaque), already turned upright and mirrored where
// it should be.
struct Frame {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

struct Camera {
  std::string id;
  std::string name;
};

// Everything here comes on the main thread.
struct Callbacks {
  std::function<void(State state)> state;
  std::function<void(int bars)> signalBars;  // 0 to 4.
  std::function<void(bool muted, bool video)> remoteMedia;
  std::function<void(std::string data)> signaling;  // To sendCallSignalingData.
  std::function<void(Frame frame)> remoteFrame;
  std::function<void(Frame frame)> localFrame;
  std::function<void()> cameraFailed;
  std::function<void()> videoStopped;        // The other side's video went (or paused).
};

class Media {
 public:
  virtual ~Media() = default;

  virtual void receiveSignaling(const std::string &data) = 0;
  virtual void setMuted(bool muted) = 0;
  // A device name of OpenAL (the sink's description), empty for the system's default.
  virtual void setOutputDevice(const std::string &device) = 0;
  // Empty: the camera goes off.
  virtual void setCamera(const std::string &id) = 0;
  [[nodiscard]] virtual std::int64_t preferredRelay() = 0;
  [[nodiscard]] virtual std::string lastError() = 0;
  // The sound module's counters, for the debug state.
  [[nodiscard]] virtual std::string soundStats() = 0;
  // Takes the call apart. tgcalls finishes this on its own threads afterwards: see WaitForStoppedMedia.
  virtual void stop() = 0;
};

// Null when the version is unknown to tgcalls.
[[nodiscard]] std::unique_ptr<Media> StartMedia(Parameters parameters, Callbacks callbacks);
// Made-up media for the scenario: no network and no sound, frames of a pattern for the video.
[[nodiscard]] std::unique_ptr<Media> MakeScenarioMedia(Parameters parameters, Callbacks callbacks);

// The protocol versions tgcalls speaks, newest first, and the layer to tell TDLib.
[[nodiscard]] std::vector<std::string> Versions();
[[nodiscard]] int MaxLayer();

// The cameras, front first on a phone; only the phone's own cameras when it has them.
[[nodiscard]] std::vector<Camera> Cameras();

// tgcalls goes on taking a stopped call apart on its threads, with no signal at the end, and the program's static
// destructors must not run under them. Called before exit: waits a little after a call that stopped just now.
void WaitForStoppedMedia();

}  // namespace Contrail::Calls
