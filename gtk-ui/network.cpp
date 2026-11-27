#include "network.hpp"

namespace Contrail {
namespace {

namespace api = Td::api;

const char *KindName(int kind) {
  switch (kind) {
    case 1: return "none";
    case 2: return "mobile";
    case 3: return "wifi";
    default: return "unknown";
  }
}

}  // namespace

NetworkWatch::NetworkWatch(Td::Client &client) : _client(client), _monitor(g_network_monitor_get_default()) {
  g_object_ref(_monitor);
  _handler = g_signal_connect_swapped(_monitor, "network-changed", G_CALLBACK(+[](NetworkWatch *self) {
    self->changed();
  }), this);
  // At the start TDLib finds the network by itself; only its absence is worth telling.
  if (!g_network_monitor_get_network_available(_monitor)) apply(Kind::None);
  else _kind = g_network_monitor_get_network_metered(_monitor) ? Kind::Mobile : Kind::WiFi;
}

NetworkWatch::~NetworkWatch() {
  g_signal_handler_disconnect(_monitor, _handler);
  g_object_unref(_monitor);
}

// The signal comes many times for one change (every route and address): only a different kind is sent.
void NetworkWatch::changed() {
  if (!g_network_monitor_get_network_available(_monitor)) apply(Kind::None);
  else apply(g_network_monitor_get_network_metered(_monitor) ? Kind::Mobile : Kind::WiFi);
}

void NetworkWatch::apply(Kind kind) {
  if (kind == _kind) return;
  _kind = kind;
  ++_sent;
  auto type = api::object_ptr<api::NetworkType>();
  switch (kind) {
    case Kind::None: type = api::make_object<api::networkTypeNone>(); break;
    case Kind::Mobile: type = api::make_object<api::networkTypeMobile>(); break;
    default: type = api::make_object<api::networkTypeWiFi>(); break;
  }
  _client.send(api::make_object<api::setNetworkType>(std::move(type)));
  g_message("network: %s", KindName(int(kind)));
}

void NetworkWatch::debugState() const {
  g_message("network: kind=%s sent=%d available=%d metered=%d", KindName(int(_kind)), _sent,
            int(g_network_monitor_get_network_available(_monitor)), int(g_network_monitor_get_network_metered(_monitor)));
}

void NetworkWatch::debugSimulate(const std::string &kind) {
  apply(kind == "none" ? Kind::None : kind == "mobile" ? Kind::Mobile : Kind::WiFi);
}

}  // namespace Contrail
