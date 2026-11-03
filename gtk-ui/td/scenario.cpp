#include "td/scenario.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Contrail::Td {
namespace {

constexpr auto kSelfId = std::int64_t(1000);
constexpr auto kChatCount = 40;
constexpr auto kNow = 1793500000;
// CONTRAIL_SCENARIO=list: everything the chat list can show, and a long account that loads page by page.
constexpr auto kLoadedChats = 2000;
constexpr auto kLoadedChatIds = std::int64_t(100000);
constexpr auto kArchivedChats = 6;
constexpr auto kArchivedChatIds = std::int64_t(5000);
constexpr auto kGroupId = std::int64_t(-100);
constexpr auto kChannelId = std::int64_t(-200);
constexpr auto kPublicChatId = std::int64_t(-300);
// A group with topics, below the friends: General, a pinned closed one, one with unread messages and a mention, a
// muted one. Sending "/in N text" in it brings "text" from somebody into topic N, "/unforum" turns the topics off,
// "/pin N" has an admin pin message N of the topic it is sent in.
constexpr auto kForumId = std::int64_t(-400);
constexpr auto kForumGroupId = std::int64_t(400);
constexpr auto kForumOrder = std::int64_t(1) << 31;
constexpr auto kTopicMessages = 30;
constexpr auto kForumUsers = std::int64_t(2001);
constexpr auto kPinnedOrder = std::int64_t(1) << 62;
constexpr auto kNewMessageEveryMs = 5000;
// Message ids in TDLib are server ids shifted by 20 bits.
constexpr auto kIdStep = std::int64_t(1) << 20;
// Pinned messages: three in Friend 1's chat (the middle one a photo, whatever CONTRAIL_SCENARIO_PHOTOS says), one in
// Friend 2's, one in the group's topic Random. Nobody but its admins may pin in the group: there the bar can only be
// hidden.
constexpr auto kPinnedPhoto = 70;
// Reactions: some messages of the friends' chats have them (a 👍 from the friend, a ❤ of this account's), Friend 1's
// three custom emoji ones (a still sticker, an animated one with a still thumbnail, one with nothing still), Friend
// 2's newest an unread
// 🔥. In the group with topics they come with the people who reacted. A channel below the group has many on every
// post, and while it is open somebody reacts to its posts all the time, as in a busy channel.
constexpr auto kReactionsChannelId = std::int64_t(-500);
constexpr auto kReactionsChannelGroupId = std::int64_t(500);
constexpr auto kReactionsChannelOrder = std::int64_t(1) << 30;
constexpr auto kCustomStill = std::int64_t(5000000001);
constexpr auto kCustomAnimated = std::int64_t(5000000002);
constexpr auto kCustomNoStill = std::int64_t(5000000003);  // Animated, its thumbnail too: shown as its emoji.
constexpr auto kCustomStillFile = 2000001;
constexpr auto kCustomThumbnailFile = 2000002;
constexpr auto kCustomAnimatedFile = 2000003;
constexpr auto kUnreadReactionChat = std::int64_t(2);
constexpr auto kReactionBurstMs = 150u;
// A channel below the reactions one with a post of every kind the chat shows: text with entities among emoji, a
// video, a GIF, a sticker, polls, a contact, a link preview with a picture, a file, a photo, service messages; and a
// sponsored message after the newest post. Its video, GIF and file come in steps once asked for
// (cancelDownloadFile stops them). Anywhere, sending "/fail text" fails and can be sent again, "/slow text" is
// confirmed after a while. Friend 3's newest message has a bot command and a mention.
constexpr auto kShowcaseId = std::int64_t(-600);
constexpr auto kShowcaseGroupId = std::int64_t(600);
constexpr auto kShowcaseOrder = std::int64_t(1) << 29;
constexpr auto kShowcaseMessages = 20;
constexpr auto kShowcaseVideoFile = 3000001;
constexpr auto kShowcaseGifFile = 3000002;
constexpr auto kShowcaseStickerFile = 3000003;
constexpr auto kShowcaseDocumentFile = 3000004;
constexpr auto kSponsoredMessageId = std::int64_t(5);
constexpr auto kDownloadStepMs = 200u;
constexpr auto kDownloadStep = std::int64_t(128 * 1024);
constexpr auto kSlowSendMs = 1500u;
constexpr auto kCommandChat = std::int64_t(3);

// CONTRAIL_SCENARIO=3000 gives a chat that long: enough to walk away from what is held.
int MessagesInChat() {
  static const auto result = [] {
    const auto value = g_getenv("CONTRAIL_SCENARIO");
    return value ? std::max(std::atoi(value), 120) : 120;
  }();
  return result;
}

int Count(std::int64_t chat) {
  return chat == kShowcaseId ? kShowcaseMessages : MessagesInChat();
}

api::object_ptr<api::formattedText> Formatted(const std::string &text) {
  return api::make_object<api::formattedText>(text, std::vector<api::object_ptr<api::textEntity>>());
}

api::object_ptr<api::user> MakeUser(std::int64_t id, const std::string &first, const std::string &last) {
  auto user = api::make_object<api::user>();
  user->id_ = id;
  user->first_name_ = first;
  user->last_name_ = last;
  user->type_ = api::make_object<api::userTypeRegular>();
  user->status_ = api::make_object<api::userStatusOffline>(kNow - 3600);
  return user;
}

api::object_ptr<api::message> MakeMessage(std::int64_t chatId, std::int64_t id, std::int64_t senderId, int date,
                                          const std::string &text) {
  auto message = api::make_object<api::message>();
  message->id_ = id;
  message->chat_id_ = chatId;
  message->sender_id_ = api::make_object<api::messageSenderUser>(senderId);
  message->is_outgoing_ = senderId == kSelfId;
  message->date_ = date;
  auto content = api::make_object<api::messageText>();
  content->text_ = Formatted(text);
  message->content_ = std::move(content);
  return message;
}

// CONTRAIL_SCENARIO_PHOTOS=1: every fourth message is a photo, as large as Telegram sends them, to measure
// what pictures cost (tools/mem-scenario.sh). Eight pictures in ~/.cache/contrail-scenario, made once; every
// message still has file ids of its own, so each one is decoded as a different photo would be.
constexpr auto kScenarioPictures = 8;

bool WithPhotos() {
  static const auto result = g_getenv("CONTRAIL_SCENARIO_PHOTOS") != nullptr;
  return result;
}

std::string PhotoFile(int picture, int width, int height) {
  auto *name = g_strdup_printf("%d-%dx%d.jpg", picture, width, height);
  auto *path = g_build_filename(g_get_user_cache_dir(), "contrail-scenario", name, nullptr);
  auto result = std::string(path);
  g_free(path);
  g_free(name);
  if (g_file_test(result.c_str(), G_FILE_TEST_EXISTS)) return result;
  auto *directory = g_path_get_dirname(result.c_str());
  g_mkdir_with_parents(directory, 0700);
  g_free(directory);
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (!pixbuf) {
    g_warning("Scenario: no memory for a %dx%d picture", width, height);
    return result;
  }
  auto *pixels = gdk_pixbuf_get_pixels(pixbuf);
  const auto stride = gdk_pixbuf_get_rowstride(pixbuf);
  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      auto *pixel = pixels + y * stride + x * 3;
      pixel[0] = guchar(x * 255 / width);
      pixel[1] = guchar(y * 255 / height);
      pixel[2] = guchar((picture * 40 + (x ^ y)) & 0xff);  // Some detail, so the JPEG is not tiny.
    }
  }
  gchar *buffer = nullptr;
  gsize length = 0;
  GError *error = nullptr;
  // Written whole or not at all: two scenarios may start at once, and one may be killed while writing.
  if (!gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &length, "jpeg", &error, "quality", "85", nullptr)
      || !g_file_set_contents(result.c_str(), buffer, gssize(length), &error)) {
    g_warning("Scenario: picture %s not made: %s", result.c_str(), error ? error->message : "?");
    g_clear_error(&error);
  }
  g_free(buffer);
  g_object_unref(pixbuf);
  return result;
}

// The picture of a custom emoji: a PNG in the cache, as small as Telegram's (100 px).
std::string EmojiFile(const char *name, guint32 color) {
  auto *path = g_build_filename(g_get_user_cache_dir(), "contrail-scenario", name, nullptr);
  auto result = std::string(path);
  g_free(path);
  if (g_file_test(result.c_str(), G_FILE_TEST_EXISTS)) return result;
  auto *directory = g_path_get_dirname(result.c_str());
  g_mkdir_with_parents(directory, 0700);
  g_free(directory);
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 100, 100);
  gdk_pixbuf_fill(pixbuf, color);
  auto *error = static_cast<GError *>(nullptr);
  // Written whole or not at all, as the photos are.
  gchar *buffer = nullptr;
  gsize length = 0;
  if (!gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &length, "png", &error, nullptr)
      || !g_file_set_contents(result.c_str(), buffer, gssize(length), &error)) {
    g_warning("Scenario: %s not made: %s", result.c_str(), error ? error->message : "?");
    g_clear_error(&error);
  }
  g_free(buffer);
  g_object_unref(pixbuf);
  return result;
}

api::object_ptr<api::file> EmojiFileObject(std::int32_t id, const std::string &path, bool complete) {
  auto local = api::make_object<api::localFile>();
  local->path_ = complete ? path : std::string();
  local->is_downloading_completed_ = complete;
  local->downloaded_size_ = complete ? 1000 : 0;
  auto file = api::make_object<api::file>();
  file->id_ = id;
  file->size_ = 1000;
  file->local_ = std::move(local);
  file->remote_ = api::make_object<api::remoteFile>();
  return file;
}

std::string CustomStillPath() {
  return EmojiFile("emoji-still.png", 0xf08020ff);
}

std::string CustomThumbnailPath() {
  return EmojiFile("emoji-thumbnail.png", 0x20a0f0ff);
}

// Made before anything is shown, so that measurements don't include encoding them.
void MakePhotoFiles() {
  for (auto picture = 0; picture < kScenarioPictures; ++picture) {
    PhotoFile(picture, 320, 240);
    PhotoFile(picture, 2560, 1920);
  }
}

api::object_ptr<api::photoSize> PhotoSize(int index, const char *type, int width, int height) {
  const auto path = PhotoFile(index % kScenarioPictures, width, height);
  GStatBuf info;
  const auto length = g_stat(path.c_str(), &info) == 0 ? std::int64_t(info.st_size) : std::int64_t(0);
  auto local = api::make_object<api::localFile>();
  local->path_ = path;
  local->is_downloading_completed_ = length > 0;
  local->downloaded_size_ = length;
  auto file = api::make_object<api::file>();
  file->id_ = 1000000 + index * 10 + (width > 1000 ? 1 : 0);
  file->size_ = std::int64_t(length);
  file->local_ = std::move(local);
  file->remote_ = api::make_object<api::remoteFile>();
  auto size = api::make_object<api::photoSize>();
  size->type_ = type;
  size->photo_ = std::move(file);
  size->width_ = width;
  size->height_ = height;
  return size;
}

// The blurred picture a message carries: a JPEG of a few hundred bytes.
api::object_ptr<api::minithumbnail> MiniThumbnail() {
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 40, 30);
  gdk_pixbuf_fill(pixbuf, 0x3080c0ff);
  gchar *buffer = nullptr;
  gsize length = 0;
  auto result = api::object_ptr<api::minithumbnail>();
  if (gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &length, "jpeg", nullptr, "quality", "40", nullptr)) {
    result = api::make_object<api::minithumbnail>(40, 30, std::string(buffer, length));
  }
  g_free(buffer);
  g_object_unref(pixbuf);
  return result;
}

api::object_ptr<api::MessageContent> PhotoContent(int index) {
  auto photo = api::make_object<api::photo>();
  photo->sizes_.push_back(PhotoSize(index, "m", 320, 240));
  photo->sizes_.push_back(PhotoSize(index, "w", 2560, 1920));
  auto content = api::make_object<api::messagePhoto>();
  content->photo_ = std::move(photo);
  content->caption_ = Formatted(std::string());
  return content;
}

// UTF-16 units, as TDLib counts the entities.
int Utf16Length(std::string_view text) {
  auto units = 0;
  for (const auto c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte & 0xC0) != 0x80) units += (byte >= 0xF0) ? 2 : 1;
  }
  return units;
}

// `text` with the entities given to parts of it.
class Marked {
 public:
  explicit Marked(std::string text) : _text(Formatted(text)) {}
  Marked &&mark(const std::string &part, api::object_ptr<api::TextEntityType> type) && {
    const auto at = _text->text_.find(part);
    if (at != std::string::npos) {
      _text->entities_.push_back(api::make_object<api::textEntity>(Utf16Length(_text->text_.substr(0, at)),
                                                                   Utf16Length(part), std::move(type)));
    }
    return std::move(*this);
  }
  api::object_ptr<api::formattedText> take() && { return std::move(_text); }

 private:
  api::object_ptr<api::formattedText> _text;
};

api::object_ptr<api::MessageContent> TextContent(api::object_ptr<api::formattedText> text) {
  auto content = api::make_object<api::messageText>();
  content->text_ = std::move(text);
  return content;
}

std::int64_t ShowcaseSize(std::int32_t file) {
  switch (file) {
    case kShowcaseVideoFile: return 3 * 1024 * 1024;
    case kShowcaseGifFile: return 800 * 1024;
    case kShowcaseDocumentFile: return 1536 * 1024;
    default: return 30 * 1024;
  }
}

// What a download of the showcase ends with: nothing plays it here, so what it holds doesn't matter. The sticker is
// a real picture.
std::string ShowcasePath(std::int32_t file) {
  if (file == kShowcaseStickerFile) return EmojiFile("sticker.png", 0x60c060ff);
  auto *name = g_strdup_printf("showcase-%d.bin", file);
  auto *path = g_build_filename(g_get_user_cache_dir(), "contrail-scenario", name, nullptr);
  auto result = std::string(path);
  g_free(path);
  g_free(name);
  if (!g_file_test(result.c_str(), G_FILE_TEST_EXISTS)) g_file_set_contents(result.c_str(), "scenario", -1, nullptr);
  return result;
}

api::object_ptr<api::poll> MakePoll(bool voted) {
  auto poll = api::make_object<api::poll>();
  poll->id_ = voted ? 2 : 1;
  poll->question_ = Formatted(voted ? "Which one do you carry?" : "Which phone runs Contrail best?");
  const char *answers[] = {"FLX1s", "PinePhone", "Librem 5"};
  const int percents[] = {50, 30, 20};
  for (auto i = 0; i < 3; ++i) {
    auto option = api::make_object<api::pollOption>();
    option->id_ = std::to_string(i);
    option->text_ = Formatted(answers[i]);
    option->voter_count_ = voted ? percents[i] / 10 : 0;
    option->vote_percentage_ = voted ? percents[i] : 0;
    option->is_chosen_ = voted && i == 0;
    poll->options_.push_back(std::move(option));
  }
  poll->total_voter_count_ = voted ? 10 : 12;
  poll->is_anonymous_ = true;
  poll->type_ = api::make_object<api::pollTypeRegular>();
  return poll;
}

std::string MessageText(int index) {
  switch (index % 5) {
    case 0: return "Message " + std::to_string(index);
    case 1: return "A longer message that has to wrap on the narrow screen of a phone, number " + std::to_string(index);
    case 2: return "With an emoji 😀 in the middle, number " + std::to_string(index);
    case 3: return "Первая строка\nВторая строка " + std::to_string(index);
    default: return "ok";
  }
}

api::object_ptr<api::ChatList> ListObject(bool archive) {
  if (archive) return api::make_object<api::chatListArchive>();
  return api::make_object<api::chatListMain>();
}

class ScenarioTransport final : public Transport {
 public:
  ScenarioTransport() : _fullList(g_strcmp0(g_getenv("CONTRAIL_SCENARIO"), "list") == 0) {}

  ~ScenarioTransport() override {
    if (_source) g_source_remove(_source);
    if (_ticker) g_source_remove(_ticker);
    if (_burst) g_source_remove(_burst);
    if (_downloadTicker) g_source_remove(_downloadTicker);
    for (auto *later : std::set<Later *>(_later)) g_source_remove(later->source);
  }

  void start(Sink sink) override {
    _sink = std::move(sink);
    _source = g_idle_add_once(+[](gpointer data) {
      auto *self = static_cast<ScenarioTransport *>(data);
      self->_source = 0;
      self->authorization(api::make_object<api::authorizationStateWaitTdlibParameters>());
    }, this);
  }

  void send(std::uint64_t id, FunctionPtr function) override {
    if (showcaseRequest(id, *function) || forumRequest(id, *function) || pinnedRequest(id, *function)
        || reactionRequest(id, *function) || backgroundRequest(id, *function)) {
      return;
    }
    switch (function->get_id()) {
      case api::setTdlibParameters::ID:
        answer(id, api::make_object<api::ok>());
        authorization(api::make_object<api::authorizationStateReady>());
        fill();
        break;
      case api::getAuthorizationState::ID:
        answer(id, api::make_object<api::authorizationStateReady>());
        break;
      case api::loadChats::ID:
        answer(id, loadChats(static_cast<api::loadChats &>(*function)));
        break;
      case api::getChat::ID:
        answer(id, makeChat(static_cast<api::getChat &>(*function).chat_id_));
        break;
      case api::createPrivateChat::ID: {
        const auto user = static_cast<api::createPrivateChat &>(*function).user_id_;
        // Saved Messages, never used yet: TDLib tells about the chat before the answer, as about any new one.
        if (user == kSelfId && !_selfAdded) {
          _selfAdded = true;
          update(api::make_object<api::updateNewChat>(
              api::move_object_as<api::chat>(makeChat(kSelfId))));
        }
        answer(id, makeChat(user));
        break;
      }
      case api::forwardMessages::ID:
        answer(id, forwarded(static_cast<api::forwardMessages &>(*function)));
        break;
      case api::searchChatsOnServer::ID:
        answer(id, api::make_object<api::chats>(0, std::vector<std::int64_t>()));
        break;
      case api::searchPublicChats::ID:
        answer(id, publicChats(static_cast<api::searchPublicChats &>(*function)));
        break;
      case api::toggleChatIsPinned::ID:
        pin(static_cast<api::toggleChatIsPinned &>(*function));
        answer(id, api::make_object<api::ok>());
        break;
      case api::setChatNotificationSettings::ID: {
        auto &request = static_cast<api::setChatNotificationSettings &>(*function);
        update(api::make_object<api::updateChatNotificationSettings>(request.chat_id_,
                                                                     std::move(request.notification_settings_)));
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::addChatToList::ID:
        move(static_cast<api::addChatToList &>(*function));
        answer(id, api::make_object<api::ok>());
        break;
      case api::deleteChatHistory::ID:
        remove(static_cast<api::deleteChatHistory &>(*function).chat_id_);
        answer(id, api::make_object<api::ok>());
        break;
      case api::leaveChat::ID:
        remove(static_cast<api::leaveChat &>(*function).chat_id_);
        answer(id, api::make_object<api::ok>());
        break;
      case api::readAllChatMentions::ID:
        update(api::make_object<api::updateChatUnreadMentionCount>(
            static_cast<api::readAllChatMentions &>(*function).chat_id_, 0));
        answer(id, api::make_object<api::ok>());
        break;
      case api::readAllChatReactions::ID:
        update(api::make_object<api::updateChatUnreadReactionCount>(
            static_cast<api::readAllChatReactions &>(*function).chat_id_, 0));
        answer(id, api::make_object<api::ok>());
        break;
      case api::toggleChatIsMarkedAsUnread::ID: {
        auto &request = static_cast<api::toggleChatIsMarkedAsUnread &>(*function);
        update(api::make_object<api::updateChatIsMarkedAsUnread>(request.chat_id_, request.is_marked_as_unread_));
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::viewMessages::ID: {
        auto &request = static_cast<api::viewMessages &>(*function);
        if (_fullList && !request.message_ids_.empty()) {
          update(api::make_object<api::updateChatReadInbox>(request.chat_id_, request.message_ids_.back(), 0));
        }
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::getChatHistory::ID:
        answer(id, history(static_cast<api::getChatHistory &>(*function)));
        break;
      case api::sendMessage::ID:
        answer(id, sent(static_cast<api::sendMessage &>(*function)));
        break;
      case api::getChatMessageByDate::ID: {
        const auto &request = static_cast<api::getChatMessageByDate &>(*function);
        const auto index = MessagesInChat() - (kNow - request.date_) / 600;
        if (index < 1) answer(id, api::make_object<api::error>(404, "Not Found"));
        else answer(id, messageAt(request.chat_id_, std::min(index, MessagesInChat())));
        break;
      }
      case api::getRepliedMessage::ID: {
        const auto &request = static_cast<api::getRepliedMessage &>(*function);
        answer(id, messageAt(request.chat_id_, int(request.message_id_ / kIdStep) - 1));
        break;
      }
      case api::getMessage::ID: {
        const auto &request = static_cast<api::getMessage &>(*function);
        answer(id, messageAt(request.chat_id_, int(request.message_id_ / kIdStep)));
        break;
      }
      case api::getMessageProperties::ID: {
        auto properties = api::make_object<api::messageProperties>();
        properties->can_be_edited_ = true;
        properties->can_be_forwarded_ = true;
        properties->can_be_copied_ = true;
        properties->can_be_saved_ = true;
        properties->can_be_replied_ = true;
        properties->can_be_deleted_for_all_users_ = true;
        properties->can_be_deleted_only_for_self_ = true;
        const auto chat = static_cast<api::getMessageProperties &>(*function).chat_id_;
        properties->can_be_pinned_ = chat != kForumId && chat != kChannelId;
        answer(id, std::move(properties));
        break;
      }
      case api::editMessageText::ID: {
        auto &request = static_cast<api::editMessageText &>(*function);
        auto text = std::string();
        if (request.input_message_content_->get_id() == api::inputMessageText::ID) {
          text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
        }
        auto content = api::make_object<api::messageText>();
        content->text_ = Formatted(text);
        update(api::make_object<api::updateMessageContent>(request.chat_id_, request.message_id_, std::move(content)));
        update(api::make_object<api::updateMessageEdited>(request.chat_id_, request.message_id_, kNow, nullptr));
        answer(id, MakeMessage(request.chat_id_, request.message_id_, kSelfId, kNow, text));
        break;
      }
      case api::deleteMessages::ID: {
        auto &request = static_cast<api::deleteMessages &>(*function);
        update(api::make_object<api::updateDeleteMessages>(request.chat_id_, std::move(request.message_ids_), true, false));
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::getLanguagePackStrings::ID:
        answer(id, api::make_object<api::error>(400, "The scenario has no language pack"));
        break;
      case api::setOption::ID:
      case api::openChat::ID:
      case api::closeChat::ID:
      case api::setChatDraftMessage::ID:
      case api::sendChatAction::ID:
      case api::downloadFile::ID:
        answer(id, api::make_object<api::ok>());
        break;
      default:
        answer(id, api::make_object<api::error>(400, "Not in the scenario"));
        break;
    }
  }

 private:
  void answer(std::uint64_t id, ObjectPtr object) { _sink(id, std::move(object)); }
  void update(api::object_ptr<api::Update> update) { _sink(0, std::move(update)); }
  void authorization(api::object_ptr<api::AuthorizationState> state) {
    update(api::make_object<api::updateAuthorizationState>(std::move(state)));
  }

  void fill() {
    if (WithPhotos()) MakePhotoFiles();
    update(api::make_object<api::updateOption>("my_id", api::make_object<api::optionValueInteger>(kSelfId)));
    update(api::make_object<api::updateUser>(MakeUser(kSelfId, "Vlad", "")));
    for (auto i = 1; i <= kChatCount; ++i) {
      const auto id = std::int64_t(i);
      update(api::make_object<api::updateUser>(MakeUser(id, "Friend", std::to_string(i))));
      auto chat = api::make_object<api::chat>();
      chat->id_ = id;
      chat->type_ = api::make_object<api::chatTypePrivate>(id);
      chat->title_ = "Friend " + std::to_string(i);
      chat->unread_count_ = i % 4 == 0 ? i : 0;
      chat->last_read_inbox_message_id_ = (MessagesInChat() - chat->unread_count_) * kIdStep;
      chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
      chat->last_message_ = messageAt(id, MessagesInChat());
      if (id == kUnreadReactionChat) chat->unread_reaction_count_ = 1;
      chat->positions_.push_back(api::make_object<api::chatPosition>(
          api::make_object<api::chatListMain>(), std::int64_t(kChatCount - i + 1) << 32, i == 1, nullptr));
      if (_fullList) decorate(*chat, i);
      _orders[id] = {std::int64_t(kChatCount - i + 1) << 32, false};
      update(api::make_object<api::updateNewChat>(std::move(chat)));
    }
    addForum();
    addReactionsChannel();
    addShowcase();
    update(api::make_object<api::updateDefaultReactionType>(api::make_object<api::reactionTypeEmoji>("\xE2\x9D\xA4")));
    if (!_fullList) return;
    addOtherChats();
    _ticker = g_timeout_add(kNewMessageEveryMs, +[](gpointer data) -> gboolean {
      static_cast<ScenarioTransport *>(data)->newMessage();
      return G_SOURCE_CONTINUE;
    }, this);
  }

  // One friend for every way a row can look.
  void decorate(api::chat &chat, int index) {
    const auto id = chat.id_;
    const auto outgoing = [&](api::object_ptr<api::MessageSendingState> state) {
      chat.last_message_ = MakeMessage(id, MessagesInChat() * kIdStep, kSelfId, kNow - 60, "Outgoing message");
      chat.last_message_->sending_state_ = std::move(state);
    };
    switch (index) {
      case 2: {
        auto content = api::make_object<api::draftMessageContentText>();
        content->text_ = Formatted("A draft\nof two lines");
        chat.draft_message_ = api::make_object<api::draftMessage>();
        chat.draft_message_->content_ = std::move(content);
        break;
      }
      case 3:
        chat.unread_count_ = 5;
        chat.notification_settings_->mute_for_ = 1000000;
        break;
      case 4:
        chat.unread_count_ = 2;
        chat.unread_mention_count_ = 1;
        break;
      case 5: chat.unread_reaction_count_ = 1; break;
      case 6: chat.is_marked_as_unread_ = true; break;
      case 7:
        outgoing(nullptr);
        chat.last_read_outbox_message_id_ = MessagesInChat() * kIdStep;
        break;
      case 8: outgoing(nullptr); break;
      case 9: outgoing(api::make_object<api::messageSendingStatePending>(0)); break;
      case 10: outgoing(api::make_object<api::messageSendingStateFailed>()); break;
      default: break;
    }
  }

  void addChat(std::int64_t id, api::object_ptr<api::ChatType> type, const std::string &title, std::int64_t order,
               bool archive, const std::string &text, std::int64_t sender) {
    auto chat = api::make_object<api::chat>();
    chat->id_ = id;
    chat->type_ = std::move(type);
    chat->title_ = title;
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    if (!text.empty()) chat->last_message_ = MakeMessage(id, kIdStep, sender, kNow - 7200, text);
    if (order) {
      chat->positions_.push_back(api::make_object<api::chatPosition>(ListObject(archive), order, false, nullptr));
      _orders[id] = {order, archive};
    }
    update(api::make_object<api::updateNewChat>(std::move(chat)));
  }

  void addOtherChats() {
    update(api::make_object<api::updateUser>(MakeUser(2000, "Anna", "Long")));
    _selfAdded = true;
    addChat(kSelfId, api::make_object<api::chatTypePrivate>(kSelfId), "Vlad", std::int64_t(100) << 32, false,
            "Saved note", kSelfId);
    addChat(kGroupId, api::make_object<api::chatTypeBasicGroup>(100), "Team", std::int64_t(99) << 32, false,
            "Lunch at one?", 2000);
    addChat(kChannelId, api::make_object<api::chatTypeSupergroup>(200, true), "News", std::int64_t(98) << 32, false,
            "Today's post", 2000);
    update(api::make_object<api::updateChatAction>(1, nullptr, api::make_object<api::messageSenderUser>(1),
                                                   api::make_object<api::chatActionTyping>()));
  }

  // Page by page like the real thing: an account with far more chats than one answer.
  ObjectPtr loadChats(api::loadChats &request) {
    if (!_fullList) return api::make_object<api::error>(404, "Not Found");
    if (request.chat_list_ && request.chat_list_->get_id() == api::chatListArchive::ID) {
      if (_archiveLoaded) return api::make_object<api::error>(404, "Not Found");
      _archiveLoaded = true;
      for (auto i = 1; i <= kArchivedChats; ++i) {
        const auto id = kArchivedChatIds + i;
        update(api::make_object<api::updateUser>(MakeUser(id, "Old", std::to_string(i))));
        addChat(id, api::make_object<api::chatTypePrivate>(id), "Old " + std::to_string(i),
                std::int64_t(kArchivedChats - i + 1) << 32, true, "Archived message", id);
      }
      update(api::make_object<api::updateUnreadChatCount>(ListObject(true), 2, 2, 0, 0, 0));
      return api::make_object<api::ok>();
    }
    if (_loaded >= kLoadedChats) return api::make_object<api::error>(404, "Not Found");
    for (auto i = 0; i < request.limit_ && _loaded < kLoadedChats; ++i) {
      ++_loaded;
      const auto id = kLoadedChatIds + _loaded;
      update(api::make_object<api::updateUser>(MakeUser(id, "Person", std::to_string(_loaded))));
      addChat(id, api::make_object<api::chatTypePrivate>(id), "Person " + std::to_string(_loaded),
              kLoadedChats - _loaded + 1, false, "Older message " + std::to_string(_loaded), id);
    }
    return api::make_object<api::ok>();
  }

  ObjectPtr makeChat(std::int64_t id) {
    auto chat = api::make_object<api::chat>();
    chat->id_ = id;
    chat->type_ = api::make_object<api::chatTypePrivate>(id);
    chat->title_ = id == kSelfId ? "Vlad" : "Chat " + std::to_string(id);
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    chat->notification_settings_->sound_id_ = 1;
    return chat;
  }

  ObjectPtr publicChats(api::searchPublicChats &request) {
    auto ids = std::vector<std::int64_t>();
    if (_fullList && request.query_.find("pub") != std::string::npos) {
      if (!_publicAdded) {
        _publicAdded = true;
        auto group = api::make_object<api::supergroup>();
        group->id_ = 300;
        auto names = api::make_object<api::usernames>();
        names->editable_username_ = "public_news";
        names->active_usernames_.push_back("public_news");
        group->usernames_ = std::move(names);
        update(api::make_object<api::updateSupergroup>(std::move(group)));
        addChat(kPublicChatId, api::make_object<api::chatTypeSupergroup>(300, true), "Public Channel", 0, false, "", 0);
      }
      ids.push_back(kPublicChatId);
    }
    return api::make_object<api::chats>(int(ids.size()), std::move(ids));
  }

  void position(std::int64_t id, std::int64_t order, bool archive, bool pinned = false) {
    update(api::make_object<api::updateChatPosition>(
        id, api::make_object<api::chatPosition>(ListObject(archive), order, pinned, nullptr)));
  }

  void pin(api::toggleChatIsPinned &request) {
    const auto archive = request.chat_list_ && request.chat_list_->get_id() == api::chatListArchive::ID;
    const auto id = request.chat_id_;
    position(id, request.is_pinned_ ? kPinnedOrder + (++_pins) : _orders[id].first, archive, request.is_pinned_);
  }

  void move(api::addChatToList &request) {
    const auto id = request.chat_id_;
    const auto archive = request.chat_list_ && request.chat_list_->get_id() == api::chatListArchive::ID;
    auto &known = _orders[id];
    position(id, 0, known.second);
    known.second = archive;
    position(id, known.first, archive);
  }

  void remove(std::int64_t id) {
    position(id, 0, _orders[id].second);
    _orders.erase(id);
  }

  // Something happens while a person reads another chat. Always somebody down the list: the row has to come up.
  void newMessage() {
    const auto id = std::int64_t(20 + (_ticks % 10));
    const auto index = MessagesInChat() + 1000 + (++_ticks);
    auto positions = std::vector<api::object_ptr<api::chatPosition>>();
    _orders[id].first = std::int64_t(1000 + _ticks) << 32;
    positions.push_back(api::make_object<api::chatPosition>(ListObject(false), _orders[id].first, false, nullptr));
    update(api::make_object<api::updateChatReadInbox>(id, 0, _ticks));
    update(api::make_object<api::updateChatLastMessage>(
        id, MakeMessage(id, index * kIdStep, id, kNow, "New message " + std::to_string(_ticks)), std::move(positions)));
  }

  api::object_ptr<api::message> messageAt(std::int64_t chatId, int index) {
    if (chatId == kShowcaseId) return showcaseMessage(index);
    const auto sender = index % 3 == 0 ? kSelfId : chatId;
    auto message = MakeMessage(chatId, index * kIdStep, sender, kNow - (MessagesInChat() - index) * 600, MessageText(index));
    if (chatId == kReactionsChannelId) {
      message->sender_id_ = api::make_object<api::messageSenderChat>(chatId);
      message->is_outgoing_ = false;
      message->is_channel_post_ = true;
      auto content = api::make_object<api::messageText>();
      content->text_ = Formatted("Post " + std::to_string(index));
      message->content_ = std::move(content);
    }
    if (WithPhotos() && index % 4 == 1) message->content_ = PhotoContent(index);
    if (chatId == kCommandChat && index == MessagesInChat()) {
      message->content_ = TextContent(Marked("Try /help, or ask @friend1")
                                          .mark("/help", api::make_object<api::textEntityTypeBotCommand>())
                                          .mark("@friend1", api::make_object<api::textEntityTypeMention>())
                                          .take());
    }
    if (chatId == 1 && index == kPinnedPhoto) {
      auto photo = api::move_object_as<api::messagePhoto>(PhotoContent(index));
      photo->photo_->minithumbnail_ = MiniThumbnail();
      photo->caption_ = Formatted("Pinned photo");
      message->content_ = std::move(photo);
    }
    // Every seventh message answers the one before it.
    if (index % 7 == 0) {
      message->reply_to_ = api::make_object<api::messageReplyToMessage>(
          chatId, (index - 1) * kIdStep, nullptr, 0, std::string(), nullptr, 0, nullptr);
    }
    applyEntry(*message);
    message->is_pinned_ = isPinned(chatId, message->id_);
    applyReactions(*message);
    return message;
  }

  ObjectPtr history(api::getChatHistory &request) {
    auto result = api::make_object<api::messages>();
    const auto chatId = request.chat_id_;
    const auto count = Count(chatId);
    auto newest = request.from_message_id_ ? int(request.from_message_id_ / kIdStep) : count + 1;
    // from_message_id itself is included only with a negative offset, as in TDLib.
    newest -= request.offset_;
    if (request.offset_ == 0) --newest;
    if (newest > count + _sentExtra[chatId]) newest = count + _sentExtra[chatId];
    for (auto index = newest; index >= 1 && int(result->messages_.size()) < request.limit_; --index) {
      result->messages_.push_back(messageAt(chatId, index));
    }
    result->total_count_ = count;
    return result;
  }

  ObjectPtr sent(api::sendMessage &request) {
    auto text = std::string();
    if (request.input_message_content_
        && request.input_message_content_->get_id() == api::inputMessageText::ID) {
      text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
    }
    if (text.starts_with("/fail ")) return failing(request.chat_id_, text.substr(6));
    if (text.starts_with("/slow ")) return outgoing(request.chat_id_, Entry{.text = text.substr(6)}, kSlowSendMs);
    return outgoing(request.chat_id_, Entry{.text = text});
  }

  // A message that fails to be sent, a moment after it was: it stays with its error until it is sent again.
  api::object_ptr<api::message> failing(std::int64_t chatId, const std::string &text) {
    const auto temporary = (Count(chatId) + _sentExtra[chatId] + 1) * kIdStep + 1 + 2 * (++_failures);
    const auto pending = [=] {
      auto message = MakeMessage(chatId, temporary, kSelfId, kNow, text);
      message->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
      return message;
    };
    update(api::make_object<api::updateNewMessage>(pending()));
    later(300, [this, chatId, temporary, text] {
      auto failed = MakeMessage(chatId, temporary + 1, kSelfId, kNow, text);
      auto state = api::make_object<api::messageSendingStateFailed>();
      state->error_ = api::make_object<api::error>(400, "Scenario: not sent");
      state->can_retry_ = true;
      failed->sending_state_ = std::move(state);
      _failed[{chatId, temporary + 1}] = text;
      update(api::make_object<api::updateMessageSendFailed>(std::move(failed), temporary,
                                                            api::make_object<api::error>(400, "Scenario: not sent")));
    });
    return pending();
  }

  // As TDLib does: the failed message goes, and a new one is sent in its place.
  ObjectPtr resend(api::resendMessages &request) {
    auto result = api::make_object<api::messages>();
    for (const auto id : request.message_ids_) {
      const auto it = _failed.find({request.chat_id_, id});
      if (it == _failed.end()) continue;
      const auto text = it->second;
      _failed.erase(it);
      update(api::make_object<api::updateDeleteMessages>(request.chat_id_, std::vector<std::int64_t>{id}, true, false));
      result->messages_.push_back(outgoing(request.chat_id_, Entry{.text = text}));
    }
    result->total_count_ = int(result->messages_.size());
    return result;
  }

  // Once, `ms` from now, unless the scenario has gone.
  struct Later {
    ScenarioTransport *self = nullptr;
    std::function<void()> callback;
    guint source = 0;
  };
  void later(guint ms, std::function<void()> callback) {
    auto *data = new Later{this, std::move(callback), 0};
    data->source = g_timeout_add_full(
        G_PRIORITY_DEFAULT, ms,
        +[](gpointer pointer) -> gboolean {
          auto *later = static_cast<Later *>(pointer);
          later->self->_later.erase(later);
          later->callback();
          return G_SOURCE_REMOVE;
        },
        data, +[](gpointer pointer) { delete static_cast<Later *>(pointer); });
    _later.insert(data);
  }

  // --- what this account sent or forwarded ---------------------------------------

  // Who a forwarded message was written by.
  struct Origin {
    std::int64_t user = 0;
    std::int64_t chat = 0;
    std::string hidden;
  };
  struct Entry {
    std::string text;
    int photo = -1;  // The scenario's photo of this index, with `text` as its caption.
    bool forwarded = false;
    Origin origin;
  };

  // A message sent or forwarded here: it stays as sent when the chat is opened again.
  void applyEntry(api::message &message) {
    const auto it = _entries.find({message.chat_id_, message.id_ - message.id_ % kIdStep});
    if (it == _entries.end()) return;
    const auto &entry = it->second;
    message.sender_id_ = api::make_object<api::messageSenderUser>(kSelfId);
    message.is_outgoing_ = true;
    message.date_ = kNow;
    message.reply_to_ = nullptr;
    if (entry.photo >= 0) {
      auto content = api::move_object_as<api::messagePhoto>(PhotoContent(entry.photo));
      content->caption_ = Formatted(entry.text);
      message.content_ = std::move(content);
    } else {
      auto content = api::make_object<api::messageText>();
      content->text_ = Formatted(entry.text);
      message.content_ = std::move(content);
    }
    message.forward_info_ = nullptr;
    if (!entry.forwarded) return;
    auto origin = api::object_ptr<api::MessageOrigin>();
    if (entry.origin.user) {
      origin = api::make_object<api::messageOriginUser>(entry.origin.user);
    } else if (entry.origin.chat) {
      origin = api::make_object<api::messageOriginChat>(entry.origin.chat, std::string());
    } else {
      origin = api::make_object<api::messageOriginHiddenUser>(entry.origin.hidden);
    }
    message.forward_info_ = api::make_object<api::messageForwardInfo>(std::move(origin), kNow - 3600, nullptr,
                                                                      std::string());
  }

  // `delay`: the server confirms it that much later.
  api::object_ptr<api::message> outgoing(std::int64_t chatId, Entry entry, guint delay = 0) {
    const auto index = Count(chatId) + ++_sentExtra[chatId];
    const auto id = index * kIdStep;
    _entries[{chatId, id}] = std::move(entry);
    const auto temporary = id + 1;
    auto pending = messageAt(chatId, index);
    pending->id_ = temporary;
    pending->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
    auto shown = messageAt(chatId, index);
    shown->id_ = temporary;
    shown->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
    update(api::make_object<api::updateNewMessage>(std::move(shown)));
    const auto succeeded = [this, chatId, index, temporary] {
      update(api::make_object<api::updateMessageSendSucceeded>(messageAt(chatId, index), temporary));
      update(api::make_object<api::updateChatLastMessage>(chatId, messageAt(chatId, index),
                                                          std::vector<api::object_ptr<api::chatPosition>>()));
    };
    if (delay) later(delay, succeeded);
    else succeeded();
    return pending;
  }

  // A message as the chat has it now, null when there is none.
  api::object_ptr<api::message> existing(std::int64_t chatId, std::int64_t id) {
    if (chatId == kForumId) {
      const auto topic = findTopic(TopicOfId(id));
      const auto index = IndexOfId(id);
      if (!topic || index < 1 || index > topic->last) return nullptr;
      return topicMessage(topic->id, index, index > kTopicMessages ? _sentTexts[id] : "");
    }
    const auto index = int(id / kIdStep);
    if (index < 1 || index > Count(chatId) + _sentExtra[chatId]) return nullptr;
    return messageAt(chatId, index);
  }

  // As Telegram does: with the senders' names the copies say whose they were (of a forwarded one, whose it was
  // first); without them they are this account's own messages.
  ObjectPtr forwarded(api::forwardMessages &request) {
    auto result = api::make_object<api::messages>();
    const auto topicId = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
        ? static_cast<api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 1;
    for (const auto id : request.message_ids_) {
      auto source = existing(request.from_chat_id_, id);
      if (!source) {
        result->messages_.push_back(nullptr);
        continue;
      }
      auto entry = Entry();
      if (const auto text = source->content_->get_id() == api::messageText::ID
              ? static_cast<const api::messageText &>(*source->content_).text_.get() : nullptr) {
        entry.text = text->text_;
      } else if (source->content_->get_id() == api::messagePhoto::ID) {
        const auto &photo = static_cast<const api::messagePhoto &>(*source->content_);
        entry.photo = int(source->id_ / kIdStep);
        if (!request.remove_caption_ && photo.caption_) entry.text = photo.caption_->text_;
      }
      if (!request.send_copy_) {
        entry.forwarded = true;
        const auto origin = source->forward_info_ ? source->forward_info_->origin_.get() : nullptr;
        if (origin && origin->get_id() == api::messageOriginUser::ID) {
          entry.origin.user = static_cast<const api::messageOriginUser &>(*origin).sender_user_id_;
        } else if (origin && origin->get_id() == api::messageOriginChat::ID) {
          entry.origin.chat = static_cast<const api::messageOriginChat &>(*origin).sender_chat_id_;
        } else if (origin && origin->get_id() == api::messageOriginHiddenUser::ID) {
          entry.origin.hidden = static_cast<const api::messageOriginHiddenUser &>(*origin).sender_name_;
        } else if (source->sender_id_->get_id() == api::messageSenderUser::ID) {
          entry.origin.user = static_cast<const api::messageSenderUser &>(*source->sender_id_).user_id_;
        } else {
          entry.origin.chat = static_cast<const api::messageSenderChat &>(*source->sender_id_).chat_id_;
        }
      }
      if (request.chat_id_ == kForumId) {
        const auto into = findTopic(topicId);
        if (!into) return api::make_object<api::error>(400, "TOPIC_ID_INVALID");
        result->messages_.push_back(forumOutgoing(*into, std::move(entry)));
      } else {
        result->messages_.push_back(outgoing(request.chat_id_, std::move(entry)));
      }
    }
    result->total_count_ = int(result->messages_.size());
    return result;
  }


  // --- pinned messages -----------------------------------------------------------

  bool isPinned(std::int64_t chat, std::int64_t id) const {
    const auto it = _pinned.find(chat);
    return it != _pinned.end() && it->second.contains(id);
  }

  api::object_ptr<api::message> anyMessage(std::int64_t chat, std::int64_t id) {
    if (chat == kForumId) {
      const auto topic = TopicOfId(id);
      const auto index = IndexOfId(id);
      return topicMessage(topic, index, index > kTopicMessages ? _sentTexts[id] : "");
    }
    return messageAt(chat, int(id / kIdStep));
  }

  void setPinned(std::int64_t chat, std::int64_t id, bool pinned) {
    auto &ids = _pinned[chat];
    if (pinned ? !ids.insert(id).second : !ids.erase(id)) return;
    update(api::make_object<api::updateMessageIsPinned>(chat, id, pinned));
  }

  // Newest first, as TDLib answers; from_message_id itself is left out.
  ObjectPtr searchPinned(const api::searchChatMessages &request) {
    auto ids = std::vector<std::int64_t>();
    if (const auto it = _pinned.find(request.chat_id_); it != _pinned.end()) {
      const auto topic = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
          ? static_cast<const api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 0;
      for (const auto id : it->second) {
        if (!topic || TopicOfId(id) == topic) ids.push_back(id);
      }
    }
    std::ranges::sort(ids, std::greater<>());
    auto result = api::make_object<api::foundChatMessages>();
    result->total_count_ = int(ids.size());
    for (const auto id : ids) {
      if (request.from_message_id_ && id >= request.from_message_id_) continue;
      if (int(result->messages_.size()) == request.limit_) {
        result->next_from_message_id_ = result->messages_.back()->id_;
        break;
      }
      result->messages_.push_back(anyMessage(request.chat_id_, id));
    }
    return result;
  }

  bool pinnedRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::searchChatMessages::ID: {
        auto &request = static_cast<api::searchChatMessages &>(function);
        if (!request.filter_ || request.filter_->get_id() != api::searchMessagesFilterPinned::ID) return false;
        answer(id, searchPinned(request));
        return true;
      }
      case api::getChatPinnedMessage::ID: {
        const auto chat = static_cast<api::getChatPinnedMessage &>(function).chat_id_;
        const auto it = _pinned.find(chat);
        if (it == _pinned.end() || it->second.empty()) answer(id, api::make_object<api::error>(404, "Not Found"));
        else answer(id, anyMessage(chat, *it->second.rbegin()));
        return true;
      }
      case api::pinChatMessage::ID: {
        const auto &request = static_cast<api::pinChatMessage &>(function);
        if (request.chat_id_ == kForumId || request.chat_id_ == kChannelId) {
          answer(id, api::make_object<api::error>(400, "CHAT_ADMIN_REQUIRED"));
          return true;
        }
        setPinned(request.chat_id_, request.message_id_, true);
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::unpinChatMessage::ID: {
        const auto &request = static_cast<api::unpinChatMessage &>(function);
        setPinned(request.chat_id_, request.message_id_, false);
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::unpinAllChatMessages::ID: {
        const auto chat = static_cast<api::unpinAllChatMessages &>(function).chat_id_;
        const auto ids = _pinned[chat];
        for (const auto message : ids) setPinned(chat, message, false);
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::unpinAllForumTopicMessages::ID: {
        const auto &request = static_cast<api::unpinAllForumTopicMessages &>(function);
        const auto ids = _pinned[request.chat_id_];
        for (const auto message : ids) {
          if (TopicOfId(message) == request.forum_topic_id_) setPinned(request.chat_id_, message, false);
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      default:
        return false;
    }
  }

  // --- reactions -----------------------------------------------------------------

  struct Reaction {
    std::string key;  // The emoji, or "custom:ID".
    int count = 0;
    bool chosen = false;
    std::vector<std::int64_t> recent;  // Users, newest first; none in the channel.
  };
  using Reactions = std::vector<Reaction>;

  static api::object_ptr<api::ReactionType> ReactionType(const std::string &key) {
    if (key.starts_with("custom:")) return api::make_object<api::reactionTypeCustomEmoji>(std::atoll(key.c_str() + 7));
    return api::make_object<api::reactionTypeEmoji>(key);
  }

  static std::string ReactionKey(const api::ReactionType *type) {
    if (!type) return {};
    if (type->get_id() == api::reactionTypeEmoji::ID) return static_cast<const api::reactionTypeEmoji &>(*type).emoji_;
    if (type->get_id() == api::reactionTypeCustomEmoji::ID) {
      return "custom:" + std::to_string(static_cast<const api::reactionTypeCustomEmoji &>(*type).custom_emoji_id_);
    }
    return "paid";
  }

  // What a message has before anybody here does anything.
  static Reactions InitialReactions(std::int64_t chat, std::int64_t id) {
    auto result = Reactions();
    const auto thumbs = "\xF0\x9F\x91\x8D", heart = "\xE2\x9D\xA4", fire = "\xF0\x9F\x94\xA5", party = "\xF0\x9F\x8E\x89";
    if (chat == kReactionsChannelId) {
      const auto index = int(id / kIdStep);
      result.push_back({thumbs, 5 + index * 37 % 900});
      result.push_back({heart, 1 + index % 50 + (index % 4 == 0), index % 4 == 0});
      if (index % 7) result.push_back({fire, index % 7});
      if (index % 6 == 0) result.push_back({"custom:" + std::to_string(kCustomStill), 3});
      return result;
    }
    if (chat == kForumId) {
      const auto index = IndexOfId(id), topic = TopicOfId(id);
      if (index % 4 == 1) result.push_back({thumbs, 2, false, {kForumUsers + topic % 3, kForumUsers + (topic + 1) % 3}});
      if (index % 8 == 1) result.push_back({party, 1, true, {kSelfId}});
      return result;
    }
    if (chat < 1 || chat > kChatCount) return result;
    const auto index = int(id / kIdStep);
    if (index > MessagesInChat()) return result;
    if (index % 5 == 2) result.push_back({thumbs, 1, false, {chat}});
    if (index % 10 == 2) result.push_back({heart, 1, true, {kSelfId}});
    if (chat == 1 && index == MessagesInChat() - 3) result.push_back({"custom:" + std::to_string(kCustomStill), 1, false, {chat}});
    if (chat == 1 && index == MessagesInChat() - 4) result.push_back({"custom:" + std::to_string(kCustomAnimated), 1, false, {chat}});
    if (chat == 1 && index == MessagesInChat() - 5) result.push_back({"custom:" + std::to_string(kCustomNoStill), 1, false, {chat}});
    if (chat == kUnreadReactionChat && index == MessagesInChat()) result.push_back({fire, 1, false, {chat}});
    return result;
  }

  Reactions &reactionsOf(std::int64_t chat, std::int64_t id) {
    const auto key = std::make_pair(chat, id);
    auto it = _reactions.find(key);
    if (it == _reactions.end()) it = _reactions.emplace(key, InitialReactions(chat, id)).first;
    return it->second;
  }

  api::object_ptr<api::messageInteractionInfo> interactionInfo(std::int64_t chat, std::int64_t id) {
    auto info = api::make_object<api::messageInteractionInfo>();
    if (chat == kReactionsChannelId) info->view_count_ = 1000 + int(id / kIdStep);
    const auto &list = reactionsOf(chat, id);
    if (!list.empty()) {
      auto reactions = api::make_object<api::messageReactions>();
      // As TDLib: who reacted is told in groups, not in private chats or channels.
      reactions->can_get_added_reactions_ = chat == kForumId;
      for (const auto &reaction : list) {
        auto recent = std::vector<api::object_ptr<api::MessageSender>>();
        for (const auto user : reaction.recent) recent.push_back(api::make_object<api::messageSenderUser>(user));
        reactions->reactions_.push_back(api::make_object<api::messageReaction>(
            ReactionType(reaction.key), reaction.count, reaction.chosen,
            reaction.chosen ? api::make_object<api::messageSenderUser>(kSelfId) : nullptr, std::move(recent)));
      }
      info->reactions_ = std::move(reactions);
    }
    return info;
  }

  bool unreadReaction(std::int64_t chat, std::int64_t id) const {
    return !_reactionRead && chat == kUnreadReactionChat && id == MessagesInChat() * kIdStep;
  }

  void applyReactions(api::message &message) {
    message.interaction_info_ = interactionInfo(message.chat_id_, message.id_);
    if (unreadReaction(message.chat_id_, message.id_)) {
      message.unread_reactions_.push_back(api::make_object<api::unreadReaction>(
          ReactionType("\xF0\x9F\x94\xA5"), api::make_object<api::messageSenderUser>(message.chat_id_), false));
    }
  }

  void reactionsChanged(std::int64_t chat, std::int64_t id) {
    update(api::make_object<api::updateMessageInteractionInfo>(chat, id, interactionInfo(chat, id)));
  }

  // Which reactions may be set: in the channel only a few, in Saved Messages tags (Telegram Premium).
  std::vector<std::string> available(std::int64_t chat) const {
    if (chat == kReactionsChannelId) {
      return {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4", "\xF0\x9F\x94\xA5", "\xF0\x9F\x8E\x89", "\xF0\x9F\x98\x81"};
    }
    return {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4", "\xF0\x9F\x94\xA5", "\xF0\x9F\xA5\xB0", "\xF0\x9F\x91\x8F",
            "\xF0\x9F\x98\x81", "\xF0\x9F\xA4\x94", "\xF0\x9F\xA4\xAF", "\xF0\x9F\x98\xB1", "\xF0\x9F\x8E\x89"};
  }

  ObjectPtr addReaction(const api::addMessageReaction &request) {
    const auto key = ReactionKey(request.reaction_type_.get());
    if (!std::ranges::count(available(request.chat_id_), key)) return api::make_object<api::error>(400, "REACTION_INVALID");
    auto &list = reactionsOf(request.chat_id_, request.message_id_);
    // One reaction without Telegram Premium: the one chosen before goes, as TDLib does.
    for (auto &reaction : list) {
      if (!reaction.chosen || reaction.key == key) continue;
      reaction.chosen = false;
      --reaction.count;
      std::erase(reaction.recent, kSelfId);
    }
    std::erase_if(list, [](const Reaction &reaction) { return reaction.count <= 0; });
    auto it = std::ranges::find(list, key, &Reaction::key);
    if (it == list.end()) {
      list.push_back({key});
      it = list.end() - 1;
    }
    if (!it->chosen) {
      it->chosen = true;
      ++it->count;
      if (request.chat_id_ != kReactionsChannelId) it->recent.insert(it->recent.begin(), kSelfId);
      if (it->recent.size() > 3) it->recent.resize(3);
    }
    reactionsChanged(request.chat_id_, request.message_id_);
    return api::make_object<api::ok>();
  }

  ObjectPtr removeReaction(const api::removeMessageReaction &request) {
    const auto key = ReactionKey(request.reaction_type_.get());
    auto &list = reactionsOf(request.chat_id_, request.message_id_);
    const auto it = std::ranges::find(list, key, &Reaction::key);
    if (it == list.end() || !it->chosen) return api::make_object<api::ok>();
    it->chosen = false;
    --it->count;
    std::erase(it->recent, kSelfId);
    std::erase_if(list, [](const Reaction &reaction) { return reaction.count <= 0; });
    reactionsChanged(request.chat_id_, request.message_id_);
    return api::make_object<api::ok>();
  }

  // Everybody who reacted: the recent ones, then the group's other members.
  ObjectPtr addedReactions(const api::getMessageAddedReactions &request) {
    if (request.chat_id_ != kForumId) return api::make_object<api::error>(400, "MESSAGE_ID_INVALID");
    const auto filter = ReactionKey(request.reaction_type_.get());
    auto result = api::make_object<api::addedReactions>();
    for (const auto &reaction : reactionsOf(request.chat_id_, request.message_id_)) {
      if (!filter.empty() && reaction.key != filter) continue;
      auto users = reaction.recent;
      for (auto other = kForumUsers; int(users.size()) < reaction.count; ++other) {
        if (!std::ranges::count(users, other)) users.push_back(other);
      }
      for (const auto user : users) {
        result->reactions_.push_back(api::make_object<api::addedReaction>(
            ReactionType(reaction.key), api::make_object<api::messageSenderUser>(user), user == kSelfId, kNow - 60));
      }
    }
    result->total_count_ = int(result->reactions_.size());
    return result;
  }

  ObjectPtr customEmojiStickers(const api::getCustomEmojiStickers &request) {
    auto result = api::make_object<api::stickers>();
    for (const auto id : request.custom_emoji_ids_) {
      auto sticker = api::make_object<api::sticker>();
      sticker->id_ = id;
      sticker->width_ = sticker->height_ = 100;
      sticker->full_type_ = api::make_object<api::stickerFullTypeCustomEmoji>(id, false);
      if (id == kCustomStill) {
        sticker->emoji_ = "\xF0\x9F\x8D\x8A";
        sticker->format_ = api::make_object<api::stickerFormatWebp>();
        sticker->sticker_ = EmojiFileObject(kCustomStillFile, CustomStillPath(), _customDownloaded);
      } else if (id == kCustomAnimated) {
        sticker->emoji_ = "\xF0\x9F\x90\xB3";
        sticker->format_ = api::make_object<api::stickerFormatTgs>();
        sticker->thumbnail_ = api::make_object<api::thumbnail>(api::make_object<api::thumbnailFormatWebp>(), 100, 100,
                                                              EmojiFileObject(kCustomThumbnailFile, CustomThumbnailPath(),
                                                                              _customDownloaded));
        sticker->sticker_ = EmojiFileObject(kCustomAnimatedFile, std::string(), false);
      } else if (id == kCustomNoStill) {
        sticker->emoji_ = "\xF0\x9F\x90\x9F";
        sticker->format_ = api::make_object<api::stickerFormatTgs>();
        sticker->thumbnail_ = api::make_object<api::thumbnail>(api::make_object<api::thumbnailFormatTgs>(), 100, 100,
                                                              EmojiFileObject(kCustomAnimatedFile + 1, std::string(), false));
        sticker->sticker_ = EmojiFileObject(kCustomAnimatedFile + 2, std::string(), false);
      } else {
        continue;
      }
      result->stickers_.push_back(std::move(sticker));
    }
    return result;
  }

  void addReactionsChannel() {
    auto group = api::make_object<api::supergroup>();
    group->id_ = kReactionsChannelGroupId;
    group->member_count_ = 25000;
    group->is_channel_ = true;
    group->status_ = api::make_object<api::chatMemberStatusMember>(0);
    update(api::make_object<api::updateSupergroup>(std::move(group)));
    auto chat = api::make_object<api::chat>();
    chat->id_ = kReactionsChannelId;
    chat->type_ = api::make_object<api::chatTypeSupergroup>(kReactionsChannelGroupId, true);
    chat->title_ = "Reactions";
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    chat->last_message_ = messageAt(kReactionsChannelId, MessagesInChat());
    chat->last_read_inbox_message_id_ = MessagesInChat() * kIdStep;
    chat->positions_.push_back(
        api::make_object<api::chatPosition>(ListObject(false), kReactionsChannelOrder, false, nullptr));
    _orders[kReactionsChannelId] = {kReactionsChannelOrder, false};
    update(api::make_object<api::updateNewChat>(std::move(chat)));
  }

  // Somebody reacts to three posts of the channel every 150 ms while it is open.
  void burst() {
    for (auto i = 0; i < 3; ++i) {
      _burstSeed = _burstSeed * 1103515245 + 12345;
      const auto index = 1 + int((_burstSeed >> 8) % unsigned(MessagesInChat()));
      const auto id = index * kIdStep;
      auto &list = reactionsOf(kReactionsChannelId, id);
      if (!list.empty()) ++list.front().count;
      reactionsChanged(kReactionsChannelId, id);
      ++_burstUpdates;
    }
    if (_burstUpdates % 300 == 0) g_message("Scenario: %d reaction updates in the channel", _burstUpdates);
  }

  bool reactionRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::addMessageReaction::ID:
        answer(id, addReaction(static_cast<api::addMessageReaction &>(function)));
        return true;
      case api::removeMessageReaction::ID:
        answer(id, removeReaction(static_cast<api::removeMessageReaction &>(function)));
        return true;
      case api::getMessageAvailableReactions::ID: {
        const auto chat = static_cast<api::getMessageAvailableReactions &>(function).chat_id_;
        auto result = api::make_object<api::availableReactions>();
        for (const auto &key : available(chat)) {
          result->top_reactions_.push_back(api::make_object<api::availableReaction>(ReactionType(key), false));
        }
        result->are_tags_ = chat == kSelfId;
        answer(id, std::move(result));
        return true;
      }
      case api::getMessageAddedReactions::ID:
        answer(id, addedReactions(static_cast<api::getMessageAddedReactions &>(function)));
        return true;
      case api::getCustomEmojiStickers::ID:
        answer(id, customEmojiStickers(static_cast<api::getCustomEmojiStickers &>(function)));
        return true;
      case api::downloadFile::ID: {
        // The custom emoji pictures are not here until asked for.
        const auto file = static_cast<api::downloadFile &>(function).file_id_;
        if (file != kCustomStillFile && file != kCustomThumbnailFile) return false;
        _customDownloaded = true;
        answer(id, EmojiFileObject(file, file == kCustomStillFile ? CustomStillPath() : CustomThumbnailPath(), true));
        return true;
      }
      case api::viewMessages::ID: {
        // Seen on the screen: its new reaction is read, as TDLib tells of a message it holds.
        const auto &request = static_cast<api::viewMessages &>(function);
        for (const auto message : request.message_ids_) {
          if (!unreadReaction(request.chat_id_, message)) continue;
          _reactionRead = true;
          update(api::make_object<api::updateMessageUnreadReactions>(
              request.chat_id_, message, std::vector<api::object_ptr<api::unreadReaction>>(), 0));
        }
        return false;
      }
      case api::openChat::ID:
        if (static_cast<api::openChat &>(function).chat_id_ == kReactionsChannelId && !_burst) {
          _burst = g_timeout_add(kReactionBurstMs, +[](gpointer data) -> gboolean {
            static_cast<ScenarioTransport *>(data)->burst();
            return G_SOURCE_CONTINUE;
          }, this);
        }
        return false;
      case api::closeChat::ID:
        if (static_cast<api::closeChat &>(function).chat_id_ == kReactionsChannelId && _burst) {
          g_source_remove(_burst);
          _burst = 0;
        }
        return false;
      default:
        return false;
    }
  }

  // --- the showcase channel, sponsored messages, links, downloads ------------------

  void addShowcase() {
    auto group = api::make_object<api::supergroup>();
    group->id_ = kShowcaseGroupId;
    group->member_count_ = 1200;
    group->is_channel_ = true;
    group->status_ = api::make_object<api::chatMemberStatusMember>(0);
    update(api::make_object<api::updateSupergroup>(std::move(group)));
    auto chat = api::make_object<api::chat>();
    chat->id_ = kShowcaseId;
    chat->type_ = api::make_object<api::chatTypeSupergroup>(kShowcaseGroupId, true);
    chat->title_ = "Showcase";
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    chat->last_message_ = messageAt(kShowcaseId, kShowcaseMessages);
    chat->last_read_inbox_message_id_ = kShowcaseMessages * kIdStep;
    chat->positions_.push_back(api::make_object<api::chatPosition>(ListObject(false), kShowcaseOrder, false, nullptr));
    _orders[kShowcaseId] = {kShowcaseOrder, false};
    update(api::make_object<api::updateNewChat>(std::move(chat)));
  }

  // As TDLib has it now: the sticker is here from the start, the rest once its download has come.
  api::object_ptr<api::file> showcaseFile(std::int32_t id) {
    const auto size = ShowcaseSize(id);
    const auto it = _downloads.find(id);
    const auto downloaded = (id == kShowcaseStickerFile) ? size : (it == _downloads.end() ? 0 : it->second.downloaded);
    const auto complete = downloaded >= size;
    auto local = api::make_object<api::localFile>();
    local->path_ = complete ? ShowcasePath(id) : std::string();
    local->can_be_downloaded_ = true;
    local->is_downloading_active_ = it != _downloads.end() && it->second.active;
    local->is_downloading_completed_ = complete;
    local->downloaded_size_ = downloaded;
    local->downloaded_prefix_size_ = downloaded;
    auto file = api::make_object<api::file>();
    file->id_ = id;
    file->size_ = size;
    file->local_ = std::move(local);
    file->remote_ = api::make_object<api::remoteFile>();
    return file;
  }

  api::object_ptr<api::message> showcaseMessage(int index) {
    auto message = MakeMessage(kShowcaseId, index * kIdStep, kSelfId, kNow - (kShowcaseMessages - index) * 600, "");
    message->sender_id_ = api::make_object<api::messageSenderChat>(kShowcaseId);
    message->is_outgoing_ = false;
    message->is_channel_post_ = true;
    const auto user = [&](std::int64_t id) {
      message->sender_id_ = api::make_object<api::messageSenderUser>(id);
      message->is_channel_post_ = false;
    };
    switch (index) {
      case 1: message->content_ = api::make_object<api::messageSupergroupChatCreate>("Showcase"); break;
      case 2:
        message->content_ = TextContent(
            Marked("😀 Bold, italic and a link; 👍 then @friend1, #contrail, /help, t.me/friend1/5 and a tg link")
                .mark("Bold", api::make_object<api::textEntityTypeBold>())
                .mark("italic", api::make_object<api::textEntityTypeItalic>())
                .mark("a link", api::make_object<api::textEntityTypeTextUrl>("https://example.org/page"))
                .mark("@friend1", api::make_object<api::textEntityTypeMention>())
                .mark("#contrail", api::make_object<api::textEntityTypeHashtag>())
                .mark("/help", api::make_object<api::textEntityTypeBotCommand>())
                .mark("t.me/friend1/5", api::make_object<api::textEntityTypeUrl>())
                .mark("a tg link", api::make_object<api::textEntityTypeTextUrl>("tg://resolve?domain=friend2"))
                .take());
        break;
      case 3: message->content_ = api::make_object<api::messageChatChangeTitle>("Showcase"); break;
      case 4: {
        auto video = api::make_object<api::video>();
        video->duration_ = 42;
        video->width_ = 1280;
        video->height_ = 720;
        video->file_name_ = "clip.mp4";
        video->mime_type_ = "video/mp4";
        video->minithumbnail_ = MiniThumbnail();
        video->video_ = showcaseFile(kShowcaseVideoFile);
        auto content = api::make_object<api::messageVideo>();
        content->video_ = std::move(video);
        content->caption_ = Formatted("A video");
        message->content_ = std::move(content);
        break;
      }
      case 5: {
        auto animation = api::make_object<api::animation>();
        animation->duration_ = 3;
        animation->width_ = 320;
        animation->height_ = 240;
        animation->file_name_ = "loop.mp4";
        animation->mime_type_ = "video/mp4";
        animation->minithumbnail_ = MiniThumbnail();
        animation->animation_ = showcaseFile(kShowcaseGifFile);
        auto content = api::make_object<api::messageAnimation>();
        content->animation_ = std::move(animation);
        content->caption_ = Formatted(std::string());
        message->content_ = std::move(content);
        break;
      }
      case 6: {
        auto sticker = api::make_object<api::sticker>();
        sticker->id_ = 77;
        sticker->width_ = 512;
        sticker->height_ = 512;
        sticker->emoji_ = "😀";
        sticker->format_ = api::make_object<api::stickerFormatWebp>();
        sticker->full_type_ = api::make_object<api::stickerFullTypeRegular>(nullptr);
        sticker->sticker_ = showcaseFile(kShowcaseStickerFile);
        message->content_ = api::make_object<api::messageSticker>(std::move(sticker), false);
        break;
      }
      case 7:
      case 8: {
        auto content = api::make_object<api::messagePoll>();
        content->poll_ = MakePoll(index == 8);
        content->description_ = Formatted(std::string());
        message->content_ = std::move(content);
        break;
      }
      case 9:
        message->content_ = api::make_object<api::messageContact>(
            api::make_object<api::contact>("491701234567", "Anna", "K", std::string(), 0));
        break;
      case 10: {
        auto content = api::move_object_as<api::messageText>(TextContent(
            Marked("Read https://example.org/article")
                .mark("https://example.org/article", api::make_object<api::textEntityTypeUrl>())
                .take()));
        auto photo = api::make_object<api::photo>();
        photo->minithumbnail_ = MiniThumbnail();
        photo->sizes_.push_back(PhotoSize(3, "m", 320, 240));
        photo->sizes_.push_back(PhotoSize(3, "w", 2560, 1920));
        auto page = api::make_object<api::linkPreview>();
        page->url_ = "https://example.org/article";
        page->display_url_ = "example.org/article";
        page->site_name_ = "Example";
        page->title_ = "A page with a picture";
        page->description_ = Formatted("Its picture comes with the preview, and the chat goes on scrolling.");
        page->type_ = api::make_object<api::linkPreviewTypeArticle>(std::move(photo));
        content->link_preview_ = std::move(page);
        message->content_ = std::move(content);
        break;
      }
      case 11:
        user(2);
        message->content_ = api::make_object<api::messageChatAddMembers>(std::vector<std::int64_t>{4});
        break;
      case 12: message->content_ = api::make_object<api::messagePinMessage>(2 * kIdStep); break;
      case 13: {
        auto photo = api::make_object<api::chatPhoto>();
        photo->sizes_.push_back(PhotoSize(4, "a", 320, 240));
        message->content_ = api::make_object<api::messageChatChangePhoto>(std::move(photo));
        break;
      }
      case 14: {
        auto document = api::make_object<api::document>();
        document->file_name_ = "report.pdf";
        document->mime_type_ = "application/pdf";
        document->document_ = showcaseFile(kShowcaseDocumentFile);
        message->content_ = api::make_object<api::messageDocument>(std::move(document), Formatted(std::string()));
        break;
      }
      case 15:
        message->content_ = TextContent(Formatted(
            "A long line to wrap: "
            "https://example.org/a/very/long/path/without/any/spaces/that/has/to/break/somewhere/inside/the/bubble "
            "and Donaudampfschifffahrtsgesellschaftskapitänswitwenrentenversicherungsanstalt, then 😀😀😀 at the "
            "end of it all, written to be wider than any phone and most windows."));
        break;
      case 16:
        user(3);
        message->content_ = api::make_object<api::messageChatJoinByLink>();
        break;
      case 17:
        user(5);
        message->content_ = api::make_object<api::messageChatDeleteMember>(5);
        break;
      case 18:
        message->content_ = TextContent(Marked("🎉 A secret: hidden words 🙈, ask Friend 2 about it")
                                            .mark("hidden words", api::make_object<api::textEntityTypeSpoiler>())
                                            .mark("Friend 2", api::make_object<api::textEntityTypeMentionName>(2))
                                            .take());
        break;
      case 19: message->content_ = api::make_object<api::messageChatDeletePhoto>(); break;
      default: {
        auto content = api::move_object_as<api::messagePhoto>(PhotoContent(index));
        content->caption_ = Formatted("A photo with a caption");
        message->content_ = std::move(content);
        break;
      }
    }
    return message;
  }

  api::object_ptr<api::sponsoredMessages> sponsoredMessages(std::int64_t chat) {
    auto result = api::make_object<api::sponsoredMessages>();
    if (chat != kShowcaseId) return result;
    auto message = api::make_object<api::sponsoredMessage>();
    message->message_id_ = kSponsoredMessageId;
    message->can_be_reported_ = true;
    message->content_ = TextContent(Formatted("Contrail: Telegram for the phones that run Linux."));
    message->sponsor_ = api::make_object<api::advertisementSponsor>("https://t.me/friend1", nullptr,
                                                                     "Sponsor: Example Ltd");
    message->title_ = "Contrail";
    message->button_text_ = "View Channel";
    message->additional_info_ = "Ads by Telegram";
    result->messages_.push_back(std::move(message));
    return result;
  }

  // "friend3" is Friend 3's chat; "t.me/friend3/45" its message 45.
  static std::int64_t FriendOf(std::string_view name) {
    if (!name.starts_with("friend")) return 0;
    const auto id = std::atoll(std::string(name.substr(6)).c_str());
    return (id >= 1 && id <= kChatCount) ? id : 0;
  }

  static std::string LinkPath(std::string link) {
    for (const auto *prefix : {"https://", "http://", "t.me/", "tg://resolve?domain="}) {
      if (link.starts_with(prefix)) link.erase(0, std::string_view(prefix).size());
    }
    return link;
  }

  ObjectPtr internalLink(const std::string &link) {
    const auto path = LinkPath(link);
    if (!link.starts_with("tg:") && link.find("t.me/") == std::string::npos) {
      return api::make_object<api::error>(400, "Not a Telegram link");
    }
    if (link.starts_with("tg://socks?")) return proxyLink(link);
    const auto slash = path.find('/');
    const auto name = path.substr(0, slash);
    if (slash != std::string::npos) return api::make_object<api::internalLinkTypeMessage>(link);
    if (link.starts_with("tg://resolve?")) {
      return api::make_object<api::internalLinkTypePublicChat>(name, std::string(), false, std::string());
    }
    if (link.starts_with("tg:")) return api::make_object<api::internalLinkTypeUnknownDeepLink>(link);
    return api::make_object<api::internalLinkTypePublicChat>(name, std::string(), false, std::string());
  }

  ObjectPtr messageLinkInfo(const std::string &link) {
    const auto path = LinkPath(link);
    const auto slash = path.find('/');
    const auto chat = FriendOf(path.substr(0, slash));
    if (!chat || slash == std::string::npos) return api::make_object<api::error>(400, "Not found");
    auto info = api::make_object<api::messageLinkInfo>();
    info->is_public_ = true;
    info->chat_id_ = chat;
    info->message_ = messageAt(chat, std::atoi(path.c_str() + slash + 1));
    return info;
  }

  void downloadTick() {
    auto any = false;
    for (auto &[file, download] : _downloads) {
      if (!download.active) continue;
      download.downloaded = std::min(download.downloaded + kDownloadStep, ShowcaseSize(file));
      download.active = download.downloaded < ShowcaseSize(file);
      any = any || download.active;
      update(api::make_object<api::updateFile>(showcaseFile(file)));
    }
    if (!any && _downloadTicker) {
      g_source_remove(_downloadTicker);
      _downloadTicker = 0;
    }
  }

  bool showcaseRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::getChatSponsoredMessages::ID:
        answer(id, sponsoredMessages(static_cast<api::getChatSponsoredMessages &>(function).chat_id_));
        return true;
      case api::clickChatSponsoredMessage::ID: {
        const auto &request = static_cast<api::clickChatSponsoredMessage &>(function);
        g_message("Scenario: sponsored message %lld clicked, media=%d", (long long)request.message_id_,
                  int(request.is_media_click_));
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::viewMessages::ID: {
        const auto &request = static_cast<api::viewMessages &>(function);
        if (request.chat_id_ == kShowcaseId && std::ranges::count(request.message_ids_, kSponsoredMessageId)) {
          g_message("Scenario: sponsored message %lld viewed", (long long)kSponsoredMessageId);
        }
        return false;
      }
      case api::searchPublicChat::ID: {
        const auto chat = FriendOf(static_cast<api::searchPublicChat &>(function).username_);
        if (chat) answer(id, makeChat(chat));
        else answer(id, api::make_object<api::error>(400, "USERNAME_NOT_OCCUPIED"));
        return true;
      }
      case api::getInternalLinkType::ID:
        answer(id, internalLink(static_cast<api::getInternalLinkType &>(function).link_));
        return true;
      case api::getMessageLinkInfo::ID:
        answer(id, messageLinkInfo(static_cast<api::getMessageLinkInfo &>(function).url_));
        return true;
      case api::resendMessages::ID:
        answer(id, resend(static_cast<api::resendMessages &>(function)));
        return true;
      case api::downloadFile::ID: {
        const auto file = static_cast<api::downloadFile &>(function).file_id_;
        if (file >= 1000000 && file < 2000000) {
          // A photo: on disk from the start, and TDLib tells so in its answer.
          const auto large = file % 10 == 1;
          auto size = PhotoSize((file - 1000000) / 10, "x", large ? 2560 : 320, large ? 1920 : 240);
          answer(id, std::move(size->photo_));
          return true;
        }
        if (file < kShowcaseVideoFile || file > kShowcaseDocumentFile) return false;
        auto &download = _downloads[file];
        if (file != kShowcaseStickerFile && download.downloaded < ShowcaseSize(file)) {
          download.active = true;
          if (!_downloadTicker) {
            _downloadTicker = g_timeout_add(kDownloadStepMs, +[](gpointer data) -> gboolean {
              static_cast<ScenarioTransport *>(data)->downloadTick();
              return G_SOURCE_CONTINUE;
            }, this);
          }
        }
        answer(id, showcaseFile(file));
        return true;
      }
      case api::cancelDownloadFile::ID: {
        const auto file = static_cast<api::cancelDownloadFile &>(function).file_id_;
        if (const auto it = _downloads.find(file); it != _downloads.end() && it->second.active) {
          it->second.active = false;
          update(api::make_object<api::updateFile>(showcaseFile(file)));
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      default:
        return false;
    }
  }

  // --- the group with topics ---------------------------------------------------

  struct Topic {
    std::int32_t id = 0;
    std::string name;
    int last = kTopicMessages;  // Its messages are 1..last.
    std::int64_t readInbox = 0;
    int mentions = 0;
    bool pinned = false;
    bool closed = false;
    bool muted = false;
    std::string draft;
  };

  // A message id of the group: the topic in the lowest digit, so the topics' messages go in turn.
  static std::int64_t TopicMessageId(std::int32_t topic, int index) { return (std::int64_t(index) * 10 + topic) * kIdStep; }
  static std::int32_t TopicOfId(std::int64_t id) { return std::int32_t((id / kIdStep) % 10); }
  static int IndexOfId(std::int64_t id) { return int((id / kIdStep) / 10); }

  Topic *findTopic(std::int32_t id) {
    const auto it = std::ranges::find_if(_topics, [&](const Topic &topic) { return topic.id == id; });
    return it == _topics.end() ? nullptr : &*it;
  }

  static std::int64_t SenderOf(std::int32_t topic, int index) {
    return index % 3 == 0 ? kSelfId : kForumUsers + (index + topic) % 3;
  }

  api::object_ptr<api::message> topicMessage(std::int32_t topic, int index, const std::string &text = std::string()) {
    const auto id = TopicMessageId(topic, index);
    const auto date = std::min(kNow - (kTopicMessages - index) * 600 + topic * 10, kNow);
    auto message = MakeMessage(kForumId, id, SenderOf(topic, index), date,
                               text.empty() ? "Topic " + std::to_string(topic) + ", message " + std::to_string(index) : text);
    message->topic_id_ = api::make_object<api::messageTopicForum>(topic);
    applyEntry(*message);
    message->is_pinned_ = isPinned(kForumId, id);
    applyReactions(*message);
    return message;
  }

  int unreadOf(const Topic &topic) {
    auto result = 0;
    for (auto index = topic.last; index >= 1 && TopicMessageId(topic.id, index) > topic.readInbox; --index) {
      if (SenderOf(topic.id, index) != kSelfId) ++result;
    }
    return result;
  }

  api::object_ptr<api::chatNotificationSettings> topicSettings(const Topic &topic) {
    auto settings = api::make_object<api::chatNotificationSettings>();
    settings->use_default_mute_for_ = !topic.muted;
    settings->mute_for_ = topic.muted ? 0x7fffffff : 0;
    return settings;
  }

  api::object_ptr<api::draftMessage> topicDraft(const Topic &topic) {
    if (topic.draft.empty()) return nullptr;
    auto content = api::make_object<api::draftMessageContentText>();
    content->text_ = Formatted(topic.draft);
    auto draft = api::make_object<api::draftMessage>();
    draft->content_ = std::move(content);
    return draft;
  }

  api::object_ptr<api::forumTopic> forumTopic(const Topic &topic) {
    auto info = api::make_object<api::forumTopicInfo>(
        kForumId, topic.id, topic.name, api::make_object<api::forumTopicIcon>(0x6FB9F0, 0), kNow - 86400 * 30,
        api::make_object<api::messageSenderUser>(kForumUsers), topic.id == 1, false, topic.closed, false, false);
    auto result = api::make_object<api::forumTopic>();
    result->info_ = std::move(info);
    result->last_message_ = topicMessage(topic.id, topic.last, topic.last > kTopicMessages ? _sentTexts[TopicMessageId(topic.id, topic.last)] : "");
    result->order_ = result->last_message_->id_;
    result->is_pinned_ = topic.pinned;
    result->unread_count_ = unreadOf(topic);
    result->last_read_inbox_message_id_ = topic.readInbox;
    result->last_read_outbox_message_id_ = TopicMessageId(topic.id, topic.last);
    result->unread_mention_count_ = topic.mentions;
    result->notification_settings_ = topicSettings(topic);
    result->draft_message_ = topicDraft(topic);
    return result;
  }

  void topicChanged(const Topic &topic) {
    update(api::make_object<api::updateForumTopic>(kForumId, topic.id, topic.pinned, topic.readInbox,
                                                   TopicMessageId(topic.id, topic.last), topic.mentions, 0, 0,
                                                   topicSettings(topic), topicDraft(topic)));
  }

  // The group's last message, unread count and mentions follow its topics.
  api::object_ptr<api::message> forumLastMessage() {
    const Topic *newest = nullptr;
    for (const auto &topic : _topics) {
      if (!newest || TopicMessageId(topic.id, topic.last) > TopicMessageId(newest->id, newest->last)) newest = &topic;
    }
    const auto id = TopicMessageId(newest->id, newest->last);
    return topicMessage(newest->id, newest->last, newest->last > kTopicMessages ? _sentTexts[id] : "");
  }

  void forumCounters() {
    auto unread = 0, mentions = 0;
    auto read = std::int64_t(0);
    for (const auto &topic : _topics) {
      unread += unreadOf(topic);
      mentions += topic.mentions;
      read = std::max(read, topic.readInbox);
    }
    update(api::make_object<api::updateChatReadInbox>(kForumId, read, unread));
    update(api::make_object<api::updateChatUnreadMentionCount>(kForumId, mentions));
  }

  void addForum() {
    update(api::make_object<api::updateUser>(MakeUser(kForumUsers, "Anna", "K")));
    update(api::make_object<api::updateUser>(MakeUser(kForumUsers + 1, "Boris", "L")));
    update(api::make_object<api::updateUser>(MakeUser(kForumUsers + 2, "Clara", "M")));
    _topics = {
        {.id = 1, .name = "General"},
        {.id = 2, .name = "Announcements", .pinned = true, .closed = true},
        {.id = 3, .name = "Random", .mentions = 1},
        {.id = 4, .name = "Help", .muted = true},
    };
    for (auto &topic : _topics) topic.readInbox = TopicMessageId(topic.id, topic.last);
    // Random: the last few messages not read yet.
    findTopic(3)->readInbox = TopicMessageId(3, kTopicMessages - 4);
    supergroup(true);
    auto chat = api::make_object<api::chat>();
    chat->id_ = kForumId;
    chat->type_ = api::make_object<api::chatTypeSupergroup>(kForumGroupId, false);
    chat->title_ = "Contrail Club";
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    chat->notification_settings_->use_default_mute_for_ = true;
    chat->last_message_ = forumLastMessage();
    chat->view_as_topics_ = true;
    chat->positions_.push_back(api::make_object<api::chatPosition>(ListObject(false), kForumOrder, false, nullptr));
    _orders[kForumId] = {kForumOrder, false};
    update(api::make_object<api::updateNewChat>(std::move(chat)));
    forumCounters();
  }

  void supergroup(bool forum) {
    auto group = api::make_object<api::supergroup>();
    group->id_ = kForumGroupId;
    group->member_count_ = 12;
    group->is_forum_ = forum;
    update(api::make_object<api::updateSupergroup>(std::move(group)));
  }

  ObjectPtr forumHistory(const api::getForumTopicHistory &request) {
    const auto topic = findTopic(request.forum_topic_id_);
    if (!topic) return api::make_object<api::error>(400, "TOPIC_ID_INVALID");
    // As in history(): from_message_id itself only with a negative offset. It may be of another topic.
    const auto from = request.from_message_id_;
    auto ids = std::vector<std::int64_t>();
    for (auto index = 1; index <= topic->last; ++index) ids.push_back(TopicMessageId(topic->id, index));
    auto position = from ? int(std::ranges::lower_bound(ids, from) - ids.begin()) : int(ids.size());
    const auto exists = position < int(ids.size()) && ids[position] == from;
    auto newest = exists ? (request.offset_ == 0 ? position - 1 : position - request.offset_) : position - 1 - request.offset_;
    newest = std::min(newest, int(ids.size()) - 1);
    auto result = api::make_object<api::messages>();
    for (auto i = newest; i >= 0 && int(result->messages_.size()) < request.limit_; --i) {
      const auto index = i + 1;
      result->messages_.push_back(topicMessage(topic->id, index, index > kTopicMessages ? _sentTexts[ids[i]] : ""));
    }
    result->total_count_ = int(ids.size());
    return result;
  }

  // "/in N text" and "/unforum" do what the other side would; anything else is sent into its topic.
  ObjectPtr forumSent(api::sendMessage &request) {
    const auto topicId = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
        ? static_cast<api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 1;
    auto text = std::string();
    if (request.input_message_content_ && request.input_message_content_->get_id() == api::inputMessageText::ID) {
      text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
    }
    if (text == "/unforum") {
      supergroup(false);
      return api::make_object<api::error>(400, "Scenario: the group has no topics now");
    }
    if (text.rfind("/pin ", 0) == 0) {
      setPinned(kForumId, TopicMessageId(topicId, std::atoi(text.c_str() + 5)), true);
      return api::make_object<api::error>(400, "Scenario: pinned by an admin");
    }
    auto incoming = text.rfind("/in ", 0) == 0;
    auto into = findTopic(topicId);
    if (incoming) {
      into = findTopic(std::atoi(text.c_str() + 4));
      const auto gap = text.find(' ', 4);
      text = gap == std::string::npos ? std::string("Hello") : text.substr(gap + 1);
    }
    if (!into) return api::make_object<api::error>(400, "TOPIC_ID_INVALID");
    // Every third index is this account's (SenderOf): skip to one of the right side.
    auto index = into->last + 1;
    while ((SenderOf(into->id, index) == kSelfId) != !incoming) ++index;
    into->last = index;
    const auto final = TopicMessageId(into->id, index);
    _sentTexts[final] = text;
    auto message = topicMessage(into->id, index, text);
    message->date_ = kNow;
    if (incoming) {
      update(api::make_object<api::updateNewMessage>(std::move(message)));
      update(api::make_object<api::updateChatLastMessage>(kForumId, forumLastMessage(),
                                                          std::vector<api::object_ptr<api::chatPosition>>()));
      forumCounters();
      return api::make_object<api::error>(400, "Scenario: a message came into topic " + std::to_string(into->id));
    }
    into->draft.clear();
    return sendInto(*into, index, std::move(message));
  }

  // This account's message `index` of a topic, sent there now.
  api::object_ptr<api::message> sendInto(Topic &into, int index, api::object_ptr<api::message> message) {
    const auto temporary = TopicMessageId(into.id, index) + 1;
    const auto text = _sentTexts[TopicMessageId(into.id, index)];
    auto pending = topicMessage(into.id, index, text);
    pending->id_ = temporary;
    pending->date_ = kNow;
    pending->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
    auto shown = topicMessage(into.id, index, text);
    shown->id_ = temporary;
    shown->date_ = kNow;
    update(api::make_object<api::updateNewMessage>(std::move(shown)));
    update(api::make_object<api::updateMessageSendSucceeded>(std::move(message), temporary));
    update(api::make_object<api::updateChatLastMessage>(kForumId, forumLastMessage(),
                                                        std::vector<api::object_ptr<api::chatPosition>>()));
    topicChanged(into);
    return pending;
  }

  api::object_ptr<api::message> forumOutgoing(Topic &into, Entry entry) {
    auto index = into.last + 1;
    while (SenderOf(into.id, index) != kSelfId) ++index;
    into.last = index;
    const auto id = TopicMessageId(into.id, index);
    _sentTexts[id] = entry.text;
    _entries[{kForumId, id}] = std::move(entry);
    auto message = topicMessage(into.id, index, _sentTexts[id]);
    return sendInto(into, index, std::move(message));
  }

  // The requests about the group with topics; false for anything else.
  bool forumRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::getForumTopics::ID: {
        auto result = api::make_object<api::forumTopics>();
        if (static_cast<api::getForumTopics &>(function).chat_id_ == kForumId) {
          for (const auto &topic : _topics) result->topics_.push_back(forumTopic(topic));
        }
        result->total_count_ = int(result->topics_.size());
        answer(id, std::move(result));
        return true;
      }
      case api::getForumTopic::ID: {
        const auto &request = static_cast<api::getForumTopic &>(function);
        const auto topic = request.chat_id_ == kForumId ? findTopic(request.forum_topic_id_) : nullptr;
        if (topic) answer(id, forumTopic(*topic));
        else answer(id, api::make_object<api::error>(400, "TOPIC_ID_INVALID"));
        return true;
      }
      case api::getForumTopicHistory::ID:
        answer(id, forumHistory(static_cast<api::getForumTopicHistory &>(function)));
        return true;
      case api::setForumTopicNotificationSettings::ID: {
        auto &request = static_cast<api::setForumTopicNotificationSettings &>(function);
        if (const auto topic = findTopic(request.forum_topic_id_)) {
          const auto &settings = request.notification_settings_;
          topic->muted = settings && !settings->use_default_mute_for_ && settings->mute_for_ > 0;
          topicChanged(*topic);
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::readAllForumTopicMentions::ID: {
        if (const auto topic = findTopic(static_cast<api::readAllForumTopicMentions &>(function).forum_topic_id_)) {
          topic->mentions = 0;
          topicChanged(*topic);
          forumCounters();
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::readAllForumTopicReactions::ID:
        answer(id, api::make_object<api::ok>());
        return true;
      case api::sendMessage::ID: {
        auto &request = static_cast<api::sendMessage &>(function);
        if (request.chat_id_ != kForumId) return false;
        answer(id, forumSent(request));
        return true;
      }
      case api::viewMessages::ID: {
        auto &request = static_cast<api::viewMessages &>(function);
        if (request.chat_id_ != kForumId) return false;
        for (const auto message : request.message_ids_) {
          const auto topic = findTopic(TopicOfId(message));
          if (!topic || message <= topic->readInbox) continue;
          topic->readInbox = message;
          if (message >= TopicMessageId(topic->id, topic->last)) topic->mentions = 0;
          topicChanged(*topic);
        }
        forumCounters();
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::setChatDraftMessage::ID: {
        auto &request = static_cast<api::setChatDraftMessage &>(function);
        if (request.chat_id_ != kForumId) return false;
        const auto forum = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
            ? static_cast<api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 0;
        if (const auto topic = findTopic(forum)) {
          topic->draft.clear();
          const auto &draft = request.draft_message_;
          if (draft && draft->content_ && draft->content_->get_id() == api::draftMessageContentText::ID) {
            const auto &text = static_cast<const api::draftMessageContentText &>(*draft->content_).text_;
            if (text) topic->draft = text->text_;
          }
          topicChanged(*topic);
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::getMessage::ID:
      case api::getRepliedMessage::ID: {
        const auto chat = function.get_id() == api::getMessage::ID
            ? static_cast<api::getMessage &>(function).chat_id_ : static_cast<api::getRepliedMessage &>(function).chat_id_;
        if (chat != kForumId) return false;
        const auto message = function.get_id() == api::getMessage::ID
            ? static_cast<api::getMessage &>(function).message_id_
            : static_cast<api::getRepliedMessage &>(function).message_id_;
        const auto topic = findTopic(TopicOfId(message));
        const auto index = IndexOfId(message);
        if (!topic || index < 1 || index > topic->last) {
          answer(id, api::make_object<api::error>(404, "Not Found"));
        } else {
          answer(id, topicMessage(topic->id, index, index > kTopicMessages ? _sentTexts[message] : ""));
        }
        return true;
      }
      case api::getChatHistory::ID:
        // Its messages are read in the topics.
        if (static_cast<api::getChatHistory &>(function).chat_id_ != kForumId) return false;
        answer(id, api::make_object<api::messages>(0, std::vector<api::object_ptr<api::message>>()));
        return true;
      default:
        return false;
    }
  }

  // --- settings and the app in the background ------------------------------------

  template <typename Type>
  static const Type *As(const api::Object *object) {
    return object && object->get_id() == Type::ID ? static_cast<const Type *>(object) : nullptr;
  }

  bool backgroundRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::loadChats::ID:
        // The badge of the tray icon: the friends with unread messages (every fourth), none of them muted.
        if (!_unreadCountSent) {
          _unreadCountSent = true;
          const auto unread = kChatCount / 4;
          update(api::make_object<api::updateUnreadChatCount>(ListObject(false), kChatCount, unread, unread, 0, 0));
        }
        return false;
      case api::getMe::ID: {
        auto me = MakeUser(kSelfId, "Vlad", "");
        me->phone_number_ = "15550001000";
        answer(id, std::move(me));
        return true;
      }
      case api::setOption::ID: {
        auto &request = static_cast<api::setOption &>(function);
        if (request.name_ == "notification_group_count_max") {
          const auto value = As<api::optionValueInteger>(request.value_.get());
          _notificationsOn = value && value->value_ > 0;
        }
        return false;
      }
      case api::sendMessage::ID: {
        auto &request = static_cast<api::sendMessage &>(function);
        const auto input = As<api::inputMessageText>(request.input_message_content_.get());
        const auto text = input && input->text_ ? input->text_->text_ : std::string();
        // "/notify" or "/notify2": a message or two come to Friend 5 a second later, with their notifications.
        if (text == "/notify" || text == "/notify2") {
          later(1000, [this, count = text == "/notify2" ? 2 : 1] {
            for (auto i = 0; i != count; ++i) notifyIncoming();
          });
        }
        if (request.chat_id_ == kNotifyChat) {
          const auto reply = As<api::inputMessageReplyToMessage>(request.reply_to_.get());
          g_message("scenario: sent to %lld \"%s\" reply_to=%lld", static_cast<long long>(request.chat_id_),
                    text.c_str(), static_cast<long long>(reply ? reply->message_id_ : 0));
        }
        return false;
      }
      case api::viewMessages::ID: {
        auto &request = static_cast<api::viewMessages &>(function);
        // Read here or anywhere: TDLib takes the chat's notifications away.
        if (request.chat_id_ == kNotifyChat && !_notes.empty()) {
          update(api::make_object<api::updateNotificationGroup>(
              kNotifyGroup, api::make_object<api::notificationGroupTypeMessages>(), kNotifyChat, kNotifyChat, 1, 0,
              std::vector<api::object_ptr<api::notification>>(), std::exchange(_notes, {})));
        }
        return false;
      }
      case api::removeNotificationGroup::ID:
        g_message("scenario: notifications removed");
        _notes.clear();
        answer(id, api::make_object<api::ok>());
        return true;
      case api::setNetworkType::ID:
        g_message("scenario: network %d", static_cast<api::setNetworkType &>(function).type_->get_id());
        answer(id, api::make_object<api::ok>());
        return true;
      case api::getProxies::ID:
        answer(id, addedProxies());
        return true;
      case api::addProxy::ID:
      case api::editProxy::ID: {
        const auto edit = function.get_id() == api::editProxy::ID;
        auto &proxy = edit ? static_cast<api::editProxy &>(function).proxy_ : static_cast<api::addProxy &>(function).proxy_;
        const auto enable = edit ? static_cast<api::editProxy &>(function).enable_ : static_cast<api::addProxy &>(function).enable_;
        auto it = edit ? std::ranges::find(_proxies, static_cast<api::editProxy &>(function).proxy_id_, &Proxy::id)
                       : _proxies.end();
        if (edit && it == _proxies.end()) {
          answer(id, api::make_object<api::error>(400, "PROXY_NOT_FOUND"));
          return true;
        }
        if (!edit) it = _proxies.insert(_proxies.end(), Proxy{.id = ++_proxyIds});
        it->proxy = CopyProxy(*proxy);
        if (enable) useProxy(it->id);
        answer(id, addedProxy(*it));
        return true;
      }
      case api::enableProxy::ID:
        useProxy(static_cast<api::enableProxy &>(function).proxy_id_);
        answer(id, api::make_object<api::ok>());
        return true;
      case api::disableProxy::ID:
        useProxy(0);
        answer(id, api::make_object<api::ok>());
        return true;
      case api::removeProxy::ID:
        std::erase_if(_proxies, [&](const Proxy &proxy) { return proxy.id == static_cast<api::removeProxy &>(function).proxy_id_; });
        answer(id, api::make_object<api::ok>());
        return true;
      case api::pingProxy::ID: {
        // A server named "down..." doesn't answer.
        const auto &proxy = static_cast<api::pingProxy &>(function).proxy_;
        if (proxy && proxy->server_.starts_with("down")) answer(id, api::make_object<api::error>(400, "Connection refused"));
        else answer(id, api::make_object<api::seconds>(0.042));
        return true;
      }
      case api::logOut::ID:
        // As TDLib does: the session ends and its data goes; the program starts a new client.
        answer(id, api::make_object<api::ok>());
        authorization(api::make_object<api::authorizationStateLoggingOut>());
        later(300, [this] { authorization(api::make_object<api::authorizationStateClosed>()); });
        return true;
      default:
        return false;
    }
  }

  // A message from Friend 5, and its notification when the program asked for them.
  void notifyIncoming() {
    const auto chat = kNotifyChat;
    const auto index = MessagesInChat() + 2000 + (++_notified);
    const auto text = "Are you coming tonight? " + std::to_string(_notified);
    _orders[chat].first = std::int64_t(3000 + _notified) << 32;
    auto positions = std::vector<api::object_ptr<api::chatPosition>>();
    positions.push_back(api::make_object<api::chatPosition>(ListObject(false), _orders[chat].first, false, nullptr));
    update(api::make_object<api::updateNewMessage>(MakeMessage(chat, index * kIdStep, chat, kNow, text)));
    update(api::make_object<api::updateChatLastMessage>(chat, MakeMessage(chat, index * kIdStep, chat, kNow, text),
                                                        std::move(positions)));
    update(api::make_object<api::updateChatReadInbox>(chat, (MessagesInChat() - 1) * kIdStep, int(_notes.size()) + 1));
    if (!_notificationsOn) return;
    const auto notification = 100 + _notified;
    auto added = std::vector<api::object_ptr<api::notification>>();
    added.push_back(api::make_object<api::notification>(
        notification, kNow, false,
        api::make_object<api::notificationTypeNewMessage>(MakeMessage(chat, index * kIdStep, chat, kNow, text), true)));
    _notes.push_back(notification);
    update(api::make_object<api::updateNotificationGroup>(
        kNotifyGroup, api::make_object<api::notificationGroupTypeMessages>(), chat, chat, 1, int(_notes.size()),
        std::move(added), std::vector<std::int32_t>()));
  }

  struct Proxy {
    std::int32_t id = 0;
    api::object_ptr<api::proxy> proxy;
    bool enabled = false;
    std::int32_t lastUsed = 0;
  };
  static api::object_ptr<api::proxy> CopyProxy(const api::proxy &proxy) {
    auto type = api::object_ptr<api::ProxyType>();
    if (const auto socks = As<api::proxyTypeSocks5>(proxy.type_.get())) {
      type = api::make_object<api::proxyTypeSocks5>(socks->username_, socks->password_);
    } else if (const auto http = As<api::proxyTypeHttp>(proxy.type_.get())) {
      type = api::make_object<api::proxyTypeHttp>(http->username_, http->password_, http->http_only_);
    } else if (const auto mtproto = As<api::proxyTypeMtproto>(proxy.type_.get())) {
      type = api::make_object<api::proxyTypeMtproto>(mtproto->secret_);
    }
    return api::make_object<api::proxy>(proxy.server_, proxy.port_, std::move(type));
  }
  api::object_ptr<api::addedProxy> addedProxy(const Proxy &proxy) {
    return api::make_object<api::addedProxy>(proxy.id, proxy.lastUsed, proxy.enabled, std::string(),
                                             CopyProxy(*proxy.proxy));
  }
  api::object_ptr<api::addedProxies> addedProxies() {
    auto result = api::make_object<api::addedProxies>();
    for (const auto &proxy : _proxies) result->proxies_.push_back(addedProxy(proxy));
    return result;
  }
  // As TDLib does when the way to the servers changes: connecting for a moment, then connected.
  void useProxy(std::int32_t id) {
    for (auto &proxy : _proxies) {
      proxy.enabled = proxy.id == id;
      if (proxy.enabled) proxy.lastUsed = kNow;
    }
    update(api::make_object<api::updateConnectionState>(
        id ? api::object_ptr<api::ConnectionState>(api::make_object<api::connectionStateConnectingToProxy>())
           : api::object_ptr<api::ConnectionState>(api::make_object<api::connectionStateConnecting>())));
    later(1500, [this] {
      update(api::make_object<api::updateConnectionState>(api::make_object<api::connectionStateReady>()));
    });
  }
  // tg://socks?server=S&port=P, as TDLib reads it.
  ObjectPtr proxyLink(const std::string &link) {
    const auto value = [&](const std::string &key) {
      const auto at = link.find(key + "=");
      if (at == std::string::npos) return std::string();
      const auto from = at + key.size() + 1;
      return link.substr(from, link.find('&', from) - from);
    };
    return api::make_object<api::internalLinkTypeProxy>(api::make_object<api::proxy>(
        value("server"), std::atoi(value("port").c_str()), api::make_object<api::proxyTypeSocks5>("", "")));
  }
  std::vector<Proxy> _proxies;
  std::int32_t _proxyIds = 0;

  static constexpr auto kNotifyChat = std::int64_t(5);
  static constexpr auto kNotifyGroup = std::int32_t(1);
  bool _unreadCountSent = false;
  bool _notificationsOn = false;
  int _notified = 0;
  std::vector<std::int32_t> _notes;  // The notifications of Friend 5 not read yet.

  const bool _fullList;
  Sink _sink;
  guint _source = 0;
  guint _ticker = 0;
  std::map<std::int64_t, int> _sentExtra;
  std::map<std::int64_t, std::pair<std::int64_t, bool>> _orders;  // Chat: its order when not pinned, and whether archived.
  int _loaded = 0;
  int _pins = 0;
  int _ticks = 0;
  bool _archiveLoaded = false;
  bool _publicAdded = false;
  std::vector<Topic> _topics;
  std::map<std::int64_t, std::string> _sentTexts;  // What was written in the topics, by message id.
  std::map<std::pair<std::int64_t, std::int64_t>, Entry> _entries;  // By chat and message id.
  std::map<std::int64_t, std::set<std::int64_t>> _pinned = {
      {1, {30 * kIdStep, kPinnedPhoto * kIdStep, 100 * kIdStep}},
      {2, {50 * kIdStep}},
      {kForumId, {(10 * 10 + 3) * kIdStep}},
  };
  bool _selfAdded = false;
  std::map<std::pair<std::int64_t, std::int64_t>, Reactions> _reactions;  // By chat and message id, once asked.
  bool _reactionRead = false;
  bool _customDownloaded = false;
  struct Download {
    std::int64_t downloaded = 0;
    bool active = false;
  };
  std::map<std::int32_t, Download> _downloads;  // The showcase's files asked for.
  guint _downloadTicker = 0;
  std::map<std::pair<std::int64_t, std::int64_t>, std::string> _failed;  // Not sent, by chat and message id.
  int _failures = 0;
  std::set<Later *> _later;
  guint _burst = 0;
  unsigned _burstSeed = 1;
  int _burstUpdates = 0;
};

}  // namespace

std::unique_ptr<Transport> MakeScenarioTransport() {
  return std::make_unique<ScenarioTransport>();
}

}  // namespace Contrail::Td
