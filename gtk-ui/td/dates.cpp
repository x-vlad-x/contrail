#include "td/dates.hpp"

#include "td/strings.hpp"

#include <glib.h>

#include <langinfo.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace Contrail::Td {
namespace {

tm Local(std::int32_t date) {
  const auto value = time_t(date);
  auto result = tm();
  localtime_r(&value, &result);
  return result;
}

bool Clock12Hours() {
  static const auto result = [] {
    const auto *format = nl_langinfo(T_FMT);
    return format && (std::strstr(format, "%p") || std::strstr(format, "%I") || std::strstr(format, "%r"));
  }();
  return result;
}

}  // namespace

std::string FormatTime(std::int32_t date) {
  const auto local = Local(date);
  auto buffer = std::array<char, 32>();
  std::strftime(buffer.data(), buffer.size(), Clock12Hours() ? "%l:%M %p" : "%H:%M", &local);
  auto *start = buffer.data();
  while (*start == ' ') ++start;
  return start;
}

std::string FormatDay(std::int32_t date) {
  static const char *const kMonths[] = {"lng_month_day1", "lng_month_day2", "lng_month_day3", "lng_month_day4",
                                        "lng_month_day5", "lng_month_day6", "lng_month_day7", "lng_month_day8",
                                        "lng_month_day9", "lng_month_day10", "lng_month_day11", "lng_month_day12"};
  const auto local = Local(date);
  const auto thisYear = Local(Now()).tm_year;
  const auto month = Tr(kMonths[local.tm_mon]);
  const auto day = std::to_string(local.tm_mday);
  if (local.tm_year == thisYear) return Tr("lng_month_day", {{"month", month}, {"day", day}});
  return Tr("lng_month_day_year", {{"month", month}, {"day", day}, {"year", std::to_string(local.tm_year + 1900)}});
}

std::string FormatListDate(std::int32_t date) {
  static const char *const kWeekdays[] = {"lng_weekday7", "lng_weekday1", "lng_weekday2", "lng_weekday3",
                                          "lng_weekday4", "lng_weekday5", "lng_weekday6"};
  constexpr auto kRecentSeconds = 20 * 3600;
  const auto now = Now();
  const auto local = Local(date);
  const auto today = Local(now);
  const auto sameDay = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday;
  if (sameDay || std::abs(now - date) < kRecentSeconds) return FormatTime(date);
  // Whole days between the two local dates; DST makes plain seconds off by an hour.
  auto first = local;
  auto second = today;
  first.tm_hour = second.tm_hour = 12;
  first.tm_min = second.tm_min = first.tm_sec = second.tm_sec = 0;
  const auto days = std::llround(std::difftime(mktime(&second), mktime(&first)) / 86400.);
  if (std::llabs(days) < 7) return Tr(kWeekdays[local.tm_wday]);
  auto buffer = std::array<char, 32>();
  std::strftime(buffer.data(), buffer.size(), "%x", &local);
  return buffer.data();
}

std::int64_t DayNumber(std::int32_t date) {
  const auto local = Local(date);
  return (local.tm_year + 1900) * 372 + local.tm_mon * 31 + local.tm_mday;
}

std::int32_t Now() {
  return std::int32_t(time(nullptr));
}

}  // namespace Contrail::Td
