// What to tell the user about an error of a login step. Kept apart from the view so that tests can read it.
#pragma once

#include <string>

namespace Contrail::Td {

enum class LoginError {
  Flood,          // Too many attempts.
  BadPhone,
  BannedPhone,
  BadCode,
  ExpiredCode,    // The step has to start again from the phone number.
  BadPassword,
  BadName,
  Other,          // Shown as TDLib wrote it.
};

[[nodiscard]] inline LoginError ClassifyLoginError(int code, const std::string &message) {
  const auto has = [&](const char *text) { return message.find(text) != std::string::npos; };
  if (code == 429 || has("FLOOD_WAIT") || has("Too Many Requests")) return LoginError::Flood;
  if (has("PHONE_NUMBER_BANNED")) return LoginError::BannedPhone;
  if (has("PHONE_NUMBER_INVALID") || has("PHONE_NUMBER_FLOOD")) return LoginError::BadPhone;
  if (has("PHONE_CODE_EXPIRED")) return LoginError::ExpiredCode;
  if (has("PHONE_CODE_EMPTY") || has("PHONE_CODE_INVALID") || has("EMAIL_CODE_INVALID")) return LoginError::BadCode;
  if (has("PASSWORD_HASH_INVALID")) return LoginError::BadPassword;
  if (has("FIRSTNAME_INVALID") || has("LASTNAME_INVALID")) return LoginError::BadName;
  return LoginError::Other;
}

}  // namespace Contrail::Td
