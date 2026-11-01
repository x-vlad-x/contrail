// Dates and times as the interface shows them, in the user's time zone and language.
#pragma once

#include <cstdint>
#include <string>

namespace Contrail::Td {

[[nodiscard]] std::string FormatTime(std::int32_t date);  // 12:34, or 12:34 PM where the locale says so.
[[nodiscard]] std::string FormatDay(std::int32_t date);   // "October 13", with the year for past years.
// A number that is the same for two dates of one local day, and grows by one a day or more with the next.
[[nodiscard]] std::int64_t DayNumber(std::int32_t date);
// Seconds since the epoch.
[[nodiscard]] std::int32_t Now();

}  // namespace Contrail::Td
