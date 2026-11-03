// What the interface remembers on this device between runs, beside TDLib's database: a small key file there.
#pragma once

#include <cstdint>
#include <string>

namespace Contrail {

// The pinned message that was the newest when the bar was hidden in a chat (or its topic), 0 if it never was. The
// bar stays hidden until another message is pinned there, as in Telegram Desktop.
[[nodiscard]] std::int64_t HiddenPinnedMessage(std::int64_t chat, std::int32_t topic);
void SetHiddenPinnedMessage(std::int64_t chat, std::int32_t topic, std::int64_t message);

// The settings page's switches. The defaults are Telegram Desktop's, as 0.3.1 had them.
struct AppSettings {
  bool notify = true;       // Notifications at all.
  bool sound = true;        // With the system's message sound.
  bool preview = true;      // The text of the message in them, or only who wrote.
  bool background = false;  // Closing the window keeps the app running: messages, notifications and calls come.
};
[[nodiscard]] const AppSettings &Settings();
void SetSettings(const AppSettings &settings);

// Contrail 0.3 (on Telegram Desktop) left its data beside this one: its switches and language were carried over at
// the first start (from contrail-data/tdata into [settings]); the language pack it used, empty for the system's.
[[nodiscard]] std::string ChosenLanguage();
// It had a signed-in account, which can't come along: the login page says why the user signs in again. Forgotten
// once signed in.
[[nodiscard]] bool SignedInBeforeTdlib();
void ForgetSignedInBeforeTdlib();

}  // namespace Contrail
