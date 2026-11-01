// The connection to TDLib. Everything here except TdTransport's receive thread runs on the main thread.
#pragma once

#include <td/telegram/td_api.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Contrail::Td {

namespace api = td::td_api;
using ObjectPtr = api::object_ptr<api::Object>;
using FunctionPtr = api::object_ptr<api::Function>;

// Held by whatever waits for an answer: when it goes away, the answer is dropped instead of reaching a closed view.
class Lifetime final {
 public:
  Lifetime() : _alive(std::make_shared<char>()) {}
  [[nodiscard]] std::weak_ptr<void> token() const { return _alive; }
  void reset() { _alive = std::make_shared<char>(); }

 private:
  std::shared_ptr<void> _alive;
};

// Where requests go and objects come from: TDLib itself, or a script in the tests. A sink is always called on the
// main thread. Id 0 marks an update.
class Transport {
 public:
  using Sink = std::function<void(std::uint64_t id, ObjectPtr object)>;

  virtual ~Transport() = default;
  virtual void start(Sink sink) = 0;
  virtual void send(std::uint64_t id, FunctionPtr function) = 0;
};

// Owns one TDLib client. A thread sits in receive() and hands everything to the GLib main loop: nothing polls, so
// a quiet connection wakes nothing up.
[[nodiscard]] std::unique_ptr<Transport> MakeTdTransport();

class Client final {
 public:
  using Handler = std::function<void(ObjectPtr object)>;
  using UpdateHandler = std::function<void(api::Update &update)>;

  explicit Client(std::unique_ptr<Transport> transport);

  // The answer comes on the main thread, unless the lifetime has ended by then. An error is an api::error.
  void send(FunctionPtr function, Handler handler, const Lifetime &lifetime);
  void send(FunctionPtr function, Handler handler = nullptr);

  // Typed answer: `done` gets the object, `failed` (optional) the error, with FLOOD_WAIT and the like already told
  // to the error handler set by setErrorHandler() when `failed` is empty.
  template <typename Result, typename Done>
  void request(FunctionPtr function, const Lifetime &lifetime, Done done,
               std::function<void(api::error &)> failed = nullptr) {
    send(std::move(function),
         [this, done = std::move(done), failed = std::move(failed)](ObjectPtr object) {
           if (object && object->get_id() == api::error::ID) {
             auto &error = static_cast<api::error &>(*object);
             if (failed) failed(error);
             else reportError(error);
           } else if (object && object->get_id() == Result::ID) {
             done(static_cast<Result &>(*object));
           }
         },
         lifetime);
  }

  // Every update, in the order TDLib sent them. The handler stays for the life of the lifetime.
  void subscribe(UpdateHandler handler, const Lifetime &lifetime);

  void setErrorHandler(std::function<void(api::error &)> handler) { _errorHandler = std::move(handler); }
  void reportError(api::error &error);

 private:
  struct Pending {
    Handler handler;
    std::weak_ptr<void> token;
    bool tracked = false;  // Otherwise the handler has no lifetime to check.
  };
  struct Subscriber {
    UpdateHandler handler;
    std::weak_ptr<void> token;
  };

  void received(std::uint64_t id, ObjectPtr object);

  std::unique_ptr<Transport> _transport;
  std::uint64_t _nextId = 1;
  std::map<std::uint64_t, Pending> _pending;
  std::vector<Subscriber> _subscribers;
  std::function<void(api::error &)> _errorHandler;
};

// The number of seconds of a FLOOD_WAIT_n error, 0 for any other.
[[nodiscard]] int FloodWaitSeconds(const std::string &message);

}  // namespace Contrail::Td
