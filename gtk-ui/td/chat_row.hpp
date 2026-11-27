// What a row of the chat list shows, worked out from the store. No widgets here, so tests can check it.
#pragma once

#include "td/store.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace Contrail::Td {

// The range of a list that differs: most changes move one chat, and the rows around it stay as they are.
struct ListChange {
  std::size_t first = 0;
  std::size_t removed = 0;
  std::size_t added = 0;
};

template <typename Item>
[[nodiscard]] ListChange DiffLists(const std::vector<Item> &before, const std::vector<Item> &after) {
  const auto limit = std::min(before.size(), after.size());
  auto first = std::size_t(0);
  while (first < limit && before[first] == after[first]) ++first;
  auto tail = std::size_t(0);
  while (tail < limit - first && before[before.size() - tail - 1] == after[after.size() - tail - 1]) ++tail;
  return {first, before.size() - first - tail, after.size() - first - tail};
}

// The query as casefolded words.
[[nodiscard]] std::vector<std::string> SearchWords(const std::string &query);
// Every word is in the title or starts the username.
[[nodiscard]] bool MatchesSearch(const std::string &title, const std::string &username,
                                 const std::vector<std::string> &words);

// One line of at most `limit` bytes, cut between characters. Labels stay cheap whatever the message's length.
[[nodiscard]] std::string OneLine(const std::string &text, std::size_t limit = 256);

struct Badge {
  bool visible = false;
  bool muted = false;
  std::string text;
};
[[nodiscard]] Badge ChatBadge(const Chat &chat);
[[nodiscard]] bool ChatIsUnread(const Chat &chat);

// The icon of the last message, a name from gtk-ui/icons or the system theme. Null when there is none.
[[nodiscard]] const char *ChatStatusIcon(const Chat &chat, UserId self);

// "typing", "Anna is typing", "3 people are typing"; empty when nobody does anything.
[[nodiscard]] std::string TypingText(const Chat &chat, const Store &store);

// "You", or the sender's first name in a group. Empty when the preview goes without a sender.
[[nodiscard]] std::string PreviewSender(const Chat &chat, const Store &store);

}  // namespace Contrail::Td
