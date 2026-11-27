#include "td/strings.hpp"

#include "td/client.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "td/plural_rules.inc"

namespace Contrail {
namespace {

struct BuiltIn {
  const char *key;
  const char *value;   // The value, or the "other" form of a plural.
  const char *single;  // The "one" form of a plural, null for a plain string.
};

constexpr BuiltIn kBuiltIn[] = {
#include "td/strings_en.inc"
};

struct Entry {
  // Indexed by the forms of plural_rules.inc; a plain string only has its value in kShiftOther.
  std::array<std::string, 6> forms;
  bool plural = false;
};

struct Pack {
  std::string language = "en";
  ChoosePluralMethod rule = nullptr;
  std::unordered_map<std::string, Entry> strings;
};

Pack &Current() {
  static Pack pack;
  return pack;
}

const Entry *Find(const char *key) {
  auto &strings = Current().strings;
  if (strings.empty()) {
    for (const auto &item : kBuiltIn) {
      auto entry = Entry();
      entry.forms[kShiftOther] = item.value;
      if (item.single) {
        entry.plural = true;
        entry.forms[kShiftOne] = item.single;
      }
      strings.emplace(item.key, std::move(entry));
    }
  }
  const auto it = strings.find(key);
  return it == strings.end() ? nullptr : &it->second;
}

ChoosePluralMethod RuleFor(const std::string &language) {
  auto id = language;
  std::ranges::transform(id, id.begin(), [](unsigned char c) { return c == '_' ? '-' : char(std::tolower(c)); });
  if (const auto it = kPluralRules.find(id); it != kPluralRules.end()) return it->second;
  // "pt-br" falls back to "pt", and an unknown language to the rule without forms.
  if (const auto dash = id.find('-'); dash != std::string::npos) {
    if (const auto it = kPluralRules.find(id.substr(0, dash)); it != kPluralRules.end()) return it->second;
  }
  return ChoosePlural1;
}

int Form(ChoosePluralMethod rule, std::int64_t count) {
  const auto value = int(std::min<std::int64_t>(std::llabs(count), 2147483647));
  return rule(value, value, 0, 0, 0, 0);
}

std::string Substitute(std::string text, const char *name, const std::string &value) {
  const auto tag = "{" + std::string(name) + "}";
  for (auto at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size())) {
    text.replace(at, tag.size(), value);
  }
  return text;
}

}  // namespace

std::string SystemLanguage() {
  const auto names = g_get_language_names();
  for (auto name = names; name && *name; ++name) {
    auto value = std::string(*name);
    if (value.empty() || value == "C" || value == "POSIX") continue;
    value = value.substr(0, value.find_first_of(".@"));
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return c == '_' ? '-' : char(std::tolower(c)); });
    // Language packs only tell regions apart for Portuguese and Chinese.
    if (value.starts_with("pt-br")) return "pt-br";
    if (value.starts_with("zh")) return value.find("tw") != std::string::npos ? "zh-hant" : "zh-hans";
    return value.substr(0, value.find('-'));
  }
  return "en";
}

void LoadStrings(Td::Client &client, const Td::Lifetime &lifetime, const std::string &language,
                 std::function<void()> done) {
  namespace api = Td::api;
  Find("");  // The built-in table: the answer replaces what it covers.
  auto &pack = Current();
  pack.language = language;
  pack.rule = RuleFor(language);
  if (language == "en") {
    done();
    return;
  }
  auto keys = std::vector<std::string>();
  keys.reserve(std::size(kBuiltIn));
  for (const auto &item : kBuiltIn) keys.emplace_back(item.key);
  client.request<api::languagePackStrings>(
      api::make_object<api::getLanguagePackStrings>(language, std::move(keys)), lifetime,
      [done](api::languagePackStrings &result) {
        for (auto &item : result.strings_) {
          if (!item || !item->value_) continue;
          auto entry = Entry();
          if (item->value_->get_id() == api::languagePackStringValueOrdinary::ID) {
            entry.forms[kShiftOther] = static_cast<api::languagePackStringValueOrdinary &>(*item->value_).value_;
          } else if (item->value_->get_id() == api::languagePackStringValuePluralized::ID) {
            auto &value = static_cast<api::languagePackStringValuePluralized &>(*item->value_);
            entry.plural = true;
            entry.forms = {value.zero_value_, value.one_value_, value.two_value_, value.few_value_, value.many_value_,
                           value.other_value_};
          } else {
            continue;  // Deleted: the built-in one stays.
          }
          Current().strings.insert_or_assign(item->key_, std::move(entry));
        }
        done();
      },
      [done](api::error &error) {
        g_warning("Strings: %s", error.message_.c_str());
        done();
      });
}

std::string Tr(const char *key) {
  const auto entry = Find(key);
  return entry ? entry->forms[kShiftOther] : std::string(key);
}

std::string Tr(const char *key, std::initializer_list<TrArgument> arguments) {
  auto text = Tr(key);
  for (const auto &[name, value] : arguments) text = Substitute(std::move(text), name, value);
  return text;
}

std::string TrCount(const char *key, std::int64_t count, std::initializer_list<TrArgument> arguments) {
  const auto entry = Find(key);
  if (!entry) return key;
  auto &pack = Current();
  const auto form = Form(pack.rule ? pack.rule : RuleFor(pack.language), count);
  auto text = entry->forms[size_t(form)];
  // TDLib leaves a form empty when the language doesn't use it.
  if (text.empty()) text = entry->forms[kShiftOther];
  text = Substitute(std::move(text), "count", std::to_string(count));
  for (const auto &[name, value] : arguments) text = Substitute(std::move(text), name, value);
  return text;
}

std::string Translated(const char *english, const char *russian, const char *ukrainian) {
  const auto &language = Current().language;
  return language == "ru" ? russian : language == "uk" ? ukrainian : english;
}

int PluralForm(const std::string &language, std::int64_t count) {
  return Form(RuleFor(language), count);
}

}  // namespace Contrail
