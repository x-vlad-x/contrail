// What Contrail 0.3 kept in Telegram Desktop's data folder (tdata) that the version on TDLib can carry over: the
// settings page's switches and the language. The account can't come along: TDLib can't open Telegram Desktop's
// session, so the user signs in again (the login page says why).
#pragma once

#include <optional>
#include <string>

namespace Contrail {

struct LegacySettings {
  bool notify = true;
  bool sound = true;
  bool preview = true;
  bool background = false;
  std::string language;  // The language pack Telegram Desktop used ("ru", "pt-br"), empty if unknown or custom.
  bool account = false;  // tdata had a signed-in account.
};

// Reads tdata the way Telegram Desktop does (its "settings" file, encrypted without a passcode), only reading.
// Nothing if the folder has no readable settings.
[[nodiscard]] std::optional<LegacySettings> ReadLegacySettings(const std::string &tdata);

}  // namespace Contrail
