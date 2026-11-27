#include "live_map.hpp"

#include "chat_live_locations.hpp"
#include "chat_view.hpp"
#include "geo.hpp"
#include "live_location.hpp"
#include "location.hpp"
#include "location_ui.hpp"
#include "main_window.hpp"
#include "map_view.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Contrail {
namespace api = Td::api;
namespace {

constexpr auto kPollInterval = 30u;  // Seconds.
constexpr auto kPersonZoom = 16.;
constexpr auto kFitMaxZoom = 16.;

class LiveMap final {
 public:
  LiveMap(MainWindow &window, Td::ChatId chat);
  ~LiveMap();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }
  void debug(const std::string &name, const std::string &rest);

 private:
  struct Person {
    GtkWidget *marker = nullptr;
    double lat = 0.;
    double lon = 0.;
  };

  void build();
  void refresh();
  void refreshRows();
  void startWatch();
  void received(const GeoFix &fix);
  void shareOrStop();
  void startSharing(std::int32_t period);
  void showAll(bool animate);
  [[nodiscard]] std::optional<std::pair<double, double>> myPoint() const;
  [[nodiscard]] std::vector<std::pair<double, double>> points() const;

  MainWindow &_window;
  Td::Store &_store;
  const Td::ChatId _chat;
  std::unique_ptr<ChatLiveLocations> _locations;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_map = nullptr;
  GtkWidget *_mine = nullptr;
  GtkWidget *_position = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_empty = nullptr;
  GtkWidget *_share = nullptr;
  std::map<std::int64_t, Person> _people;  // By message.
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _fix;
  std::int32_t _shareWhenLocated = 0;
  bool _moved = false;  // By hand: the map doesn't show everybody by itself then.
  bool _shown = false;
  guint _poll = 0;
  Td::Lifetime _lifetime;
  SignalConnections _signals;
  const std::shared_ptr<char> _alive = std::make_shared<char>();
};

LiveMap::LiveMap(MainWindow &window, Td::ChatId chat)
    : _window(window), _store(*window.store()), _chat(chat),
      _locations(std::make_unique<ChatLiveLocations>(_store, chat)) {
  build();
  _locations->onChanged([this] { refresh(); });
  Live().onChanged([this] { refreshRows(); }, _lifetime);
  _poll = g_timeout_add_seconds(kPollInterval, +[](gpointer data) -> gboolean {
    static_cast<LiveMap *>(data)->_locations->request();
    return G_SOURCE_CONTINUE;
  }, this);
  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) {
    _watch = nullptr;
    if (_poll) g_source_remove(std::exchange(_poll, 0));
  });
  _fix = Location().recent(60);
  // The device's position goes to this chat already: it is wanted on the map too.
  if (Live().sharingIn(_chat)) startWatch();
  refresh();
  g_message("Live map: shown for %lld", (long long)chat);
}

LiveMap::~LiveMap() {
  _signals.clear();
  _watch = nullptr;
  if (_poll) g_source_remove(_poll);
}

void LiveMap::build() {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 1200);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1200);
  adw_dialog_set_presentation_mode(ADW_DIALOG(_dialog), ADW_DIALOG_FLOATING);
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  const auto known = _store.chat(_chat);
  const auto self = known && known->kind == Td::ChatKind::Private && known->peerId == _store.selfId();
  auto *title = adw_window_title_new(Tr("lng_live_location").c_str(),
                                     self ? Tr("lng_saved_messages").c_str() : known ? known->title.c_str() : "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  auto *overlay = gtk_overlay_new();
#ifdef CONTRAIL_SHUMATE
  _map = MakeMap();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), _map);
  auto *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_valign(buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_end(buttons, 12);
  gtk_widget_set_margin_bottom(buttons, 64);  // Above the attribution (two lines on a phone).
  auto *everybody = MapButton("zoom-fit-best-symbolic", Translated("Show Everyone", "Показать всех", "Показати всіх"));
  _signals.clicked(everybody, [this] {
    _moved = false;
    showAll(true);
  });
  gtk_box_append(GTK_BOX(buttons), everybody);
  _mine = MapButton("contrail-my-location-symbolic", MyLocationText());
  _signals.clicked(_mine, [this] {
    if (_fix) {
      _moved = true;
      MapCenterOn(_map, _fix->lat, _fix->lon, std::max(MapZoom(_map), kPersonZoom), true);
    }
    startWatch();
  });
  gtk_box_append(GTK_BOX(buttons), _mine);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buttons);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), MakeZoomButtons(_map));
  const auto weak = std::weak_ptr<char>(_alive);
  MapOnUserMove(_map, [this, weak] {
    if (!weak.expired()) _moved = true;
  });
  MapCenterOn(_map, 30., 0., 2., false);
#endif

  // Who is where, and sharing one's own.
  auto *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(bottom, "location-details");
  _list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(_list, "boxed-list");
  _signals.connect<void(GtkListBox *, GtkListBoxRow *)>(_list, "row-activated", [this](GtkListBox *, GtkListBoxRow *row) {
    const auto it = _people.find(std::atoll(gtk_widget_get_name(GTK_WIDGET(row))));
    if (it == _people.end()) return;
#ifdef CONTRAIL_SHUMATE
    _moved = true;
    MapCenterOn(_map, it->second.lat, it->second.lon, std::max(MapZoom(_map), kPersonZoom), true);
#else
    OpenInMapsApp(it->second.lat, it->second.lon);
#endif
  });
  auto *listScroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(listScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(listScroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(listScroll), 210);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(listScroll), _list);
  gtk_box_append(GTK_BOX(bottom), listScroll);
  _empty = gtk_label_new(Translated("Nobody shares a live location in this chat now.",
                                    "Сейчас в этом чате никто не транслирует геопозицию.",
                                    "Зараз у цьому чаті ніхто не транслює геопозицію.")
                             .c_str());
  gtk_widget_add_css_class(_empty, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(_empty), TRUE);
  gtk_box_append(GTK_BOX(bottom), _empty);
  _share = gtk_button_new();
  gtk_widget_add_css_class(_share, "pill");
  gtk_widget_set_halign(_share, GTK_ALIGN_CENTER);
  _signals.clicked(_share, [this] { shareOrStop(); });
  gtk_widget_set_visible(_share, known && CanShareLive(_store, *known));
  gtk_box_append(GTK_BOX(bottom), _share);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), bottom);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), overlay);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);
}

void LiveMap::refresh() {
#ifdef CONTRAIL_SHUMATE
  // The pins follow the people: new ones come, moved ones move, those who stopped go.
  auto seen = std::vector<std::int64_t>();
  for (const auto *message : _locations->active()) {
    auto info = LocationInfo();
    if (!MessageLocation(*message, &info)) continue;
    seen.push_back(message->id_);
    auto &person = _people[message->id_];
    if (!person.marker) {
      person.marker = MapAddMarker(_map, MakeMapPin(MakeSenderAvatar(_store, *message, 30)), info.lat, info.lon);
    } else if (person.lat != info.lat || person.lon != info.lon) {
      MapMoveMarker(person.marker, info.lat, info.lon);
    }
    person.lat = info.lat;
    person.lon = info.lon;
  }
  for (auto it = _people.begin(); it != _people.end();) {
    if (std::ranges::find(seen, it->first) == seen.end()) {
      if (it->second.marker) MapRemoveMarker(_map, it->second.marker);
      it = _people.erase(it);
    } else {
      ++it;
    }
  }
#else
  _people.clear();
  for (const auto *message : _locations->active()) {
    auto info = LocationInfo();
    if (MessageLocation(*message, &info)) _people[message->id_] = {.lat = info.lat, .lon = info.lon};
  }
#endif
  if (!_shown || !_moved) showAll(_shown);
  refreshRows();
}

std::optional<std::pair<double, double>> LiveMap::myPoint() const {
  if (_fix) return std::pair(_fix->lat, _fix->lon);
  if (const auto mine = _locations->mine()) {
    auto info = LocationInfo();
    if (MessageLocation(*mine, &info)) return std::pair(info.lat, info.lon);
  }
  return std::nullopt;
}

std::vector<std::pair<double, double>> LiveMap::points() const {
  auto result = std::vector<std::pair<double, double>>();
  for (const auto &[id, person] : _people) result.emplace_back(person.lat, person.lon);
  if (_fix) result.emplace_back(_fix->lat, _fix->lon);
  return result;
}

void LiveMap::showAll(bool animate) {
#ifdef CONTRAIL_SHUMATE
  if (_moved) return;
  const auto all = points();
  if (all.empty()) return;
  MapShowAll(_map, all, kFitMaxZoom, animate);
  _shown = true;
#else
  (void)animate;
#endif
}

void LiveMap::refreshRows() {
  gtk_list_box_remove_all(GTK_LIST_BOX(_list));
  const auto me = myPoint();
  for (const auto *message : _locations->active()) {
    auto info = LocationInfo();
    if (!MessageLocation(*message, &info)) continue;
    const auto mine = _locations->isMine(*message);
    auto *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  Escape(mine ? Tr("lng_from_you") : SenderName(_store, *message)).c_str());
    auto subtitle = LiveLocationUpdated(*message);
    if (me && !mine) {
      auto away = Translated("{distance} away", "{distance} от вас", "{distance} від вас");
      away.replace(away.find("{distance}"), 10,
                   FormatDistance(DistanceMeters(me->first, me->second, info.lat, info.lon)));
      subtitle += " · " + away;
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Escape(subtitle).c_str());
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), MakeSenderAvatar(_store, *message, 36));
    auto *remaining = gtk_label_new(LiveLocationRemaining(*message).c_str());
    gtk_widget_add_css_class(remaining, "dim-label");
    gtk_widget_add_css_class(remaining, "numeric");
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remaining);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    gtk_widget_set_name(row, std::to_string(message->id_).c_str());
    gtk_list_box_append(GTK_LIST_BOX(_list), row);
  }
  const auto any = !_locations->active().empty();
  gtk_widget_set_visible(_list, any);
  gtk_widget_set_visible(_empty, !any);
  const auto sharing = Live().sharingIn(_chat) != 0;
  gtk_button_set_label(GTK_BUTTON(_share),
                       (sharing ? Translated("Stop Sharing My Location", "Остановить трансляцию", "Зупинити трансляцію")
                        : _shareWhenLocated
                            ? Translated("Locating…", "Определение местоположения…", "Визначення місцезнаходження…")
                            : Translated("Share My Live Location", "Транслировать геопозицию", "Транслювати геопозицію"))
                           .c_str());
  gtk_widget_remove_css_class(_share, sharing ? "suggested-action" : "destructive-action");
  gtk_widget_add_css_class(_share, sharing ? "destructive-action" : "suggested-action");
}

void LiveMap::startWatch() {
  if (_watch) return;
  if (_mine) gtk_widget_add_css_class(_mine, "active");
  const auto weak = std::weak_ptr<char>(_alive);
  _watch = Location().watch(
      [this, weak](const GeoFix &fix) {
        if (!weak.expired()) received(fix);
      },
      [this, weak](LocationService::Failure failure) {
        if (weak.expired()) return;
        ShowToast(FailureText(failure));
        if (_mine) gtk_widget_remove_css_class(_mine, "active");
        _shareWhenLocated = 0;
        struct Drop {
          LiveMap *self;
          std::weak_ptr<char> weak;
        };
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
          const auto drop = static_cast<Drop *>(data);
          if (!drop->weak.expired()) {
            drop->self->_watch = nullptr;
            drop->self->refreshRows();
          }
          return G_SOURCE_REMOVE;
        }, new Drop{this, weak}, +[](gpointer data) { delete static_cast<Drop *>(data); });
      });
}

void LiveMap::received(const GeoFix &fix) {
  const auto first = !_fix;
  _fix = fix;
#ifdef CONTRAIL_SHUMATE
  if (!_position) {
    _position = MapAddMarker(_map, MakePositionDot(), fix.lat, fix.lon);
  } else {
    MapMoveMarker(_position, fix.lat, fix.lon);
  }
#endif
  if (first) showAll(_shown);
  if (const auto period = std::exchange(_shareWhenLocated, 0)) startSharing(period);
  refreshRows();
}

void LiveMap::shareOrStop() {
  if (const auto sharing = Live().sharingIn(_chat)) {
    Live().stop(_chat, sharing);
    return;
  }
  const auto weak = std::weak_ptr<char>(_alive);
  ChooseLivePeriod(_dialog, [this, weak](std::int32_t period) {
    if (weak.expired()) return;
    if (_fix) {
      startSharing(period);
    } else {
      // Shared as soon as the device knows where it is.
      _shareWhenLocated = period;
      startWatch();
      refreshRows();
    }
  });
}

void LiveMap::startSharing(std::int32_t period) {
  const auto known = _store.chat(_chat);
  if (!_fix || !known || !_store.canWrite(*known) || Live().sharingIn(_chat)) return;
  Live().start(_chat, 0, 0, period, *_fix);
  // The device's position stays on the map while this chat sees it.
  startWatch();
}

// live-state | live-share (the button) | live-row N (a tap on person N)
void LiveMap::debug(const std::string &name, const std::string &rest) {
  if (name == "live-share") {
    g_signal_emit_by_name(_share, "clicked");
    return;
  } else if (name == "live-row") {
    if (auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), std::atoi(rest.c_str()))) {
      g_signal_emit_by_name(_list, "row-activated", row);
    }
    return;
  }
  auto rows = std::string();
  for (auto *row = gtk_widget_get_first_child(_list); row; row = gtk_widget_get_next_sibling(row)) {
    rows += std::string(" [\"") + adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)) + "\" \""
            + adw_action_row_get_subtitle(ADW_ACTION_ROW(row)) + "\"]";
  }
  g_message("Live map: pins=%zu share=\"%s\" watching=%d moved=%d rows:%s", _people.size(),
            gtk_button_get_label(GTK_BUTTON(_share)), int(_watch != nullptr), int(_moved), rows.c_str());
}

}  // namespace

void ShowLiveMap(MainWindow &window, Td::ChatId chat) {
  if (!window.widget() || !window.store() || !window.store()->chat(chat)) return;
  auto map = std::make_unique<LiveMap>(window, chat);
  auto *dialog = map->dialog();
  AttachData(dialog, "contrail-live-map", std::move(map));
  adw_dialog_present(ADW_DIALOG(dialog), window.widget());
}

bool LiveMapDebugCommand(MainWindow &window, const std::string &name, const std::string &rest) {
  if (name == "live-locations") {
    g_message("Live locations: %s", Live().debugState().c_str());
    return true;
  } else if (name == "live-update") {
    Live().debugUpdate();
    return true;
  } else if (name == "live-bar" || name == "live-bar-tap") {
    const auto bar = window.chatView() ? window.chatView()->liveBar() : nullptr;
    if (!bar) g_message("Live bar: none");
    else if (name == "live-bar-tap") bar->debugTap();
    else g_message("Live bar: %s", bar->debugState().c_str());
    return true;
  } else if (name == "live-map") {
    if (window.currentChat()) ShowLiveMap(window, window.currentChat());
    return true;
  } else if (!name.starts_with("live-") || !window.widget()) {
    return false;
  }
  auto *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window.widget()));
  const auto map = dialog ? static_cast<LiveMap *>(g_object_get_data(G_OBJECT(dialog), "contrail-live-map")) : nullptr;
  if (map) map->debug(name, rest);
  else g_message("Live map: none shown");
  return true;
}

}  // namespace Contrail
