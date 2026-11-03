#include <glib.h>
#include "call_backend.hpp"
#undef NDEBUG
#include <cassert>
#include <cstdio>

using namespace Contrail;
namespace api = Td::api;

struct Script final : Td::Transport {
  Sink sink;
  int discarded = 0;
  int discardId = 0;
  bool muted = false;
  void start(Sink value) override { sink = std::move(value); }
  void send(std::uint64_t id, Td::FunctionPtr function) override {
    switch (function->get_id()) {
      case api::getProxies::ID: {
        auto list = api::make_object<api::addedProxies>();
        auto proxy = api::make_object<api::addedProxy>();
        proxy->is_enabled_ = true;
        proxy->proxy_ = api::make_object<api::proxy>("localhost", 1080,
            api::make_object<api::proxyTypeSocks5>("user", "password"));
        list->proxies_.push_back(std::move(proxy));
        sink(id, std::move(list));
        break;
      }
      case api::createCall::ID:
        // TDLib can send the update before the response to createCall.
        update(10, true, api::make_object<api::callStatePending>(true, true));
        sink(id, api::make_object<api::callId>(10));
        break;
      case api::discardCall::ID:
        ++discarded;
        discardId = static_cast<api::discardCall &>(*function).call_id_;
        sink(id, api::make_object<api::ok>());
        break;
      default: sink(id, api::make_object<api::ok>()); break;
    }
  }
  void update(int id, bool outgoing, api::object_ptr<api::CallState> state) {
    auto call = api::make_object<api::call>();
    call->id_ = id; call->user_id_ = 2; call->is_outgoing_ = outgoing; call->state_ = std::move(state);
    sink(0, api::make_object<api::updateCall>(std::move(call)));
  }
};

int main() {
  g_setenv("CONTRAIL_SCENARIO", "1", true);
  auto script = std::make_unique<Script>();
  auto *raw = script.get();
  Td::Client client(std::move(script));
  Calls::Parameters negotiated;
  CallBackend backend(client, [&](Calls::Parameters parameters, Calls::Callbacks callbacks) {
    negotiated = parameters;
    return Calls::MakeScenarioMedia(std::move(parameters), std::move(callbacks));
  });
  using S = CallBackend::Stage;
  backend.start(2);
  assert(backend.snapshot().id == 10);
  assert(backend.snapshot().stage == S::Ringing);
  raw->update(11, false, api::make_object<api::callStatePending>(true, true));
  assert(raw->discarded == 1 && raw->discardId == 11);
  assert(backend.snapshot().id == 10);
  backend.hangup();
  assert(raw->discarded == 2 && raw->discardId == 10);
  assert(backend.snapshot().stage == S::HangingUp);
  raw->update(10, true, api::make_object<api::callStateDiscarded>(
      api::make_object<api::callDiscardReasonHungUp>(), false, false, false));
  assert(!backend.active());
  raw->update(12, false, api::make_object<api::callStatePending>(true, true));
  assert(backend.snapshot().stage == S::Incoming);
  backend.accept();
  assert(backend.snapshot().stage == S::Exchanging);
  raw->sink(0, api::make_object<api::updateOption>("call_connect_timeout_ms",
      api::make_object<api::optionValueInteger>(15000)));
  auto ready = api::make_object<api::callStateReady>();
  ready->protocol_ = api::make_object<api::callProtocol>(true, true, 65, 92, std::vector<std::string>{"13.0.0"});
  ready->encryption_key_ = std::string(256, 'a');
  ready->servers_.push_back(api::make_object<api::callServer>(42, "127.0.0.1", "::1", 3478,
      api::make_object<api::callServerTypeTelegramReflector>(std::string(16, 't'), true)));
  ready->emojis_ = {"😀", "🍀", "🚀", "🐾"};
  raw->sink(0, api::make_object<api::updateNewCallSignalingData>(12, "remote-muted"));
  raw->update(12, false, std::move(ready));
  assert(backend.snapshot().stage == S::Connecting);
  assert(negotiated.connectTimeout == 15.);
  assert(negotiated.proxy.host == "localhost" && negotiated.proxy.port == 1080);
  assert(negotiated.proxy.username == "user");
  assert(backend.snapshot().remoteMuted);
  assert(negotiated.key == std::string(256, 'a'));
  assert(negotiated.servers.size() == 1 && negotiated.servers[0].tcp);
  assert(negotiated.servers[0].peerTag == std::string(16, 't'));
  assert(backend.snapshot().emojis == "😀 🍀 🚀 🐾");
  backend.setMuted(true);
  assert(backend.snapshot().muted);
  backend.setCamera("scenario-back");
  assert(backend.snapshot().camera == "scenario-back");
  raw->sink(0, api::make_object<api::updateNewCallSignalingData>(12, "reconnecting"));
  assert(backend.snapshot().stage == S::Reconnecting);
  raw->sink(0, api::make_object<api::updateNewCallSignalingData>(12, "established"));
  assert(backend.snapshot().stage == S::Established);
  raw->sink(0, api::make_object<api::updateAuthorizationState>(api::make_object<api::authorizationStateClosed>()));
  assert(!backend.active() && backend.snapshot().stage == S::Ended);
  raw->update(14, false, api::make_object<api::callStatePending>(true, true));
  auto failureReady = api::make_object<api::callStateReady>();
  failureReady->protocol_ = api::make_object<api::callProtocol>(true, true, 65, 92, std::vector<std::string>{"13.0.0"});
  failureReady->encryption_key_ = std::string(256, 'b');
  raw->update(14, false, std::move(failureReady));
  raw->sink(0, api::make_object<api::updateNewCallSignalingData>(14, "failed"));
  assert(!backend.active() && backend.snapshot().stage == S::Failed);
  // An old terminal update must not replace the last call or open a window.
  raw->update(13, true, api::make_object<api::callStateError>(api::make_object<api::error>(400, "old")));
  assert(backend.snapshot().id == 14);
  // Cancelling before createCall has returned must discard the call once its id is known.
  backend.setChanged([&] {
    if (backend.snapshot().stage == S::Requesting && !backend.snapshot().id) backend.hangup();
  });
  const auto before = raw->discarded;
  backend.start(2);
  assert(raw->discarded == before + 1 && raw->discardId == 10);
  assert(backend.snapshot().stage == S::HangingUp);
  std::puts("call-test: PASS");
}
