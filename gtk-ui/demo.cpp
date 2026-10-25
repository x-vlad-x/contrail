#include "demo.hpp"

#include "app.hpp"
#include "message.hpp"

#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/components/sponsored_messages.h"
#include "data/data_changes.h"
#include "data/data_location.h"
#include "data/data_peer_id.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_account.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <cmath>
#include <random>

namespace Contrail {
namespace {

constexpr auto kSelf = UserId(7000001);
constexpr auto kMaya = UserId(7000002);
constexpr auto kSam = UserId(7000003);
constexpr auto kLena = UserId(7000004);
constexpr auto kJordan = UserId(7000005);
constexpr auto kPriya = UserId(7000006);
constexpr auto kGroup = ChatId(7100001);
constexpr auto kChannel = ChannelId(7200001);

// --- pictures ----------------------------------------------------------------

void Sky(QPainter &p, int w, int h, QColor top, QColor bottom) {
  auto gradient = QLinearGradient(0, 0, 0, h);
  gradient.setColorAt(0, top);
  gradient.setColorAt(1, bottom);
  p.fillRect(0, 0, w, h, gradient);
}

// A mountain ridge: a few sines of decreasing period, filled down to `bottom`.
QPainterPath Ridge(std::mt19937 &random, int w, double base, double amplitude, double roughness, int bottom) {
  auto phase = std::uniform_real_distribution<double>(0, 6.28);
  const double a = phase(random), b = phase(random), c = phase(random);
  auto path = QPainterPath(QPointF(0, bottom));
  for (auto x = 0; x <= w; x += 6) {
    const auto t = double(x) / w;
    path.lineTo(x, base - amplitude * (0.55 * std::sin(t * 3.1 + a) + 0.3 * std::sin(t * 7.3 + b)
                                       + roughness * std::sin(t * 19 + c)));
  }
  path.lineTo(w, bottom);
  path.closeSubpath();
  return path;
}

void Pines(QPainter &p, std::mt19937 &random, int w, int baseline, int minHeight, int maxHeight, int step,
           QColor color) {
  auto height = std::uniform_int_distribution<int>(minHeight, maxHeight);
  for (auto x = -step / 2; x < w; x += step) {
    const auto h = height(random);
    p.setBrush(color);
    p.drawPolygon(QPolygonF({QPointF(x, baseline), QPointF(x + step / 2., baseline - h), QPointF(x + step, baseline)}));
  }
}

QImage Lake() {
  constexpr auto w = 1280, h = 960, horizon = 560;
  auto random = std::mt19937(1);
  auto image = QImage(w, h, QImage::Format_RGB32);
  auto p = QPainter(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  Sky(p, w, horizon, QColor(64, 120, 196), QColor(250, 205, 160));
  p.setBrush(QColor(255, 236, 190));
  p.drawEllipse(QPointF(935, 305), 55, 55);
  const QColor ridges[] = {QColor(120, 130, 170), QColor(80, 100, 130), QColor(40, 70, 80)};
  const double bases[] = {380, 450, 520}, amplitudes[] = {140, 90, 50};
  for (auto i = 0; i != 3; ++i) {
    p.setBrush(ridges[i]);
    p.drawPath(Ridge(random, w, bases[i], amplitudes[i], 0.2, horizon));
  }
  // The water mirrors the mountains, darker.
  const auto upper = image.copy(0, horizon - (h - horizon), w, h - horizon).mirrored(false, true);
  p.drawImage(0, horizon, upper);
  p.fillRect(0, horizon, w, h - horizon, QColor(30, 60, 90, 120));
  auto lines = std::uniform_int_distribution<int>(0, w);
  auto rows = std::uniform_int_distribution<int>(horizon + 8, h - 8);
  p.setPen(QPen(QColor(210, 222, 235, 140), 2));
  for (auto i = 0; i != 70; ++i) {
    const auto x = lines(random), y = rows(random);
    p.drawLine(x, y, x + 40 + x % 150, y);
  }
  p.setPen(Qt::NoPen);
  Pines(p, random, w, horizon + 4, 60, 150, 34, QColor(18, 45, 35));
  return image;
}

QImage Forest() {
  constexpr auto w = 1280, h = 960;
  auto random = std::mt19937(2);
  auto image = QImage(w, h, QImage::Format_RGB32);
  auto p = QPainter(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  Sky(p, w, h, QColor(170, 205, 225), QColor(235, 240, 225));
  Pines(p, random, w, 640, 160, 280, 44, QColor(120, 150, 130));
  Pines(p, random, w, 720, 260, 400, 58, QColor(80, 115, 90));
  Pines(p, random, w, 820, 330, 520, 76, QColor(40, 75, 55));
  p.fillRect(0, 800, w, h - 800, QColor(55, 95, 60));
  p.setBrush(QColor(185, 155, 110));
  p.drawPolygon(QPolygonF({QPointF(500, h), QPointF(820, h), QPointF(668, 700), QPointF(632, 700)}));
  return image;
}

QImage Sunrise() {
  constexpr auto w = 1280, h = 960, horizon = 600;
  auto random = std::mt19937(3);
  auto image = QImage(w, h, QImage::Format_RGB32);
  auto p = QPainter(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  Sky(p, w, horizon, QColor(40, 50, 110), QColor(255, 150, 90));
  p.setBrush(QColor(255, 220, 140));
  p.drawEllipse(QPointF(640, 580), 80, 80);
  p.fillRect(0, horizon, w, h - horizon, QColor(40, 60, 100));
  auto spread = std::normal_distribution<double>(0, 1);
  auto rows = std::uniform_int_distribution<int>(horizon + 4, h);
  p.setPen(QPen(QColor(255, 190, 120), 2));
  for (auto i = 0; i != 160; ++i) {
    const auto y = rows(random);
    const auto width = (20 + i % 140) * (1 + (y - horizon) / 300.);
    const auto x = 640 + spread(random) * (60 + (y - horizon) * 0.4) - width / 2;
    p.drawLine(QPointF(x, y), QPointF(x + width, y));
  }
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(30, 35, 70));
  p.drawPath(Ridge(random, w, 590, 40, 0.3, horizon));
  return image;
}

// --- core data -----------------------------------------------------------------

// The pictures live in the core's caches only while somebody holds their media views.
std::vector<std::shared_ptr<Data::PhotoMedia>> &KeptMedia() {
  static auto result = std::vector<std::shared_ptr<Data::PhotoMedia>>();
  return result;
}

MTPUser User(UserId id, const QString &first, const QString &last, MTPUserStatus status, bool self = false) {
  using Flag = MTPDuser::Flag;
  return MTP_user(
      MTP_flags(Flag::f_first_name | Flag::f_last_name | Flag::f_status | Flag::f_access_hash
                | (self ? (Flag::f_self | Flag::f_phone) : (Flag::f_contact | Flag::f_mutual_contact))),
      MTP_long(id.bare), MTP_long(1), MTP_string(first), MTP_string(last),
      MTPstring(),  // username
      MTP_string(self ? u"15555550123"_q : QString()), MTP_userProfilePhotoEmpty(), status,
      MTPint(),  // bot_info_version
      MTPVector<MTPRestrictionReason>(),
      MTPstring(),  // bot_inline_placeholder
      MTPstring(),  // lang_code
      MTPEmojiStatus(), MTPVector<MTPUsername>(), MTPRecentStory(),
      MTPPeerColor(),  // color
      MTPPeerColor(),  // profile_color
      MTPint(),        // bot_active_users
      MTPlong(),       // bot_verification_icon
      MTPlong(),       // send_paid_messages_stars
      MTPlong());      // linked_community_id
}

MTPPhoto Photo(PhotoId id, const QImage &image, TimeId date) {
  return MTP_photo(MTP_flags(0), MTP_long(id), MTP_long(1), MTP_bytes(QByteArray()), MTP_int(date),
                   MTP_vector<MTPPhotoSize>(QVector<MTPPhotoSize>{MTP_photoSize(
                       MTP_string("y"), MTP_int(image.width()), MTP_int(image.height()), MTP_int(0))}),
                   MTPVector<MTPVideoSize>(), MTP_int(2));
}

// A photo with its picture already "downloaded".
MTPMessageMedia PhotoMedia(not_null<Main::Session *> session, PhotoId id, const QImage &image, TimeId date) {
  const auto photo = session->data().processPhoto(Photo(id, image, date));
  auto media = photo->createMediaView();
  auto bytes = QByteArray();
  auto buffer = QBuffer(&bytes);
  image.save(&buffer, "JPG", 90);
  media->set(Data::PhotoSize::Large, Data::PhotoSize::Large, image, bytes);
  KeptMedia().push_back(std::move(media));
  return MTP_messageMediaPhoto(MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo), Photo(id, image, date), MTPint(),
                               MTPDocument());
}

MTPMessageMedia VoiceMedia(DocumentId id, int seconds, TimeId date) {
  using Flag = MTPDdocumentAttributeAudio::Flag;
  const auto document = MTP_document(
      MTP_flags(0), MTP_long(id), MTP_long(1), MTP_bytes(QByteArray()), MTP_int(date), MTP_string("audio/ogg"),
      MTP_long(seconds * 4000), MTPVector<MTPPhotoSize>(), MTPVector<MTPVideoSize>(), MTP_int(2),
      MTP_vector<MTPDocumentAttribute>(QVector<MTPDocumentAttribute>{
          MTP_documentAttributeAudio(MTP_flags(Flag::f_voice), MTP_int(seconds), MTPstring(), MTPstring(), MTPbytes())}));
  using MediaFlag = MTPDmessageMediaDocument::Flag;
  return MTP_messageMediaDocument(MTP_flags(MediaFlag::f_document | MediaFlag::f_voice), document,
                                  MTPVector<MTPDocument>(), MTPPhoto(), MTPint(), MTPint());
}

// The reactions to a message: emoji, how many, whether one of them is the user's; and who, in small chats.
struct Reaction {
  QString emoji;
  int count = 0;
  bool mine = false;
};
MTPMessageReactions Reactions(const std::vector<Reaction> &list, const std::vector<std::pair<PeerId, QString>> &recent = {}) {
  auto results = QVector<MTPReactionCount>();
  auto order = 0;
  for (const auto &reaction : list) {
    using Flag = MTPDreactionCount::Flag;
    results.push_back(MTP_reactionCount(MTP_flags(reaction.mine ? Flag::f_chosen_order : Flag(0)), MTP_int(++order),
                                        MTP_reactionEmoji(MTP_string(reaction.emoji)), MTP_int(reaction.count)));
  }
  auto peers = QVector<MTPMessagePeerReaction>();
  for (const auto &[peer, emoji] : recent) {
    peers.push_back(MTP_messagePeerReaction(MTP_flags(0), peerToMTP(peer), MTP_int(0), MTP_reactionEmoji(MTP_string(emoji))));
  }
  using Flag = MTPDmessageReactions::Flag;
  return MTP_messageReactions(MTP_flags(recent.empty() ? Flag(0) : (Flag::f_recent_reactions | Flag::f_can_see_list)),
                              MTP_vector<MTPReactionCount>(results), MTP_vector<MTPMessagePeerReaction>(peers),
                              MTPVector<MTPMessageReactor>());
}

MTPGeoPoint Geo(double lat, double lon) {
  return MTP_geoPoint(MTP_flags(0), MTP_double(lon), MTP_double(lat), MTP_long(1), MTPint());
}

// The map Telegram's servers would draw for a location in a message, put together from OpenStreetMap's tiles in
// libshumate's cache (tools/demo.sh tiles puts them there): the demo has no network. Without tiles the map stays
// empty, as offline.
void CacheLocationMap(not_null<Main::Session *> session, double lat, double lon) {
  const auto geo = LocationMapImage(Data::LocationPoint(lat, lon, Data::LocationPoint::NoAccessHash));
  const auto width = geo.width * geo.scale, height = geo.height * geo.scale;
  const auto zoom = geo.zoom + int(std::lround(std::log2(geo.scale)));  // Twice the size: one level closer.
  constexpr auto kTile = 256;
  const auto world = double(kTile << zoom);
  const auto latRad = lat * M_PI / 180.;
  const auto left = int(std::lround((lon + 180.) / 360. * world)) - width / 2;
  const auto top = int(std::lround((1. - std::asinh(std::tan(latRad)) / M_PI) / 2. * world)) - height / 2;
  const auto tiles = QString::fromUtf8(g_get_user_cache_dir()) + u"/shumate/https___tile_openstreetmap_org__z___x___y__png/"_q;
  auto image = QImage(width, height, QImage::Format_RGB32);
  image.fill(QColor(242, 239, 233));
  auto p = QPainter(&image);
  auto found = false;
  for (auto x = left / kTile; x * kTile < left + width; ++x) {
    for (auto y = top / kTile; y * kTile < top + height; ++y) {
      const auto tile = QImage(tiles + u"%1/%2/%3.png"_q.arg(zoom).arg(x).arg(y));
      if (tile.isNull()) continue;
      p.drawImage(x * kTile - left, y * kTile - top, tile);
      found = true;
    }
  }
  p.end();
  if (!found) return;
  auto bytes = QByteArray();
  auto buffer = QBuffer(&bytes);
  image.save(&buffer, "PNG");
  session->data().cache().put(Data::GeoPointCacheKey(geo),
                              Storage::Cache::Database::TaggedValue(std::move(bytes), Data::kImageCacheTag));
}

struct Message {
  MsgId id;
  PeerId from;  // Empty for channel posts.
  TimeId date;
  QString text;
  std::optional<MTPMessageMedia> media;
  MsgId replyTo = 0;
  int views = 0;
  std::optional<MTPMessageReactions> reactions;
  TimeId edited = 0;
  bool pinned = false;
};

MTPMessage ToMTP(not_null<Main::Session *> session, PeerId peer, const Message &m) {
  using Flag = MTPDmessage::Flag;
  auto entities = TextWithEntities{m.text};
  TextUtilities::ParseEntities(entities, TextParseLinks);
  auto flags = MTPDmessage::Flags(0);
  if (m.from == session->userPeerId()) flags |= Flag::f_out;
  if (m.from) flags |= Flag::f_from_id;
  if (m.media) flags |= Flag::f_media;
  if (m.replyTo) flags |= Flag::f_reply_to;
  if (!entities.entities.empty()) flags |= Flag::f_entities;
  if (m.views) flags |= Flag::f_views | Flag::f_post;
  if (m.reactions) flags |= Flag::f_reactions;
  if (m.edited) flags |= Flag::f_edit_date | Flag::f_edit_hide;
  if (m.pinned) flags |= Flag::f_pinned;
  return MTP_message(
      MTP_flags(flags), MTP_int(m.id.bare), m.from ? peerToMTP(m.from) : MTPPeer(),
      MTPint(),     // from_boosts_applied
      MTPstring(),  // from_rank
      peerToMTP(peer),
      MTPPeer(),  // saved_peer_id
      MTPMessageFwdHeader(),
      MTPlong(),  // via_bot_id
      MTPlong(),  // via_business_bot_id
      MTPPeer(),  // guestchat_via_from
      m.replyTo ? MTP_messageReplyHeader(MTP_flags(MTPDmessageReplyHeader::Flag::f_reply_to_msg_id),
                                         MTP_int(m.replyTo.bare), MTPPeer(), MTPMessageFwdHeader(), MTPMessageMedia(),
                                         MTPint(), MTPstring(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(),
                                         MTPbytes(), MTPVector<MTPRichText>())
                : MTPMessageReplyHeader(),
      MTP_int(m.date), MTP_string(entities.text), m.media.value_or(MTP_messageMediaEmpty()), MTPReplyMarkup(),
      Api::EntitiesToMTP(session, entities.entities),
      m.views ? MTP_int(m.views) : MTPint(),  // views
      MTPint(),                               // forwards
      MTPMessageReplies(),
      m.edited ? MTP_int(m.edited) : MTPint(),  // edit_date
      MTPstring(), MTPlong(), m.reactions.value_or(MTPMessageReactions()), MTPVector<MTPRestrictionReason>(),
      MTPint(),   // ttl_period
      MTPint(),   // quick_reply_shortcut_id
      MTPlong(),  // effect
      MTPFactCheck(),
      MTPint(),   // report_delivery_until_date
      MTPlong(),  // paid_message_stars
      MTPSuggestedPost(),
      MTPint(),     // schedule_repeat_period
      MTPstring(),  // summary_from_language
      MTPRichMessage());
}

// A chat as if its whole history and its dialog entry had come from the server.
void AddChat(not_null<Main::Session *> session, PeerId peer, const std::vector<Message> &messages, int unread,
             bool pinned = false, bool muted = false) {
  auto slice = QVector<MTPMessage>();
  for (auto i = messages.rbegin(); i != messages.rend(); ++i) {
    slice.push_back(ToMTP(session, peer, *i));  // Newest first, as messages.getHistory returns them.
  }
  const auto history = session->data().history(peer);
  history->addOlderSlice(slice);
  history->addOlderSlice({});
  history->addNewerSlice({});
  const auto top = messages.back().id;
  const auto readInbox = MsgId(top.bare - unread);
  using NotifyFlag = MTPDpeerNotifySettings::Flag;
  const auto dialog = MTP_dialog(
      MTP_flags(pinned ? MTPDdialog::Flag::f_pinned : MTPDdialog::Flag(0)), peerToMTP(peer), MTP_int(top.bare),
      MTP_int(readInbox.bare), MTP_int(top.bare), MTP_int(unread),
      MTP_int(0),  // unread_mentions_count
      MTP_int(0),  // unread_reactions_count
      MTP_int(0),  // unread_poll_votes_count
      MTP_peerNotifySettings(MTP_flags(NotifyFlag::f_mute_until), MTPBool(), MTPBool(),
                             MTP_int(muted ? std::numeric_limits<int>::max() : 0), MTPNotificationSound(),
                             MTPNotificationSound(), MTPNotificationSound(), MTPBool(), MTPBool(),
                             MTPNotificationSound(), MTPNotificationSound(), MTPNotificationSound()),
      MTPint(), MTPDraftMessage(), MTPint(), MTPint());
  dialog.match([&](const MTPDdialog &data) { history->applyDialog(nullptr, data); }, [](const auto &) {});
  // The chat list row was made with the slice, before the read state came.
  using Flag = Data::HistoryUpdate::Flag;
  session->changes().historyUpdated(history, Flag::OutboxRead | Flag::UnreadView);
  if (pinned) {
    session->data().setChatPinned(history, FilterId(), true);
  }
}

void Fill(not_null<Main::Session *> session) {
  auto &data = session->data();
  const auto now = base::unixtime::now();
  const auto minutes = [&](int ago) { return TimeId(now - ago * 60); };
  data.processUsers(MTP_vector<MTPUser>(QVector<MTPUser>{
      User(kMaya, u"Maya"_q, u"Chen"_q, MTP_userStatusOnline(MTP_int(now + 3600))),
      User(kSam, u"Sam"_q, u"Okafor"_q, MTP_userStatusOffline(MTP_int(minutes(14)))),
      User(kLena, u"Lena"_q, u"Novak"_q, MTP_userStatusRecently(MTP_flags(0))),
      User(kJordan, u"Jordan"_q, u"Blake"_q, MTP_userStatusOffline(MTP_int(minutes(95)))),
      User(kPriya, u"Priya"_q, u"Sharma"_q, MTP_userStatusRecently(MTP_flags(0))),
  }));
  data.processChats(MTP_vector<MTPChat>(QVector<MTPChat>{
      MTP_chat(MTP_flags(0), MTP_long(kGroup.bare), MTP_string("Trail Runners"), MTP_chatPhotoEmpty(), MTP_int(6),
               MTP_int(minutes(60 * 24 * 90)), MTP_int(1), MTPInputChannel(), MTPChatAdminRights(),
               MTPChatBannedRights()),
      MTP_channel(MTP_flags(MTPDchannel::Flag::f_broadcast | MTPDchannel::Flag::f_access_hash
                            | MTPDchannel::Flag::f_participants_count),
                  MTP_long(kChannel.bare), MTP_long(1), MTP_string("Daily Landscapes"), MTPstring(),
                  MTP_chatPhotoEmpty(), MTP_int(minutes(60 * 24 * 365)), MTPVector<MTPRestrictionReason>(),
                  MTPChatAdminRights(), MTPChatBannedRights(), MTPChatBannedRights(), MTP_int(12840),
                  MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(), MTPPeerColor(), MTPEmojiStatus(),
                  MTPint(), MTPint(), MTPlong(), MTPlong(), MTPlong(), MTPlong(), MTPint()),
  }));
  const auto self = session->userPeerId();
  const auto maya = peerFromUser(kMaya), sam = peerFromUser(kSam), lena = peerFromUser(kLena);
  const auto jordan = peerFromUser(kJordan), priya = peerFromUser(kPriya);
  const auto lake = Lake(), forest = Forest(), sunrise = Sunrise();
  constexpr auto kPark = std::pair(46.5072, 6.6262), kPriya = std::pair(46.5101, 6.6318), kJordan = std::pair(46.5088, 6.6405);
  for (const auto &[lat, lon] : {kPark, kPriya, kJordan}) CacheLocationMap(session, lat, lon);

  AddChat(session, maya, {
      {1, maya, minutes(60 * 15 + 20), u"Are we still on for the hike on Saturday? 🥾"_q},
      {2, self, minutes(60 * 15 + 18), u"Absolutely! The forecast says sunny all weekend ☀️"_q},
      {3, maya, minutes(95), u"This is the lake I was telling you about"_q, PhotoMedia(session, 8100001, lake, minutes(95)),
       0, 0, Reactions({{u"❤️"_q, 1, true}}, {{self, u"❤️"_q}})},
      {4, self, minutes(93), u"Wow, that looks amazing! How long is the trail?"_q, std::nullopt, 3},
      {5, maya, minutes(90), u"About 12 km there and back. Mostly easy, a bit steep near the top."_q},
      {6, self, minutes(61), QString(), VoiceMedia(8200001, 7, minutes(61))},
      {7, maya, minutes(40), u"Haha, deal. I'll bring the snacks 🍫"_q, std::nullopt, 0, 0,
       Reactions({{u"😁"_q, 1, true}}, {{self, u"😁"_q}})},
      {8, self, minutes(12), u"Perfect. Let's meet at the parking lot at 8:00. Here's the route: openstreetmap.org"_q,
       std::nullopt, 0, 0, Reactions({{u"👍"_q, 1, false}}, {{maya, u"👍"_q}}), 0, true},
      {9, maya, minutes(2), u"See you there! 👋"_q},
  }, 1, true);
  AddChat(session, peerFromChat(kGroup), {
      {1, sam, minutes(52), u"Morning everyone! Who's in for the Sunday run? 🏃"_q, std::nullopt, 0, 0,
       Reactions({{u"👍"_q, 3, true}, {u"🔥"_q, 1, false}}, {{lena, u"👍"_q}, {priya, u"👍"_q}, {self, u"👍"_q}, {jordan, u"🔥"_q}})},
      {2, lena, minutes(50), u"Count me in!"_q},
      {3, priya, minutes(49), u"Me too, but I might be ten minutes late"_q},
      {4, sam, minutes(46), u"No worries. River loop or forest trail?"_q},
      {5, self, minutes(31), u"Forest trail, it's so much prettier this time of year 🍂"_q},
      {6, jordan, minutes(29), u"+1 for the forest"_q, std::nullopt, 5},
      {7, lena, minutes(8), u"Here's the view from last week"_q, PhotoMedia(session, 8100002, forest, minutes(8)), 0, 0,
       Reactions({{u"❤️"_q, 4, false}, {u"😍"_q, 2, false}})},
      {8, sam, minutes(6), QString(), MTP_messageMediaVenue(Geo(kPark.first, kPark.second), MTP_string("Riverside Park"),
                                                             MTP_string("Quai d'Ouchy 1, Lausanne"), MTP_string("foursquare"),
                                                             MTP_string("demo"), MTP_string("parks_outdoors/park"))},
      {9, jordan, minutes(5), QString(), MTP_messageMediaGeoLive(MTP_flags(0), Geo(kJordan.first, kJordan.second), MTPint(),
                                                                 MTP_int(8 * 3600), MTPint()),
       0, 0, std::nullopt, minutes(2)},
      {10, priya, minutes(4), QString(), MTP_messageMediaGeoLive(MTP_flags(0), Geo(kPriya.first, kPriya.second), MTPint(),
                                                                 MTP_int(3600), MTPint()),
       0, 0, std::nullopt, minutes(1)},
      {11, lena, minutes(3), u"Almost there, grabbing coffee first ☕"_q, std::nullopt, 0, 0,
       Reactions({{u"😁"_q, 2, false}}, {{sam, u"😁"_q}, {priya, u"😁"_q}})},
  }, 3);
  AddChat(session, peerFromChannel(kChannel), {
      {1, PeerId(), minutes(60 * 20), u"Sunrise over the bay. Shot this morning, just before six ☀️"_q,
       PhotoMedia(session, 8100003, sunrise, minutes(60 * 20)), 0, 8412,
       Reactions({{u"❤️"_q, 312, false}, {u"🔥"_q, 128, true}, {u"👏"_q, 45, false}})},
      {2, PeerId(), minutes(33), u"A calm evening at the mountain lake. Where would you go next?"_q,
       PhotoMedia(session, 8100004, lake, minutes(33)), 0, 2147},
  }, 1, false, true);
  AddChat(session, sam, {
      {1, self, minutes(70), u"Here are the notes from today's meeting"_q},
      {2, sam, minutes(64), u"Thanks, got it 👍"_q},
  }, 0);
  AddChat(session, jordan, {
      {1, jordan, minutes(185), u"Can you send me the slides from the workshop?"_q},
      {2, self, minutes(180), u"Sure, I'll send them tonight"_q},
  }, 0);
  AddChat(session, lena, {
      {1, lena, minutes(60 * 26), u"Happy birthday!! 🎉🎂"_q},
      {2, self, minutes(60 * 25), u"Thank you, Lena! 😊"_q},
  }, 0);
  AddChat(session, priya, {
      {1, priya, minutes(60 * 50), u"The new photos look great, thanks for sharing"_q},
  }, 0);
  data.sendHistoryChangeNotifications();
}

bool demoActive = false;

}  // namespace

void AddDemoSponsored() {
  auto &account = Core::App().domain().active();
  if (!demoActive || !account.sessionExists()) return;
  auto &data = account.session().data();
  const auto history = data.history(peerFromChannel(kChannel));
  history->addSponsoredMessage(
      data.nextLocalMessageId(),
      Data::SponsoredFrom{.title = u"Trail Maps"_q, .link = u"https://t.me/example"_q, .buttonText = u"View channel"_q},
      {u"Offline maps for every hiking trail in the region, updated each week."_q});
  data.sendHistoryChangeNotifications();
}

bool DemoActive() {
  return demoActive;
}

void StartDemo() {
  demoActive = true;
  // Screenshots are taken with the screen off, where animations never finish.
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
  auto &account = Core::App().domain().active();
  if (!account.sessionExists()) {
    // Creating the session switches the window from the sign-in to the chats.
    account.createSession(User(kSelf, u"Alex"_q, u"Rivera"_q, MTP_userStatusOnline(MTP_int(0)), true),
                          std::make_unique<Main::SessionSettings>());
  }
  Fill(&account.session());
}

}  // namespace Contrail
