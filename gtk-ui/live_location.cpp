#include "live_location.hpp"

#include "text.hpp"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <cmath>

namespace Contrail {
namespace {

constexpr auto kAcquireTimeout = 30 * crl::time(1000);
constexpr auto kGoodAccuracy = 30.;  // Meters: a fix this good ends the search for one.
constexpr auto kRetryInterval = 120;  // Seconds, after the location service failed.

QString StoragePath() {
  return cWorkingDir() + u"tdata/contrail-live-locations.json"_q;
}

MTPInputGeoPoint InputPoint(const GeoFix &fix) {
  using Flag = MTPDinputGeoPoint::Flag;
  const auto accuracy = int(std::lround(fix.accuracy));
  return MTP_inputGeoPoint(MTP_flags(accuracy > 0 ? Flag::f_accuracy_radius : Flag(0)), MTP_double(fix.lat),
                           MTP_double(fix.lon), MTP_int(accuracy));
}

int Heading(const GeoFix &fix) {
  // Telegram takes 1..360.
  const auto heading = int(std::lround(fix.heading)) % 360;
  return heading ? heading : 360;
}

// The live location's message id, from the server's answer to sending it.
MsgId SentMessageId(const MTPUpdates &updates, uint64 randomId) {
  auto result = MsgId();
  const auto search = [&](const MTPVector<MTPUpdate> &list) {
    for (const auto &update : list.v) {
      update.match([&](const MTPDupdateMessageID &data) {
        if (data.vrandom_id().v == randomId) result = data.vid().v;
      }, [](const auto &) {});
    }
  };
  updates.match([&](const MTPDupdates &data) { search(data.vupdates()); },
                [&](const MTPDupdatesCombined &data) { search(data.vupdates()); }, [](const auto &) {});
  return result;
}

}  // namespace

LiveLocations::LiveLocations() : _timer([=] { acquire(); }), _acquireTimeout([=] { finishAcquire(); }) {}

LiveLocations::~LiveLocations() = default;

void LiveLocations::setSession(Main::Session *session) {
  if (_session == session) return;
  _sessionLifetime.destroy();
  _watch = nullptr;
  _timer.cancel();
  _acquireTimeout.cancel();
  _shares.clear();
  _session = session;
  if (!session) {
    _changes.fire({});
    return;
  }
  load();
  auto &data = session->data();
  data.itemRemoved() | rpl::on_next([=](not_null<const HistoryItem *> item) {
    if (sharing(item->fullId())) forget(item->fullId());
  }, _sessionLifetime);
  // Stopped on another device: the server shortens the period to the time it went on.
  session->changes().messageUpdates(Data::MessageUpdate::Flag::Edited)
      | rpl::on_next([=](const Data::MessageUpdate &update) {
          const auto item = update.item;
          if (!sharing(item->fullId())) return;
          const auto location = dynamic_cast<Data::MediaLocation *>(item->media());
          const auto period = location ? location->livePeriod() : 0;
          if (!period || (period != kUntilOff && base::unixtime::now() >= item->date() + period)) {
            forget(item->fullId());
          }
        }, _sessionLifetime);
  prune();
  if (!_shares.empty()) schedule(1);
  _changes.fire({});
}

void LiveLocations::start(not_null<Data::Thread *> thread, TimeId period, const GeoFix &fix, FullMsgId replyTo) {
  const auto history = thread->owningHistory();
  const auto peer = history->peer;
  const auto session = &history->session();
  auto action = Api::SendAction(thread);
  if (replyTo) action.replyTo.messageId = replyTo;
  session->api().sendAction(action);

  const auto randomId = base::RandomValue<uint64>();
  using SendFlag = MTPmessages_SendMedia::Flag;
  auto flags = action.replyTo ? SendFlag::f_reply_to : SendFlag(0);
  using MediaFlag = MTPDinputMediaGeoLive::Flag;
  const auto mediaFlags = MediaFlag::f_period | (LiveUpdatePolicy::Moving(fix) ? MediaFlag::f_heading : MediaFlag(0));
  const auto media = MTP_inputMediaGeoLive(MTP_flags(mediaFlags), InputPoint(fix), MTP_int(Heading(fix)),
                                           MTP_int(period), MTPint());
  const auto weak = base::make_weak(this);
  const auto started = base::unixtime::now();
  auto &histories = history->owner().histories();
  histories.sendPreparedMessage(
      history, action.replyTo, randomId,
      Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
          MTP_flags(flags), peer->input(), Data::Histories::ReplyToPlaceholder(), media, MTPstring(),
          MTP_long(randomId), MTPReplyMarkup(), MTPvector<MTPMessageEntity>(), MTPint(), MTPint(), MTPInputPeer(),
          MTPInputQuickReplyShortcut(), MTPlong(), MTPlong(), MTPSuggestedPost()),
      [=](const MTPUpdates &result, const MTP::Response &) {
        const auto strong = weak.get();
        const auto id = SentMessageId(result, randomId);
        if (!strong || strong->_session != session || !id) return;
        strong->_shares.push_back({
            .id = FullMsgId(peer->id, id),
            .start = started,
            .period = period,
            .sent = {.lat = fix.lat, .lon = fix.lon, .heading = LiveUpdatePolicy::Moving(fix) ? fix.heading : -1., .time = g_get_real_time()},
        });
        strong->changed();
        if (!strong->_timer.isActive() && !strong->_watch) strong->schedule(LiveUpdatePolicy::kIntervalMoving);
      },
      [=](const MTP::Error &error, const MTP::Response &) { ShowToast(error.type()); });
}

void LiveLocations::stop(FullMsgId id) {
  if (!_session || !sharing(id)) return;
  forget(id);
  const auto peer = _session->data().peerLoaded(id.peer);
  if (!peer) return;
  using MediaFlag = MTPDinputMediaGeoLive::Flag;
  const auto session = _session;
  _session->api()
      .request(MTPmessages_EditMessage(
          MTP_flags(MTPmessages_EditMessage::Flag::f_media), peer->input(), MTP_int(id.msg), MTPstring(),
          MTP_inputMediaGeoLive(MTP_flags(MediaFlag::f_stopped), MTP_inputGeoPointEmpty(), MTPint(), MTPint(), MTPint()),
          MTPReplyMarkup(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPint(), MTPInputRichMessage()))
      .done([=](const MTPUpdates &result) { session->api().applyUpdates(result); })
      .send();
}

void LiveLocations::stopAll() {
  const auto shares = _shares;
  for (const auto &share : shares) stop(share.id);
}

FullMsgId LiveLocations::sharingIn(not_null<PeerData *> peer) const {
  const auto now = base::unixtime::now();
  for (const auto &share : _shares) {
    if (share.id.peer == peer->id && (share.period == kUntilOff || now < share.start + share.period)) return share.id;
  }
  return FullMsgId();
}

bool LiveLocations::sharing(FullMsgId id) const {
  return std::any_of(_shares.begin(), _shares.end(), [&](const Share &share) { return share.id == id; });
}

void LiveLocations::forget(FullMsgId id) {
  _shares.erase(std::remove_if(_shares.begin(), _shares.end(), [&](const Share &share) { return share.id == id; }),
                _shares.end());
  changed();
}

void LiveLocations::changed() {
  save();
  if (_shares.empty()) {
    _timer.cancel();
    _acquireTimeout.cancel();
    _watch = nullptr;
    _best = std::nullopt;
  }
  _changes.fire({});
}

// The server ends a live location by itself when its time is over: nothing to send then.
void LiveLocations::prune() {
  const auto now = base::unixtime::now();
  const auto before = _shares.size();
  _shares.erase(std::remove_if(_shares.begin(), _shares.end(), [&](const Share &share) {
    return share.period != kUntilOff && now >= share.start + share.period;
  }), _shares.end());
  if (_shares.size() != before) changed();
}

void LiveLocations::schedule(int seconds) {
  if (_shares.empty()) return;
  _timer.callOnce(seconds * crl::time(1000));
}

void LiveLocations::acquire() {
  prune();
  if (_shares.empty() || !_session || _watch) return;
  _best = std::nullopt;
  const auto weak = base::make_weak(this);
  _watch = Location().watch(
      [=](const GeoFix &fix) {
        const auto strong = weak.get();
        if (!strong || !strong->_watch) return;
        if (!strong->_best || fix.accuracy <= strong->_best->accuracy) strong->_best = fix;
        if (fix.accuracy > 0 && fix.accuracy <= kGoodAccuracy) {
          // Not from inside the service's callback: the watch ends here.
          crl::on_main(strong, [=] { finishAcquire(); });
        }
      },
      [=](LocationService::Failure failure) {
        const auto strong = weak.get();
        if (!strong) return;
        if (failure == LocationService::Failure::Denied && !strong->_deniedShown) {
          strong->_deniedShown = true;
          ShowToast(tr::lng_live_location(tr::now) + u": "_q
                    + Translated("no access to the location", "нет доступа к геопозиции", "немає доступу до геопозиції"));
        }
        crl::on_main(strong, [=] {
          _acquireTimeout.cancel();
          _watch = nullptr;
          schedule(kRetryInterval);
        });
      });
  _acquireTimeout.callOnce(kAcquireTimeout);
}

void LiveLocations::finishAcquire() {
  if (!_watch) return;
  _watch = nullptr;
  _acquireTimeout.cancel();
  const auto fix = std::exchange(_best, std::nullopt);
  if (!fix) {
    schedule(LiveUpdatePolicy::kInterval);
    return;
  }
  const auto now = g_get_real_time();
  auto moved = false;
  for (auto &share : _shares) {
    if (LiveUpdatePolicy::Moved(share.sent, *fix)) moved = true;
    if (LiveUpdatePolicy::ShouldSend(share.sent, *fix, now)) update(share, *fix);
  }
  _stillCycles = moved ? 0 : (_stillCycles + 1);
  save();
  schedule(LiveUpdatePolicy::NextInterval(*fix, moved, _stillCycles));
}

void LiveLocations::update(Share &share, const GeoFix &fix) {
  const auto peer = _session ? _session->data().peerLoaded(share.id.peer) : nullptr;
  if (!peer) return;
  share.sent = {.lat = fix.lat, .lon = fix.lon, .heading = LiveUpdatePolicy::Moving(fix) ? fix.heading : -1., .time = g_get_real_time()};
  using MediaFlag = MTPDinputMediaGeoLive::Flag;
  const auto id = share.id;
  const auto session = _session;
  const auto weak = base::make_weak(this);
  _session->api()
      .request(MTPmessages_EditMessage(
          MTP_flags(MTPmessages_EditMessage::Flag::f_media), peer->input(), MTP_int(id.msg), MTPstring(),
          MTP_inputMediaGeoLive(MTP_flags(LiveUpdatePolicy::Moving(fix) ? MediaFlag::f_heading : MediaFlag(0)), InputPoint(fix),
                                MTP_int(Heading(fix)), MTPint(), MTPint()),
          MTPReplyMarkup(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPint(), MTPInputRichMessage()))
      .done([=](const MTPUpdates &result) { session->api().applyUpdates(result); })
      .fail([=](const MTP::Error &error) {
        // Deleted, or its time is over: there is nothing to update anymore.
        if (error.type() != u"MESSAGE_NOT_MODIFIED"_q) {
          if (const auto strong = weak.get()) strong->forget(id);
        }
      })
      .send();
}

void LiveLocations::load() {
  auto file = QFile(StoragePath());
  if (!_session || !file.open(QIODevice::ReadOnly)) return;
  const auto root = QJsonDocument::fromJson(file.readAll()).object();
  if (root.value(u"user"_q).toString() != QString::number(_session->userId().bare)) return;
  for (const auto &value : root.value(u"shares"_q).toArray()) {
    const auto object = value.toObject();
    auto share = Share{
        .id = FullMsgId(PeerId(object.value(u"peer"_q).toString().toULongLong()), MsgId(object.value(u"msg"_q).toInteger())),
        .start = TimeId(object.value(u"start"_q).toInteger()),
        .period = TimeId(object.value(u"period"_q).toInteger()),
        .sent = {.lat = object.value(u"lat"_q).toDouble(), .lon = object.value(u"lon"_q).toDouble(), .heading = -1.},
    };
    if (share.id.peer && share.id.msg && share.period) _shares.push_back(share);
  }
}

void LiveLocations::save() const {
  if (!_session) return;
  auto shares = QJsonArray();
  for (const auto &share : _shares) {
    shares.append(QJsonObject{
        {u"peer"_q, QString::number(share.id.peer.value)},
        {u"msg"_q, qint64(share.id.msg.bare)},
        {u"start"_q, qint64(share.start)},
        {u"period"_q, qint64(share.period)},
        {u"lat"_q, share.sent.lat},
        {u"lon"_q, share.sent.lon},
    });
  }
  if (shares.isEmpty()) {
    QFile::remove(StoragePath());
    return;
  }
  QDir().mkpath(cWorkingDir() + u"tdata"_q);
  auto file = QFile(StoragePath());
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    const auto root = QJsonObject{{u"user"_q, QString::number(_session->userId().bare)}, {u"shares"_q, shares}};
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  }
}

QString LiveLocations::debugState() const {
  auto result = u"shares=%1 waiting=%2 timer=%3s still=%4"_q.arg(_shares.size())
                    .arg(_watch ? 1 : 0)
                    .arg(_timer.isActive() ? _timer.remainingTime() / 1000 : -1)
                    .arg(_stillCycles);
  for (const auto &share : _shares) {
    result += u" [%1/%2 period=%3 sent=%4,%5]"_q.arg(share.id.peer.value)
                  .arg(share.id.msg.bare)
                  .arg(share.period)
                  .arg(share.sent.lat, 0, 'f', 5)
                  .arg(share.sent.lon, 0, 'f', 5);
  }
  return result;
}

void LiveLocations::debugUpdate() {
  _timer.cancel();
  acquire();
}

LiveLocations &Live() {
  static auto instance = LiveLocations();
  return instance;
}

}  // namespace Contrail
