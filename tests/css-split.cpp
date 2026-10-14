// The stylesheet's dark rules for GTK before 4.20 (gtk-ui/css_split.hpp), checked against style.css itself.
#include "css_split.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char **argv) {
  using Contrail::SplitDarkRules;
  const auto [base, dark] = SplitDarkRules(
      "a { color: red; }\n@media (prefers-color-scheme: dark) {\n  a { color: blue; }\n  b { x: y; }\n}\nc { }\n");
  assert(base == "a { color: red; }\n\nc { }\n");
  assert(dark == "\n  a { color: blue; }\n  b { x: y; }\n\n");
  assert(SplitDarkRules("plain {}").first == "plain {}");
  assert(SplitDarkRules("plain {}").second.empty());

  std::ifstream file(argc > 1 ? argv[1] : "gtk-ui/style.css");
  std::stringstream css;
  css << file.rdbuf();
  const auto [rest, rules] = SplitDarkRules(css.str());
  assert(!rules.empty());
  assert(rest.find("@media") == std::string::npos);
  // Every brace stays paired on both sides.
  for (const auto &part : {rest, rules}) {
    auto depth = 0;
    for (const auto c : part) {
      depth += (c == '{') - (c == '}');
      assert(depth >= 0);
    }
    assert(depth == 0);
  }
  std::puts("CSS split for GTK before 4.20: passed");
}
