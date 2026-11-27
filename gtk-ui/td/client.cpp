#include "td/client.hpp"

#include <td/telegram/Client.h>

#include <glib.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace Contrail::Td {
namespace {

// Long enough not to matter: the thread leaves receive() as soon as anything arrives.
constexpr auto kReceiveTimeout = 3600.;
// One pass over the queue gives the main loop back after this long, so that a burst of updates can't hold up drawing.
constexpr auto kDispatchMicroseconds = gint64(8000);
// For the transport's own requests. TDLib ignores a request with id 0, and the Client's ids count up from 1.
constexpr auto kInternalRequest = ~std::uint64_t(0);

class TdTransport final : public Transport {
 public:
  ~TdTransport() override {
    // The thread leaves on authorizationStateClosed; a close sent here wakes it if nothing closed the client yet.
    _manager.send(_client, kInternalRequest, api::make_object<api::close>());
    if (_thread.joinable()) _thread.join();
    auto lock = std::lock_guard(_shared->mutex);
    _shared->sink = nullptr;
    _shared->queue.clear();
  }

  void start(Sink sink) override {
    _shared->sink = std::move(sink);
    _client = _manager.create_client_id();
    // A client only starts working after its first request.
    _manager.send(_client, kInternalRequest, api::make_object<api::getOption>("version"));
    _thread = std::thread([this] { receive(); });
  }

  void send(std::uint64_t id, FunctionPtr function) override { _manager.send(_client, id, std::move(function)); }

 private:
  struct Item {
    std::uint64_t id = 0;
    ObjectPtr object;
  };
  // Outlives the transport while an idle callback is queued.
  struct Shared {
    std::mutex mutex;
    std::deque<Item> queue;
    bool scheduled = false;
    Sink sink;
  };

  void receive() {
    while (true) {
      auto response = _manager.receive(kReceiveTimeout);
      if (!response.object) continue;
      const auto closed = response.object->get_id() == api::updateAuthorizationState::ID
          && static_cast<api::updateAuthorizationState &>(*response.object).authorization_state_->get_id()
              == api::authorizationStateClosed::ID;
      post(response.request_id, std::move(response.object));
      if (closed) return;
    }
  }

  void post(std::uint64_t id, ObjectPtr object) {
    auto lock = std::lock_guard(_shared->mutex);
    _shared->queue.push_back({id, std::move(object)});
    if (_shared->scheduled) return;
    _shared->scheduled = true;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &Dispatch, new std::shared_ptr<Shared>(_shared),
                    [](gpointer data) { delete static_cast<std::shared_ptr<Shared> *>(data); });
  }

  static gboolean Dispatch(gpointer data) {
    const auto shared = *static_cast<std::shared_ptr<Shared> *>(data);
    const auto deadline = g_get_monotonic_time() + kDispatchMicroseconds;
    while (true) {
      auto item = Item();
      auto sink = Sink();
      {
        auto lock = std::lock_guard(shared->mutex);
        if (shared->queue.empty() || !shared->sink) {
          shared->scheduled = false;
          return G_SOURCE_REMOVE;
        }
        item = std::move(shared->queue.front());
        shared->queue.pop_front();
        sink = shared->sink;
      }
      sink(item.id, std::move(item.object));
      if (g_get_monotonic_time() >= deadline) return G_SOURCE_CONTINUE;
    }
  }

  td::ClientManager _manager;
  td::ClientManager::ClientId _client = 0;
  std::shared_ptr<Shared> _shared = std::make_shared<Shared>();
  std::thread _thread;
};

}  // namespace

std::unique_ptr<Transport> MakeTdTransport() {
  return std::make_unique<TdTransport>();
}

Client::Client(std::unique_ptr<Transport> transport) : _transport(std::move(transport)) {
  _transport->start([this](std::uint64_t id, ObjectPtr object) { received(id, std::move(object)); });
}

void Client::send(FunctionPtr function, Handler handler, const Lifetime &lifetime) {
  const auto id = _nextId++;
  _pending.emplace(id, Pending{std::move(handler), lifetime.token(), true});
  _transport->send(id, std::move(function));
}

void Client::send(FunctionPtr function, Handler handler) {
  const auto id = _nextId++;
  if (handler) _pending.emplace(id, Pending{std::move(handler), {}, false});
  _transport->send(id, std::move(function));
}

void Client::subscribe(UpdateHandler handler, const Lifetime &lifetime) {
  _subscribers.push_back({std::move(handler), lifetime.token()});
}

void Client::reportError(api::error &error) {
  if (_errorHandler) _errorHandler(error);
  else g_warning("TDLib error %d: %s", int(error.code_), error.message_.c_str());
}

void Client::received(std::uint64_t id, ObjectPtr object) {
  if (id) {
    const auto it = _pending.find(id);
    if (it == _pending.end()) return;
    auto pending = std::move(it->second);
    _pending.erase(it);
    if (pending.handler && (!pending.tracked || !pending.token.expired())) pending.handler(std::move(object));
    return;
  }
  if (!object) return;
  auto &update = static_cast<api::Update &>(*object);
  // A handler can subscribe or end a lifetime while this runs: only those present now see this update.
  const auto count = _subscribers.size();
  for (auto i = std::size_t(0); i < count; ++i) {
    if (_subscribers[i].token.expired()) continue;
    const auto handler = _subscribers[i].handler;
    handler(update);
  }
  std::erase_if(_subscribers, [](const Subscriber &subscriber) { return subscriber.token.expired(); });
}

int FloodWaitSeconds(const std::string &message) {
  constexpr std::string_view prefix = "FLOOD_WAIT_";
  if (!message.starts_with(prefix)) return 0;
  return std::atoi(message.c_str() + prefix.size());
}

}  // namespace Contrail::Td
