// Tells TDLib when the phone's network changes, so that it reconnects at once instead of after its timeouts.
#pragma once

#include "td/client.hpp"

#include <gio/gio.h>

#include <string>

namespace Contrail {

// GLib's network monitor (NetworkManager, or the portal in a Flatpak): no network, a metered one (mobile data) or
// another. Each change reopens TDLib's connections (setNetworkType); without a network it stops trying, which saves
// the battery in airplane mode.
class NetworkWatch final {
 public:
  explicit NetworkWatch(Td::Client &client);
  ~NetworkWatch();
  NetworkWatch(const NetworkWatch &) = delete;
  NetworkWatch &operator=(const NetworkWatch &) = delete;

  void debugState() const;
  // As if the network became "none", "mobile" or "wifi".
  void debugSimulate(const std::string &kind);

 private:
  enum class Kind { Unknown, None, Mobile, WiFi };
  void changed();
  void apply(Kind kind);

  Td::Client &_client;
  GNetworkMonitor *_monitor = nullptr;
  gulong _handler = 0;
  Kind _kind = Kind::Unknown;
  int _sent = 0;
};

}  // namespace Contrail
