// One TDLib client from start to close: its parameters, options and authorization state.
#pragma once

#include "td/client.hpp"

#include <functional>
#include <memory>
#include <string>

namespace Contrail::Td {

struct SessionConfig {
  std::string directory;  // Database and files of this client.
  std::string language;   // Language pack id.
  std::string version;    // Shown in the list of the account's sessions.
};

class Session final {
 public:
  using AuthorizationHandler = std::function<void(api::AuthorizationState &state)>;

  Session(std::unique_ptr<Transport> transport, SessionConfig config);

  [[nodiscard]] Client &client() { return _client; }
  // Called for every state after setup; the first call says whether a login is needed.
  void setAuthorizationHandler(AuthorizationHandler handler) { _handler = std::move(handler); }
  // Telegram ended the session (or the user logged out) and TDLib has wiped its data. Comes after the Closed state.
  void setClosedHandler(std::function<void(bool lost)> handler) { _closed = std::move(handler); }
  // The user asked to log out: the end of the session is then not a loss.
  void logOut();
  // Tells Telegram whether the user is here.
  void setOnline(bool online);

 private:
  void updated(api::Update &update);
  void authorization(api::AuthorizationState &state);
  void sendParameters();

  Client _client;
  SessionConfig _config;
  Lifetime _lifetime;
  AuthorizationHandler _handler;
  std::function<void(bool lost)> _closed;
  bool _loggingOutByUser = false;
  bool _lost = false;
};

// Sends TDLib's own log to <directory>.log. Before the first client.
void SetUpLog(const std::string &directory);

// Where this program keeps its data: separate from the stable Contrail, which keeps working next to it.
[[nodiscard]] std::string DataDirectory();

}  // namespace Contrail::Td
