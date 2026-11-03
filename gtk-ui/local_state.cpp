#include "local_state.hpp"

#include "td/session.hpp"

#include <glib.h>

#include <string>

namespace Contrail {
namespace {

constexpr char kHiddenPinned[] = "hidden-pinned";

[[nodiscard]] const std::string &StatePath() {
  static const auto result = Td::DataDirectory() + "/contrail-state.ini";
  return result;
}

// Read once, written whole on every change: it holds a few lines.
[[nodiscard]] GKeyFile *State() {
  static auto *file = [] {
    auto *result = g_key_file_new();
    g_key_file_load_from_file(result, StatePath().c_str(), G_KEY_FILE_NONE, nullptr);
    return result;
  }();
  return file;
}

void Save() {
  GError *error = nullptr;
  if (!g_key_file_save_to_file(State(), StatePath().c_str(), &error)) {
    g_warning("State not saved: %s", error->message);
    g_error_free(error);
  }
}

[[nodiscard]] std::string ChatKey(std::int64_t chat, std::int32_t topic) {
  return topic ? std::to_string(chat) + "-" + std::to_string(topic) : std::to_string(chat);
}

}  // namespace

std::int64_t HiddenPinnedMessage(std::int64_t chat, std::int32_t topic) {
  return g_key_file_get_int64(State(), kHiddenPinned, ChatKey(chat, topic).c_str(), nullptr);
}

void SetHiddenPinnedMessage(std::int64_t chat, std::int32_t topic, std::int64_t message) {
  const auto key = ChatKey(chat, topic);
  if (message) {
    g_key_file_set_int64(State(), kHiddenPinned, key.c_str(), message);
  } else if (!g_key_file_remove_key(State(), kHiddenPinned, key.c_str(), nullptr)) {
    return;
  }
  Save();
}

}  // namespace Contrail
