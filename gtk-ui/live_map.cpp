#include "live_map.hpp"

#include "chat_live_locations.hpp"
#include "live_location.hpp"
#include "location.hpp"
#include "location_ui.hpp"
#include "main_window.hpp"
#include "map_view.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_chat_participant_status.h"
#include "data/data_location.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"

#include <rpl/lifetime.h>

#include <algorithm>
#include <map>

namespace Contrail {
namespace {

constexpr auto kPollInterval = 30 * crl::time(1000);
constexpr auto kPersonZoom = 16.;
constexpr auto kFitMaxZoom = 16.;

QString YouText() {
  return tr::lng_from_you(tr::now);
}

not_null<PeerData *> Sender(not_null<HistoryItem *> item) {
  return item->displayFrom() ? not_null(item->displayFrom()) : item->from();
}

}  // namespace

QString LiveLocationNames(const std::vector<not_null<HistoryItem *>> &items) {
  auto names = QStringList();
  auto mine = false;
  for (const auto &item : items) {
    if (IsMyLiveLocation(item)) {
      mine = true;
    } else {
      names.push_back(Sender(item)->shortName());
    }
  }
  if (mine) names.push_back(Translated("you", "вы", "ви"));
  if (names.isEmpty()) return QString();
  if (names.size() == 1) return mine ? YouText() : names.front();
  const auto last = names.takeLast();
  return Translated("%1 and %2", "%1 и %2", "%1 і %2").arg(names.join(u", "_q), last);
}

#ifdef CONTRAIL_SHUMATE

namespace {

class LiveMap final : public base::has_weak_ptr {
public:
  LiveMap(not_null<MainWindow *> window, not_null<Data::Thread *> thread);
  ~LiveMap();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }

private:
  struct Person {
    GtkWidget *marker = nullptr;
    double lat = 0.;
    double lon = 0.;
  };

  void build();
  void refresh();
  void refreshRows();
  void showAll(bool animate);
  [[nodiscard]] std::vector<std::pair<double, double>> points() const;
  [[nodiscard]] std::optional<std::pair<double, double>> myPoint() const;
  void startWatch();
  void received(const GeoFix &fix);
  void shareOrStop();
  void startSharing(TimeId period);

  const not_null<MainWindow *> _window;
  const not_null<Data::Thread *> _thread;
  const not_null<PeerData *> _peer;
  std::unique_ptr<ChatLiveLocations> _locations;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_map = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_listScroll = nullptr;
  GtkWidget *_empty = nullptr;
  GtkWidget *_share = nullptr;
  GtkWidget *_mine = nullptr;
  GtkWidget *_position = nullptr;
  std::map<not_null<PeerData *>, Person> _people;
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _fix;
  TimeId _shareWhenLocated = 0;  // "Share" was chosen before the first fix came.
  bool _moved = false;           // By hand: the map stops showing everybody.
  bool _shown = false;           // The map has been put on the people once.
  base::Timer _poll;
  SignalConnections _signals;
  rpl::lifetime _lifetime;
};

LiveMap::LiveMap(not_null<MainWindow *> window, not_null<Data::Thread *> thread)
    : _window(window), _thread(thread), _peer(thread->peer()),
      _locations(std::make_unique<ChatLiveLocations>(thread->owningHistory())), _poll([=] {
        _locations->request();
        refreshRows();
      }) {
  build();
  _locations->changes() | rpl::on_next([=] { refresh(); }, _lifetime);
  Live().changes() | rpl::on_next([=] { refreshRows(); }, _lifetime);
  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [=](AdwDialog *) {
    _watch = nullptr;
    _poll.cancel();
  });
  _poll.callEach(kPollInterval);
  // A live location going to this chat from here takes the device's position anyway: show it right away.
  if (Live().sharingIn(_peer)) startWatch();
  if (const auto recent = Location().recent(60)) received(*recent);
  refresh();
}

LiveMap::~LiveMap() {
  _signals.clear();
  _watch = nullptr;
}

void LiveMap::build() {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 1200);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1200);
  adw_dialog_set_presentation_mode(ADW_DIALOG(_dialog), ADW_DIALOG_FLOATING);
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  const auto chat = _peer->isSelf() ? tr::lng_saved_messages(tr::now) : _peer->name();
  auto *title = adw_window_title_new(U8(tr::lng_live_location(tr::now)).c_str(), U8(chat).c_str());
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  _map = MakeMap();
  auto *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), _map);
  auto *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_valign(buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_end(buttons, 12);
  gtk_widget_set_margin_bottom(buttons, 64);  // Above the attribution (two lines on a phone).
  auto *everybody = MapButton("zoom-fit-best-symbolic", Translated("Show Everyone", "Показать всех", "Показати всіх"));
  _signals.clicked(everybody, [=] {
    _moved = false;
    showAll(true);
  });
  gtk_box_append(GTK_BOX(buttons), everybody);
  _mine = MapButton("contrail-my-location-symbolic", MyLocationText());
  _signals.clicked(_mine, [=] {
    if (_fix) {
      _moved = true;
      MapCenterOn(_map, _fix->lat, _fix->lon, std::max(MapZoom(_map), kPersonZoom), true);
    }
    startWatch();
  });
  gtk_box_append(GTK_BOX(buttons), _mine);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buttons);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), MakeZoomButtons(_map));
  MapOnUserMove(_map, [=] { _moved = true; });
  MapCenterOn(_map, 30., 0., 2., false);

  // Who is where, and sharing one's own.
  auto *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(bottom, "location-details");
  _list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(_list, "boxed-list");
  _listScroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_listScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(_listScroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(_listScroll), 210);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_listScroll), _list);
  gtk_box_append(GTK_BOX(bottom), _listScroll);
  _empty = gtk_label_new(U8(Translated("Nobody shares a live location in this chat now.",
                                       "Сейчас в этом чате никто не транслирует геопозицию.",
                                       "Зараз у цьому чаті ніхто не транслює геопозицію."))
                             .c_str());
  gtk_widget_add_css_class(_empty, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(_empty), TRUE);
  gtk_box_append(GTK_BOX(bottom), _empty);
  _share = gtk_button_new();
  gtk_widget_add_css_class(_share, "pill");
  gtk_widget_set_halign(_share, GTK_ALIGN_CENTER);
  _signals.clicked(_share, [=] { shareOrStop(); });
  gtk_box_append(GTK_BOX(bottom), _share);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), bottom);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), overlay);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);

  Connect<void(GtkListBox *, GtkListBoxRow *)>(_list, "row-activated", crl::guard(this, [=](GtkListBox *, GtkListBoxRow *row) {
    const auto peer = static_cast<PeerData *>(g_object_get_data(G_OBJECT(row), "contrail-peer"));
    const auto i = peer ? _people.find(peer) : _people.end();
    if (i == _people.end()) return;
    _moved = true;
    MapCenterOn(_map, i->second.lat, i->second.lon, std::max(MapZoom(_map), kPersonZoom), true);
  }));
}

// The pins follow the chat's live locations.
void LiveMap::refresh() {
  auto seen = std::vector<not_null<PeerData *>>();
  auto changed = false;
  for (const auto &item : _locations->active()) {
    const auto location = LocationMedia(item);
    if (!location) continue;
    const auto peer = Sender(item);
    seen.push_back(peer);
    const auto lat = location->point().lat(), lon = location->point().lon();
    auto &person = _people[peer];
    if (!person.marker) {
      person.marker = MapAddMarker(_map, MakeMapPin(MakeAvatar(peer, 30)), lat, lon);
      changed = true;
    } else if (person.lat != lat || person.lon != lon) {
      MapMoveMarker(person.marker, lat, lon);
      changed = true;
    }
    person.lat = lat;
    person.lon = lon;
  }
  for (auto i = _people.begin(); i != _people.end();) {
    if (std::find(seen.begin(), seen.end(), i->first) == seen.end()) {
      MapRemoveMarker(_map, i->second.marker);
      i = _people.erase(i);
      changed = true;
    } else {
      ++i;
    }
  }
  if (changed || !_shown) showAll(_shown);
  refreshRows();
}

std::optional<std::pair<double, double>> LiveMap::myPoint() const {
  if (_fix) return std::pair(_fix->lat, _fix->lon);
  if (const auto mine = _locations->mine()) {
    if (const auto location = LocationMedia(mine)) return std::pair(location->point().lat(), location->point().lon());
  }
  return std::nullopt;
}

std::vector<std::pair<double, double>> LiveMap::points() const {
  auto result = std::vector<std::pair<double, double>>();
  for (const auto &[peer, person] : _people) {
    result.emplace_back(person.lat, person.lon);
  }
  if (_fix) result.emplace_back(_fix->lat, _fix->lon);
  return result;
}

void LiveMap::showAll(bool animate) {
  if (_moved) return;
  const auto all = points();
  if (all.empty()) return;
  MapShowAll(_map, all, kFitMaxZoom, animate);
  _shown = true;
}

void LiveMap::refreshRows() {
  gtk_list_box_remove_all(GTK_LIST_BOX(_list));
  const auto me = myPoint();
  for (const auto &item : _locations->active()) {
    const auto location = LocationMedia(item);
    if (!location) continue;
    const auto peer = Sender(item);
    auto *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Escape(IsMyLiveLocation(item) ? YouText() : peer->name()).c_str());
    auto subtitle = LiveLocationUpdated(item);
    if (me && !IsMyLiveLocation(item)) {
      const auto meters = DistanceMeters(me->first, me->second, location->point().lat(), location->point().lon());
      subtitle += u" · "_q + Translated("%1 away", "%1 от вас", "%1 від вас").arg(FormatDistance(meters));
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Escape(subtitle).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), MakeAvatar(peer, 36));
    auto *remaining = gtk_label_new(U8(LiveLocationRemaining(item)).c_str());
    gtk_widget_add_css_class(remaining, "dim-label");
    gtk_widget_add_css_class(remaining, "numeric");
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remaining);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data(G_OBJECT(row), "contrail-peer", peer.get());
    gtk_list_box_append(GTK_LIST_BOX(_list), row);
  }
  const auto empty = _locations->active().empty();
  gtk_widget_set_visible(_listScroll, !empty);
  gtk_widget_set_visible(_empty, empty);

  const auto sharing = bool(Live().sharingIn(_peer));
  const auto canShare = CanShareLive(_peer) && Data::CanSendTexts(_thread);
  gtk_widget_set_visible(_share, sharing || canShare);
  gtk_button_set_label(GTK_BUTTON(_share),
                       U8(sharing ? Translated("Stop Sharing My Location", "Остановить трансляцию", "Зупинити трансляцію")
                          : _shareWhenLocated ? Translated("Locating…", "Определение местоположения…",
                                                           "Визначення місцезнаходження…")
                                              : Translated("Share My Live Location", "Транслировать геопозицию",
                                                           "Транслювати геопозицію"))
                           .c_str());
  if (sharing) {
    gtk_widget_remove_css_class(_share, "suggested-action");
    gtk_widget_add_css_class(_share, "destructive-action");
  } else {
    gtk_widget_remove_css_class(_share, "destructive-action");
    gtk_widget_add_css_class(_share, "suggested-action");
  }
  gtk_widget_set_sensitive(_share, !_shareWhenLocated);
}

// The device's position is taken only while the map is open.
void LiveMap::startWatch() {
  if (_watch) return;
  gtk_widget_add_css_class(_mine, "active");
  const auto weak = base::make_weak(this);
  _watch = Location().watch(
      [=](const GeoFix &fix) {
        if (const auto strong = weak.get()) strong->received(fix);
      },
      [=](LocationService::Failure failure) {
        const auto strong = weak.get();
        if (!strong) return;
        ShowToast(FailureText(failure));
        gtk_widget_remove_css_class(strong->_mine, "active");
        strong->_shareWhenLocated = 0;
        strong->refreshRows();
        crl::on_main(strong, [=] { _watch = nullptr; });
      });
}

void LiveMap::received(const GeoFix &fix) {
  const auto first = !_fix;
  _fix = fix;
  if (!_position) {
    _position = MapAddMarker(_map, MakePositionDot(), fix.lat, fix.lon);
  } else {
    MapMoveMarker(_position, fix.lat, fix.lon);
  }
  if (first) showAll(_shown);
  if (const auto period = std::exchange(_shareWhenLocated, 0)) startSharing(period);
  refreshRows();
}

void LiveMap::shareOrStop() {
  if (const auto sharing = Live().sharingIn(_peer)) {
    Live().stop(sharing);
    return;
  }
  const auto weak = base::make_weak(this);
  ChooseLivePeriod(_dialog, [=](TimeId period) {
    const auto strong = weak.get();
    if (!strong) return;
    if (strong->_fix) {
      strong->startSharing(period);
    } else {
      // Shared as soon as the device knows where it is.
      strong->_shareWhenLocated = period;
      strong->startWatch();
      strong->refreshRows();
    }
  });
}

void LiveMap::startSharing(TimeId period) {
  if (!_fix || !Data::CanSendTexts(_thread) || Live().sharingIn(_peer)) return;
  Live().start(_thread, period, *_fix, FullMsgId());
  // The device's position stays on the map while this chat sees it.
  startWatch();
}

}  // namespace

void ShowLiveMap(not_null<MainWindow *> window, not_null<Data::Thread *> thread) {
  if (!window->widget()) return;
  auto map = std::make_unique<LiveMap>(window, thread);
  auto *dialog = map->dialog();
  AttachData(dialog, "contrail-live-map", std::move(map));
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
}

#else  // CONTRAIL_SHUMATE

// Without maps the newest live location opens in the system's map application.
void ShowLiveMap(not_null<MainWindow *> window, not_null<Data::Thread *> thread) {
  ChatLiveLocations locations(thread->owningHistory());
  if (locations.active().empty()) return;
  ShowLocation(window, locations.active().front());
}

#endif  // CONTRAIL_SHUMATE

}  // namespace Contrail
