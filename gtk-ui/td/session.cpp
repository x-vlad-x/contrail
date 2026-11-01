#include "td/session.hpp"

#include "contrail_api.h"

#include <td/telegram/Client.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <string>

namespace Contrail::Td {
namespace {

template <typename Value, typename Argument>
void SetOption(Client &client, const char *name, Argument value) {
  client.send(api::make_object<api::setOption>(name, api::make_object<Value>(value)));
}

std::string FirstLine(const char *path) {
  auto *contents = static_cast<gchar *>(nullptr);
  if (!g_file_get_contents(path, &contents, nullptr, nullptr)) return {};
  auto result = std::string(contents);
  g_free(contents);
  const auto end = result.find_first_of(std::string("\n\0", 2));
  return end == std::string::npos ? result : result.substr(0, end);
}

// Shown in the account's list of sessions.
std::string DeviceModel() {
  for (const auto path : {"/sys/firmware/devicetree/base/model", "/sys/devices/virtual/dmi/id/product_name"}) {
    if (auto model = FirstLine(path); !model.empty()) return model;
  }
  return "Linux";
}

std::string SystemVersion() {
  auto *name = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
  auto result = std::string(name ? name : "Linux");
  g_free(name);
  return result;
}

}  // namespace

std::string DataDirectory() {
  if (const auto custom = g_getenv("CONTRAIL_TD_DIR")) return custom;
  auto *path = g_build_filename(g_get_user_data_dir(), "contrail-td", "app", nullptr);
  auto result = std::string(path);
  g_free(path);
  return result;
}

void SetUpLog(const std::string &directory) {
  // TDLib says why it lost a session only from level 2, and wipes its database then: the log lives beside it.
  auto *parent = g_path_get_dirname(directory.c_str());
  g_mkdir_with_parents(parent, 0700);
  g_free(parent);
  td::ClientManager::execute(api::make_object<api::setLogStream>(
      api::make_object<api::logStreamFile>(directory + ".log", 16 * 1024 * 1024, false)));
  td::ClientManager::execute(api::make_object<api::setLogVerbosityLevel>(2));
}

Session::Session(std::unique_ptr<Transport> transport, SessionConfig config)
    : _client(std::move(transport)), _config(std::move(config)) {
  _client.subscribe([this](api::Update &update) { updated(update); }, _lifetime);
}

void Session::logOut() {
  _loggingOutByUser = true;
  _client.send(api::make_object<api::logOut>());
}

void Session::setOnline(bool online) {
  SetOption<api::optionValueBoolean>(_client, "online", online);
}

void Session::updated(api::Update &update) {
  if (update.get_id() == api::updateAuthorizationState::ID) {
    authorization(*static_cast<api::updateAuthorizationState &>(update).authorization_state_);
  }
}

void Session::authorization(api::AuthorizationState &state) {
  switch (state.get_id()) {
    case api::authorizationStateWaitTdlibParameters::ID:
      sendParameters();
      return;
    case api::authorizationStateLoggingOut::ID:
      _lost = !_loggingOutByUser;
      break;
    case api::authorizationStateClosed::ID:
      if (_handler) _handler(state);
      if (_closed) _closed(_lost);
      return;
    default: break;
  }
  if (_handler) _handler(state);
}

void Session::sendParameters() {
  g_mkdir_with_parents(_config.directory.c_str(), 0700);
  // The strings are Telegram Desktop's, the same ones the interface has always used.
  SetOption<api::optionValueString>(_client, "language_pack_database_path", _config.directory + "/language.sqlite");
  SetOption<api::optionValueString>(_client, "localization_target", std::string("tdesktop"));
  SetOption<api::optionValueString>(_client, "language_pack_id", _config.language);
  SetOption<api::optionValueBoolean>(_client, "ignore_file_names", true);
  SetOption<api::optionValueBoolean>(_client, "use_storage_optimizer", true);
  SetOption<api::optionValueBoolean>(_client, "disable_top_chats", true);
  SetOption<api::optionValueBoolean>(_client, "disable_animated_emoji", true);

  auto request = api::make_object<api::setTdlibParameters>();
  request->database_directory_ = _config.directory;
  request->files_directory_ = _config.directory + "/files";
  // Without the databases TDLib would keep every message it has seen in memory.
  request->use_file_database_ = true;
  request->use_chat_info_database_ = true;
  request->use_message_database_ = true;
  request->use_secret_chats_ = false;
  request->api_id_ = kApiId;
  request->api_hash_ = kApiHash;
  request->system_language_code_ = _config.language;
  request->device_model_ = DeviceModel();
  request->system_version_ = SystemVersion();
  request->application_version_ = _config.version;
  _client.send(std::move(request));
}

}  // namespace Contrail::Td
