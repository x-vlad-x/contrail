#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace Contrail {

// GTK before 4.20 knows no media queries: the "prefers-color-scheme: dark" blocks of style.css go into a second
// stylesheet that is added while libadwaita's style is dark. Returns {the rest, the dark rules}.
inline std::pair<std::string, std::string> SplitDarkRules(std::string_view css) {
  constexpr auto kDark = std::string_view("@media (prefers-color-scheme: dark) {");
  auto base = std::string(), dark = std::string();
  auto position = size_t(0);
  while (true) {
    const auto start = css.find(kDark, position);
    if (start == std::string_view::npos) {
      base += css.substr(position);
      break;
    }
    base += css.substr(position, start - position);
    auto i = start + kDark.size();
    const auto body = i;
    for (auto depth = 1; i < css.size() && depth > 0; ++i) {
      if (css[i] == '{') ++depth;
      else if (css[i] == '}') --depth;
    }
    dark += css.substr(body, i - 1 - body);
    dark += '\n';
    position = i;
  }
  return {base, dark};
}

}  // namespace Contrail
