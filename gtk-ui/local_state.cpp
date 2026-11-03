#include "local_state.hpp"

#include "legacy_settings.hpp"
#include "td/session.hpp"

#include <glib.h>

#include <string>

namespace Contrail {
namespace {

constexpr char kHiddenPinned[] = "hidden-pinned";
constexpr char kSettings[] = "settings";

[[nodiscard]] const std::string &StatePath() {
  static const auto result = Td::DataDirectory() + "/contrail-state.ini";
  return result;
}

// The first start next to Contrail 0.3's data (contrail-data/tdata beside contrail-data/td): its settings page's
// switches and its language come along. Its folder is only read, so going back to 0.3 keeps working.
void CarryOverLegacySettings(GKeyFile *file) {
  auto *parent = g_path_get_dirname(Td::DataDirectory().c_str());
  auto *tdata = g_build_filename(parent, "tdata", nullptr);
  const auto legacy = ReadLegacySettings(tdata);
  g_free(tdata);
  g_free(parent);
  if (!legacy) return;
  g_key_file_set_boolean(file, kSettings, "notify", legacy->notify);
  g_key_file_set_boolean(file, kSettings, "sound", legacy->sound);
  g_key_file_set_boolean(file, kSettings, "preview", legacy->preview);
  g_key_file_set_boolean(file, kSettings, "background", legacy->background);
  if (!legacy->language.empty()) g_key_file_set_string(file, kSettings, "language", legacy->language.c_str());
  if (legacy->account) g_key_file_set_boolean(file, kSettings, "signed-in-before-tdlib", TRUE);
  g_message("Settings of Contrail 0.3 carried over");
}

// Read once, written whole on every change: it holds a few lines.
[[nodiscard]] GKeyFile *State() {
  static auto *file = [] {
    auto *result = g_key_file_new();
    const auto loaded = g_key_file_load_from_file(result, StatePath().c_str(), G_KEY_FILE_NONE, nullptr);
    if (!loaded && !g_file_test(StatePath().c_str(), G_FILE_TEST_EXISTS)) {
      CarryOverLegacySettings(result);
      if (g_key_file_has_group(result, kSettings)) {
        g_mkdir_with_parents(Td::DataDirectory().c_str(), 0700);
        auto *data = g_key_file_to_data(result, nullptr, nullptr);
        g_file_set_contents(StatePath().c_str(), data, -1, nullptr);
        g_free(data);
      }
    }
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

bool Flag(const char *key, bool fallback) {
  GError *error = nullptr;
  const auto value = g_key_file_get_boolean(State(), kSettings, key, &error);
  if (!error) return value;
  g_error_free(error);
  return fallback;
}

// Read once; the page changes it only through SetSettings.
AppSettings &CurrentSettings() {
  static auto result = [] {
    const auto defaults = AppSettings();
    return AppSettings{
        .notify = Flag("notify", defaults.notify),
        .sound = Flag("sound", defaults.sound),
        .preview = Flag("preview", defaults.preview),
        .background = Flag("background", defaults.background),
    };
  }();
  return result;
}

}  // namespace

const AppSettings &Settings() {
  return CurrentSettings();
}

void SetSettings(const AppSettings &settings) {
  CurrentSettings() = settings;
  g_key_file_set_boolean(State(), kSettings, "notify", settings.notify);
  g_key_file_set_boolean(State(), kSettings, "sound", settings.sound);
  g_key_file_set_boolean(State(), kSettings, "preview", settings.preview);
  g_key_file_set_boolean(State(), kSettings, "background", settings.background);
  Save();
}

std::string ChosenLanguage() {
  auto *value = g_key_file_get_string(State(), kSettings, "language", nullptr);
  auto result = std::string(value ? value : "");
  g_free(value);
  return result;
}

bool SignedInBeforeTdlib() {
  return Flag("signed-in-before-tdlib", false);
}

void ForgetSignedInBeforeTdlib() {
  if (g_key_file_remove_key(State(), kSettings, "signed-in-before-tdlib", nullptr)) Save();
}

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
