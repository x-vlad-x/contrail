// The interface's strings, from Telegram Desktop's language pack through TDLib, in the user's language.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>

namespace Contrail::Td {
class Client;
class Lifetime;
}  // namespace Contrail::Td

namespace Contrail {

using TrArgument = std::pair<const char *, std::string>;

// The language pack id for the system's language: "ru", "pt-br". English when it can't be told.
[[nodiscard]] std::string SystemLanguage();

// Asks TDLib for every string the interface uses, once its options are set. Until the answer, and for whatever it
// lacks, the built-in English stays. `done` runs either way.
void LoadStrings(Td::Client &client, const Td::Lifetime &lifetime, const std::string &language,
                 std::function<void()> done);

// The string of a key.
[[nodiscard]] std::string Tr(const char *key);
// With {name} replaced by its value.
[[nodiscard]] std::string Tr(const char *key, std::initializer_list<TrArgument> arguments);
// The plural form for count, with {count} replaced by it.
[[nodiscard]] std::string TrCount(const char *key, std::int64_t count, std::initializer_list<TrArgument> arguments = {});

// Text the language pack doesn't have.
[[nodiscard]] std::string Translated(const char *english, const char *russian, const char *ukrainian);

// For tests: the form of the language's rule for count. 0 zero, 1 one, 2 two, 3 few, 4 many, 5 other.
[[nodiscard]] int PluralForm(const std::string &language, std::int64_t count);

}  // namespace Contrail
