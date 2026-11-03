#include "location_ui.hpp"

#include "geo.hpp"
#include "location.hpp"
#include "main_window.hpp"
#include "map_view.hpp"
#include "td/dates.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Contrail {
namespace api = Td::api;
namespace {

constexpr auto kPlaceZoom = 16.;
constexpr auto kVenuesDelay = 700u;   // Milliseconds after the map stopped or the query changed.
constexpr auto kVenuesMinMove = 150.;  // Meters the map moves before the places nearby are asked again.
constexpr auto kSelectedAway = 30.;    // Meters from the device's position: the place under the pin.

template <typename Type>
[[nodiscard]] const Type *Cast(const api::Object *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

std::int32_t UpdateDate(const api::message &message) {
  return message.edit_date_ ? message.edit_date_ : message.date_;
}

GtkWidget *RowIcon(const char *icon, const char *cssClass) {
  auto *image = gtk_image_new_from_icon_name(icon);
  gtk_image_set_pixel_size(GTK_IMAGE(image), 20);
  gtk_widget_add_css_class(image, "location-row-icon");
  gtk_widget_add_css_class(image, cssClass);
  gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
  return image;
}

bool WindowActive(MainWindow &window) {
  return window.widget() && gtk_window_is_active(GTK_WINDOW(window.widget()));
}

// A main-loop timer that is cancelled with its owner.
class Timer final {
 public:
  explicit Timer(std::function<void()> callback) : _callback(std::move(callback)) {}
  ~Timer() { cancel(); }
  void callOnce(guint ms) {
    cancel();
    _source = g_timeout_add(ms, +[](gpointer data) -> gboolean {
      auto *self = static_cast<Timer *>(data);
      self->_source = 0;
      self->_callback();
      return G_SOURCE_REMOVE;
    }, this);
  }
  void cancel() {
    if (_source) g_source_remove(std::exchange(_source, 0));
  }

 private:
  std::function<void()> _callback;
  guint _source = 0;
};

struct Venue {
  double lat = 0.;
  double lon = 0.;
  std::string title;
  std::string address;
  std::string provider;
  std::string id;
  std::string type;
};

}  // namespace

GtkWidget *MapButton(const char *icon, const std::string &tooltip) {
  auto *button = MakeIconButton(icon, tooltip.c_str(), "circular");
  gtk_widget_add_css_class(button, "osd");
  gtk_widget_add_css_class(button, "map-button");
  return button;
}

#ifdef CONTRAIL_SHUMATE
GtkWidget *MakeZoomButtons(GtkWidget *map) {
  auto *buttons = MakeMapZoomButtons(map);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_valign(buttons, GTK_ALIGN_START);
  gtk_widget_set_margin_end(buttons, 12);
  gtk_widget_set_margin_top(buttons, 12);
  return buttons;
}
#endif  // CONTRAIL_SHUMATE

std::string MyLocationText() {
  return Translated("My Location", "Моё местоположение", "Моє місцезнаходження");
}

std::string FailureText(LocationService::Failure failure) {
  return (failure == LocationService::Failure::Denied)
             ? Translated("No access to the location", "Нет доступа к геопозиции", "Немає доступу до геопозиції")
             : Translated("Location is unavailable", "Геопозиция недоступна", "Геопозиція недоступна");
}

bool CanShareLive(const Td::Store &store, const Td::Chat &chat) {
  if (chat.kind == Td::ChatKind::Channel) return false;
  return !(chat.kind == Td::ChatKind::Private && chat.peerId == store.selfId());
}

void ChooseLivePeriod(GtkWidget *parent, std::function<void(std::int32_t)> chosen) {
  auto *dialog = adw_alert_dialog_new(
      Translated("Share your live location for…", "Транслировать геопозицию…", "Транслювати геопозицію…").c_str(),
      Translated("People in this chat will see where you are in real time.",
                 "Участники чата будут видеть, где вы находитесь, в реальном времени.",
                 "Учасники чату бачитимуть, де ви, у реальному часі.")
          .c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                 "15m", TrCount("lng_minutes", 15).c_str(),
                                 "1h", TrCount("lng_hours", 1).c_str(),
                                 "8h", TrCount("lng_hours", 8).c_str(),
                                 "off", Translated("Until I turn it off", "Пока не выключу", "Доки не вимкну").c_str(),
                                 "cancel", Tr("lng_cancel").c_str(), nullptr);
  adw_alert_dialog_set_prefer_wide_layout(ADW_ALERT_DIALOG(dialog), FALSE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    const auto id = std::string(response);
    const auto period = (id == "15m") ? 15 * 60 : (id == "1h") ? 3600 : (id == "8h") ? 8 * 3600
                        : (id == "off") ? kUntilOff : 0;
    if (period) chosen(period);
  });
  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

bool MessageLocation(const api::message &message, LocationInfo *info) {
  const auto content = message.content_.get();
  auto result = LocationInfo();
  if (const auto plain = Cast<api::messageLocation>(content)) {
    if (!plain->location_) return false;
    result.lat = plain->location_->latitude_;
    result.lon = plain->location_->longitude_;
  } else if (const auto live = Cast<api::messageLiveLocation>(content)) {
    if (!live->location_ || !live->location_->location_) return false;
    result.lat = live->location_->location_->latitude_;
    result.lon = live->location_->location_->longitude_;
    result.livePeriod = std::max(live->location_->live_period_, 1);
  } else if (const auto venue = Cast<api::messageVenue>(content)) {
    if (!venue->venue_ || !venue->venue_->location_) return false;
    result.lat = venue->venue_->location_->latitude_;
    result.lon = venue->venue_->location_->longitude_;
    result.title = venue->venue_->title_;
    result.address = venue->venue_->address_;
  } else {
    return false;
  }
  if (info) *info = std::move(result);
  return true;
}

bool LiveLocationActive(const api::message &message) {
  auto info = LocationInfo();
  if (!MessageLocation(message, &info) || !info.livePeriod) return false;
  const auto now = Td::Now();
  return info.livePeriod == kUntilOff || now < message.date_ || now - message.date_ < info.livePeriod;
}

std::string LiveLocationUpdated(const api::message &message) {
  const auto date = UpdateDate(message);
  const auto now = Td::Now();
  const auto elapsed = now - date;
  if (elapsed < 60) {
    return Tr("lng_live_location_now");
  } else if (const auto minutes = elapsed / 60; minutes < 60) {
    return TrCount("lng_live_location_minutes", minutes);
  } else if (const auto hours = elapsed / 3600; hours < 12) {
    return TrCount("lng_live_location_hours", hours);
  }
  const auto time = Td::FormatTime(date);
  const auto days = Td::DayNumber(now) - Td::DayNumber(date);
  if (days == 0) return Tr("lng_live_location_today", {{"time", time}});
  if (days == 1) return Tr("lng_live_location_yesterday", {{"time", time}});
  return Tr("lng_live_location_date_time", {{"date", Td::FormatDay(date)}, {"time", time}});
}

std::string LiveLocationRemaining(const api::message &message) {
  auto info = LocationInfo();
  if (!MessageLocation(message, &info)) return {};
  if (info.livePeriod == kUntilOff) return "∞";
  const auto elapsed = Td::Now() - message.date_;
  const auto remaining = std::clamp(info.livePeriod - elapsed, 0, info.livePeriod);
  if (remaining < 10) {
    return TrCount("lng_seconds_tiny", remaining);
  } else if (remaining < 3600) {
    return TrCount("lng_minutes_tiny", std::max(remaining / 60, 1));
  } else if (remaining < 86400) {
    return TrCount("lng_hours_tiny", remaining / 3600);
  }
  return TrCount("lng_days_tiny", remaining / 86400);
}

double LiveLocationProgress(const api::message &message) {
  auto info = LocationInfo();
  if (!MessageLocation(message, &info) || !info.livePeriod) return 0.;
  if (info.livePeriod == kUntilOff) return 1.;
  const auto elapsed = Td::Now() - message.date_;
  return std::clamp(double(info.livePeriod - elapsed) / info.livePeriod, 0., 1.);
}

std::string SenderName(const Td::Store &store, const api::message &message) {
  if (const auto user = Cast<api::messageSenderUser>(message.sender_id_.get())) {
    const auto known = store.user(user->user_id_);
    return known ? Td::DisplayName(*known) : std::string();
  } else if (const auto chat = Cast<api::messageSenderChat>(message.sender_id_.get())) {
    const auto known = store.chat(chat->chat_id_);
    return known ? known->title : std::string();
  }
  return {};
}

GtkWidget *MakeSenderAvatar(Td::Store &store, const api::message &message, int size) {
  if (const auto user = Cast<api::messageSenderUser>(message.sender_id_.get())) {
    return MakeUserAvatar(store, user->user_id_, size);
  } else if (const auto chat = Cast<api::messageSenderChat>(message.sender_id_.get())) {
    return MakeAvatar(store, chat->chat_id_, size);
  }
  return adw_avatar_new(size, nullptr, TRUE);
}

namespace {

// --- the picker --------------------------------------------------------------

class LocationPicker final {
 public:
  LocationPicker(MainWindow &window, LocationTarget target);
  ~LocationPicker();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }
  void debug(const std::string &name, const std::string &rest);

 private:
  void build();
  GtkWidget *buildMap();
  void updateWatching();
  void received(const GeoFix &fix);
  void failed(LocationService::Failure failure);
  void refreshRows();
  [[nodiscard]] std::optional<std::pair<double, double>> selected() const;
  void scheduleVenues();
  void requestVenues();
  void sendVenuesRequest();
  void showVenues(std::vector<Venue> venues, bool failed);
  void sendPoint(double lat, double lon);
  void sendVenue(const Venue &venue);
  void done();

  MainWindow &_window;
  Td::Store &_store;
  const LocationTarget _target;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_map = nullptr;
  GtkWidget *_mapPin = nullptr;
  GtkWidget *_positionMarker = nullptr;
  GtkWidget *_sendRow = nullptr;
  GtkWidget *_venues = nullptr;
  GtkWidget *_venuesStatus = nullptr;
  GtkWidget *_venuesSpinner = nullptr;
  GtkWidget *_search = nullptr;
  GtkWidget *_searchBar = nullptr;
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _fix;
  std::optional<LocationService::Failure> _failure;
  bool _follow = true;     // The map follows the device's position until it is moved by hand.
  bool _centered = false;  // The map has been put on the device's position once.
  bool _closed = false;
  bool _done = false;
  std::string _query;
  Td::UserId _venuesBot = 0;
  Td::ChatId _venuesBotChat = 0;
  bool _venuesBotAsked = false;
  std::optional<std::pair<double, double>> _venuesAt;
  std::string _venuesQuery;
  Timer _venuesTimer;
  Td::Lifetime _lifetime;
  Td::Lifetime _venuesLifetime;
  SignalConnections _signals;
  const std::shared_ptr<char> _alive = std::make_shared<char>();
};

LocationPicker::LocationPicker(MainWindow &window, LocationTarget target)
    : _window(window), _store(*window.store()), _target(std::move(target)), _venuesTimer([this] { requestVenues(); }) {
  build();
  _fix = Location().recent(60);
  _signals.connect<void(GObject *, GParamSpec *)>(_window.widget(), "notify::is-active",
                                                  [this](GObject *, GParamSpec *) { updateWatching(); });
  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) {
    _closed = true;
    updateWatching();
  });
  updateWatching();
  refreshRows();
  if (_fix) {
    received(*_fix);
  } else {
    requestVenues();
  }
  g_message("Location picker: shown");
}

LocationPicker::~LocationPicker() {
  _signals.clear();
  _watch = nullptr;
}

void LocationPicker::build() {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_title(ADW_DIALOG(_dialog), Tr("lng_maps_point").c_str());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 560);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1000);
  adw_dialog_set_presentation_mode(ADW_DIALOG(_dialog), ADW_DIALOG_FLOATING);

  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *searchButton = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(searchButton), "system-search-symbolic");
  gtk_widget_set_tooltip_text(searchButton, Tr("lng_dlg_filter").c_str());
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), searchButton);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  // Places by name (the venue bot searches around the map's middle).
  _search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(_search), Tr("lng_dlg_filter").c_str());
  gtk_widget_set_hexpand(_search, TRUE);
  _searchBar = gtk_search_bar_new();
  auto *searchClamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(searchClamp), _search);
  gtk_search_bar_set_child(GTK_SEARCH_BAR(_searchBar), searchClamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(_searchBar), GTK_EDITABLE(_search));
  g_object_bind_property(searchButton, "active", _searchBar, "search-mode-enabled", G_BINDING_BIDIRECTIONAL);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), _searchBar);
  _signals.connect<void(GtkSearchEntry *)>(_search, "search-changed", [this](GtkSearchEntry *) {
    auto query = std::string(gtk_editable_get_text(GTK_EDITABLE(_search)));
    const auto first = query.find_first_not_of(" \t");
    _query = (first == std::string::npos) ? std::string() : query.substr(first, query.find_last_not_of(" \t") - first + 1);
    scheduleVenues();
  });
  _signals.connect<void(GObject *, GParamSpec *)>(_searchBar, "notify::search-mode-enabled", [this](GObject *, GParamSpec *) {
    if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(_searchBar)) && !_query.empty()) {
      _query.clear();
      scheduleVenues();
    }
  });

  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if (auto *map = buildMap()) {
    gtk_box_append(GTK_BOX(content), map);
  }

  // What to send.
  auto *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  _sendRow = adw_action_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(_sendRow), TRUE);
  adw_action_row_add_prefix(ADW_ACTION_ROW(_sendRow), RowIcon("contrail-map-pin-symbolic", "send-location"));
  _signals.connect<void(AdwActionRow *)>(_sendRow, "activated", [this](AdwActionRow *) {
    if (const auto point = selected()) sendPoint(point->first, point->second);
  });
  gtk_list_box_append(GTK_LIST_BOX(list), _sendRow);

  // Places nearby.
  auto *venuesTitle = gtk_label_new(Tr("lng_maps_places_in_area").c_str());
  gtk_widget_add_css_class(venuesTitle, "heading");
  gtk_label_set_xalign(GTK_LABEL(venuesTitle), 0);
  _venuesSpinner = adw_spinner_new();
  gtk_widget_set_size_request(_venuesSpinner, 16, 16);
  auto *venuesHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(venuesHeader), venuesTitle);
  gtk_box_append(GTK_BOX(venuesHeader), _venuesSpinner);
  gtk_widget_set_margin_top(venuesHeader, 18);
  gtk_widget_set_margin_bottom(venuesHeader, 6);
  _venues = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_venues), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(_venues, "boxed-list");
  gtk_widget_set_visible(_venues, FALSE);
  _venuesStatus = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_venuesStatus, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(_venuesStatus), TRUE);
  gtk_widget_set_margin_top(_venuesStatus, 6);
  auto *source = gtk_label_new(Tr("lng_maps_venues_source").c_str());
  gtk_widget_add_css_class(source, "dim-label");
  gtk_widget_add_css_class(source, "caption");
  gtk_widget_set_margin_top(source, 8);

  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  SetMarginAll(column, 12);
  gtk_box_append(GTK_BOX(column), list);
  gtk_box_append(GTK_BOX(column), venuesHeader);
  gtk_box_append(GTK_BOX(column), _venues);
  gtk_box_append(GTK_BOX(column), _venuesStatus);
  gtk_box_append(GTK_BOX(column), source);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), column);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(content), scroll);

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), content);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);
}

GtkWidget *LocationPicker::buildMap() {
#ifdef CONTRAIL_SHUMATE
  _map = MakeMap();
  MapCenterOn(_map, 30., 0., 2., false);
  // The place to send is under the pin in the middle, as in the phone apps.
  auto *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), _map);
  _mapPin = MakeMapPin();
  gtk_widget_set_visible(_mapPin, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _mapPin);
  auto *mine = MapButton("contrail-my-location-symbolic", MyLocationText());
  gtk_widget_set_halign(mine, GTK_ALIGN_END);
  gtk_widget_set_valign(mine, GTK_ALIGN_END);
  gtk_widget_set_margin_end(mine, 12);
  gtk_widget_set_margin_bottom(mine, 64);  // Above the attribution (two lines on a phone).
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), mine);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), MakeZoomButtons(_map));
  _signals.clicked(mine, [this] {
    _follow = true;
    if (_fix) MapCenterOn(_map, _fix->lat, _fix->lon, std::max(MapZoom(_map), kPlaceZoom), true);
    refreshRows();
    scheduleVenues();
  });
  const auto weak = std::weak_ptr<char>(_alive);
  MapOnUserMove(_map, [this, weak] {
    if (weak.expired() || !_follow) return;
    _follow = false;
    refreshRows();
  });
  MapOnCenterChanged(_map, [this, weak] {
    if (weak.expired() || _follow) return;
    refreshRows();
    scheduleVenues();
  });
  gtk_widget_set_size_request(overlay, -1, 260);
  gtk_widget_set_vexpand(overlay, TRUE);
  return overlay;
#else
  return nullptr;
#endif
}

void LocationPicker::updateWatching() {
  const auto wanted = !_closed && WindowActive(_window);
  if (!wanted) {
    _watch = nullptr;
    return;
  } else if (_watch) {
    return;
  }
  _failure = std::nullopt;
  const auto weak = std::weak_ptr<char>(_alive);
  _watch = Location().watch(
      [this, weak](const GeoFix &fix) {
        if (!weak.expired()) received(fix);
      },
      [this, weak](LocationService::Failure failure) {
        if (!weak.expired()) failed(failure);
      });
  refreshRows();
}

void LocationPicker::received(const GeoFix &fix) {
  _fix = fix;
  _failure = std::nullopt;
#ifdef CONTRAIL_SHUMATE
  if (_map) {
    if (!_positionMarker) {
      _positionMarker = MapAddMarker(_map, MakePositionDot(), fix.lat, fix.lon);
    } else {
      MapMoveMarker(_positionMarker, fix.lat, fix.lon);
    }
    if (_follow) {
      // Without an animation: one every second (a fix) would fight a finger on the map.
      MapCenterOn(_map, fix.lat, fix.lon, _centered ? MapZoom(_map) : kPlaceZoom, false);
      _centered = true;
    }
  }
#endif
  refreshRows();
  if (_follow) scheduleVenues();
}

void LocationPicker::failed(LocationService::Failure failure) {
  _failure = failure;
  // The watch is over (from inside its own callback): a new one when the window is activated again.
  auto *self = this;
  const auto weak = std::weak_ptr<char>(_alive);
  struct Drop {
    LocationPicker *self;
    std::weak_ptr<char> weak;
  };
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
    const auto drop = static_cast<Drop *>(data);
    if (!drop->weak.expired()) drop->self->_watch = nullptr;
    return G_SOURCE_REMOVE;
  }, new Drop{self, weak}, +[](gpointer data) { delete static_cast<Drop *>(data); });
  refreshRows();
}

std::optional<std::pair<double, double>> LocationPicker::selected() const {
#ifdef CONTRAIL_SHUMATE
  if (_map && !_follow) {
    auto lat = 0., lon = 0.;
    MapCenter(_map, &lat, &lon);
    return std::pair(lat, lon);
  }
#endif
  if (_fix) return std::pair(_fix->lat, _fix->lon);
  return std::nullopt;
}

void LocationPicker::refreshRows() {
  const auto point = selected();
  const auto away = point && (!_fix || DistanceMeters(point->first, point->second, _fix->lat, _fix->lon) > kSelectedAway);
  const auto chooseOnMap = _map && !_follow && away;
  if (_mapPin) gtk_widget_set_visible(_mapPin, chooseOnMap);
  std::string title, subtitle;
  if (chooseOnMap) {
    title = Tr("lng_maps_point_send");
    subtitle = FormatCoordinates(point->first, point->second);
  } else {
    title = Translated("Send My Current Location", "Отправить своё местоположение", "Надіслати своє місцезнаходження");
    if (_fix) {
      subtitle = Translated("Accurate to {distance}", "Точность: {distance}", "Точність: {distance}");
      subtitle.replace(subtitle.find("{distance}"), 10, FormatDistance(_fix->accuracy));
    } else {
      subtitle = _failure ? FailureText(*_failure)
                          : Translated("Locating…", "Определение местоположения…", "Визначення місцезнаходження…");
    }
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_sendRow), Escape(title).c_str());
  adw_action_row_set_subtitle(ADW_ACTION_ROW(_sendRow), Escape(subtitle).c_str());
  gtk_widget_set_sensitive(_sendRow, point.has_value() && (chooseOnMap || _fix));
}

// --- places ------------------------------------------------------------------

void LocationPicker::scheduleVenues() {
  _venuesTimer.callOnce(kVenuesDelay);
}

void LocationPicker::requestVenues() {
  const auto point = selected();
  if (!point) {
    gtk_widget_set_visible(_venuesSpinner, FALSE);
    gtk_label_set_text(GTK_LABEL(_venuesStatus), Tr("lng_maps_choose_to_search").c_str());
    gtk_widget_set_visible(_venuesStatus, TRUE);
    return;
  }
  if (_venuesAt && _venuesQuery == _query
      && DistanceMeters(_venuesAt->first, _venuesAt->second, point->first, point->second) < kVenuesMinMove) {
    return;
  }
  _venuesAt = point;
  _venuesQuery = _query;
  gtk_widget_set_visible(_venuesSpinner, TRUE);
  if (_venuesBot) {
    sendVenuesRequest();
    return;
  } else if (_venuesBotAsked) {
    return;
  }
  // The bot the phone apps ask about places (Foursquare's, by the server's configuration).
  _venuesBotAsked = true;
  auto &client = _store.client();
  client.request<api::optionValueString>(
      api::make_object<api::getOption>("venue_search_bot_username"), _lifetime,
      [this, &client](api::optionValueString &name) {
        client.request<api::chat>(
            api::make_object<api::searchPublicChat>(name.value_), _lifetime,
            [this](api::chat &chat) {
              _venuesBotAsked = false;
              const auto user = Cast<api::chatTypePrivate>(chat.type_.get());
              if (!user) {
                showVenues({}, true);
                return;
              }
              _venuesBot = user->user_id_;
              _venuesBotChat = chat.id_;
              sendVenuesRequest();
            },
            [this](api::error &) {
              _venuesBotAsked = false;
              showVenues({}, true);
            });
      },
      [this](api::error &) {
        _venuesBotAsked = false;
        showVenues({}, true);
      });
}

void LocationPicker::sendVenuesRequest() {
  if (!_venuesAt) return;
  _venuesLifetime.reset();
  g_message("Location picker: places near %s \"%s\"", FormatCoordinates(_venuesAt->first, _venuesAt->second).c_str(),
            _venuesQuery.c_str());
  _store.client().request<api::inlineQueryResults>(
      api::make_object<api::getInlineQueryResults>(
          _venuesBot, _target.chat, api::make_object<api::location>(_venuesAt->first, _venuesAt->second, 0.),
          _venuesQuery, std::string()),
      _venuesLifetime,
      [this](api::inlineQueryResults &results) {
        auto venues = std::vector<Venue>();
        for (const auto &result : results.results_) {
          const auto venue = Cast<api::inlineQueryResultVenue>(result.get());
          if (!venue || !venue->venue_ || !venue->venue_->location_) continue;
          const auto &data = *venue->venue_;
          venues.push_back({.lat = data.location_->latitude_, .lon = data.location_->longitude_, .title = data.title_,
                            .address = data.address_, .provider = data.provider_, .id = data.id_, .type = data.type_});
        }
        showVenues(std::move(venues), false);
      },
      [this](api::error &) { showVenues({}, true); });
}

void LocationPicker::showVenues(std::vector<Venue> venues, bool failed) {
  gtk_widget_set_visible(_venuesSpinner, FALSE);
  if (failed) _venuesAt = std::nullopt;  // Asked again next time.
  gtk_list_box_remove_all(GTK_LIST_BOX(_venues));
  for (const auto &venue : venues) {
    auto *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Escape(venue.title).c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Escape(venue.address).c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), RowIcon("contrail-map-pin-symbolic", "venue"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    _signals.connect<void(AdwActionRow *)>(row, "activated", [this, venue](AdwActionRow *) { sendVenue(venue); });
    gtk_list_box_append(GTK_LIST_BOX(_venues), row);
  }
  gtk_widget_set_visible(_venues, !venues.empty());
  gtk_label_set_text(GTK_LABEL(_venuesStatus), venues.empty() ? Tr("lng_maps_no_places").c_str() : "");
  gtk_widget_set_visible(_venuesStatus, venues.empty());
  g_message("Location picker: %zu places%s", venues.size(), failed ? " (failed)" : "");
}

// --- sending -----------------------------------------------------------------

void LocationPicker::sendPoint(double lat, double lon) {
  if (_done) return;
  g_message("Location picker: sending %s", FormatCoordinates(lat, lon).c_str());
  _target.send(api::make_object<api::inputMessageLocation>(api::make_object<api::location>(lat, lon, 0.)));
  done();
}

void LocationPicker::sendVenue(const Venue &venue) {
  if (_done) return;
  g_message("Location picker: sending the place \"%s\"", venue.title.c_str());
  _target.send(api::make_object<api::inputMessageVenue>(api::make_object<api::venue>(
      api::make_object<api::location>(venue.lat, venue.lon, 0.), venue.title, venue.address, venue.provider, venue.id,
      venue.type)));
  done();
}

// loc-send | loc-venue N | loc-search TEXT | loc-move LAT LON (as a finger moves the map) | loc-mine | loc-state
void LocationPicker::debug(const std::string &name, const std::string &rest) {
  if (name == "loc-send") {
    if (gtk_widget_get_sensitive(_sendRow)) g_signal_emit_by_name(_sendRow, "activated");
    else g_message("Location picker: nothing to send yet");
  } else if (name == "loc-venue") {
    auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_venues), std::atoi(rest.c_str()));
    if (row) g_signal_emit_by_name(row, "activated");
    else g_message("Location picker: no place %s", rest.c_str());
  } else if (name == "loc-search") {
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(_searchBar), !rest.empty());
    gtk_editable_set_text(GTK_EDITABLE(_search), rest.c_str());
  } else if (name == "loc-move") {
#ifdef CONTRAIL_SHUMATE
    auto *end = const_cast<char *>(rest.c_str());
    const auto lat = g_ascii_strtod(end, &end);
    const auto lon = g_ascii_strtod(end, &end);
    if (_map) {
      _follow = false;
      MapCenterOn(_map, lat, lon, std::max(MapZoom(_map), kPlaceZoom), false);
      refreshRows();
      scheduleVenues();
    }
#endif
  } else if (name == "loc-mine") {
    _follow = true;
    refreshRows();
    scheduleVenues();
  }
  auto venues = 0;
  for (auto *row = gtk_widget_get_first_child(_venues); row; row = gtk_widget_get_next_sibling(row)) ++venues;
  g_message("Location picker: send=\"%s\" \"%s\" sensitive=%d pin=%d follow=%d venues=%d status=\"%s\" watching=%d",
            adw_preferences_row_get_title(ADW_PREFERENCES_ROW(_sendRow)),
            adw_action_row_get_subtitle(ADW_ACTION_ROW(_sendRow)), int(gtk_widget_get_sensitive(_sendRow)),
            int(_mapPin && gtk_widget_get_visible(_mapPin)), int(_follow), venues,
            gtk_widget_get_visible(_venuesStatus) ? gtk_label_get_text(GTK_LABEL(_venuesStatus)) : "",
            int(_watch != nullptr));
}

void LocationPicker::done() {
  _done = true;
  adw_dialog_close(ADW_DIALOG(_dialog));
}

// --- a location on the map ---------------------------------------------------

#ifdef CONTRAIL_SHUMATE

class LocationViewer final {
 public:
  LocationViewer(MainWindow &window, const api::message &message);
  ~LocationViewer();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }
  void debug(const std::string &name);

 private:
  void updated(api::Update &update);
  void refresh();
  void received(const GeoFix &fix);
  void toggleMyLocation();

  MainWindow &_window;
  Td::Store &_store;
  const Td::ChatId _chat;
  std::int64_t _id = 0;
  api::object_ptr<api::message> _message;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_map = nullptr;
  GtkWidget *_marker = nullptr;
  GtkWidget *_position = nullptr;
  GtkWidget *_details = nullptr;
  GtkWidget *_distance = nullptr;
  GtkWidget *_mine = nullptr;
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _fix;
  double _lat = 0.;
  double _lon = 0.;
  bool _moved = false;  // By hand: the map doesn't follow a live location then.
  Timer _timer;
  Td::Lifetime _lifetime;
  SignalConnections _signals;
  const std::shared_ptr<char> _alive = std::make_shared<char>();
};

api::object_ptr<api::message> CopyMessage(const api::message &message) {
  // Only what the viewer reads: the content's location, the dates and the sender.
  auto copy = api::make_object<api::message>();
  copy->id_ = message.id_;
  copy->chat_id_ = message.chat_id_;
  copy->date_ = message.date_;
  copy->edit_date_ = message.edit_date_;
  copy->is_outgoing_ = message.is_outgoing_;
  if (const auto user = Cast<api::messageSenderUser>(message.sender_id_.get())) {
    copy->sender_id_ = api::make_object<api::messageSenderUser>(user->user_id_);
  } else if (const auto chat = Cast<api::messageSenderChat>(message.sender_id_.get())) {
    copy->sender_id_ = api::make_object<api::messageSenderChat>(chat->chat_id_);
  }
  auto info = LocationInfo();
  if (MessageLocation(message, &info)) {
    auto location = api::make_object<api::location>(info.lat, info.lon, 0.);
    if (info.livePeriod) {
      copy->content_ = api::make_object<api::messageLiveLocation>(
          api::make_object<api::liveLocation>(std::move(location), info.livePeriod, 0, 0), 0);
    } else if (!info.title.empty()) {
      copy->content_ = api::make_object<api::messageVenue>(
          api::make_object<api::venue>(std::move(location), info.title, info.address, "", "", ""));
    } else {
      copy->content_ = api::make_object<api::messageLocation>(std::move(location));
    }
  }
  return copy;
}

LocationViewer::LocationViewer(MainWindow &window, const api::message &message)
    : _window(window), _store(*window.store()), _chat(message.chat_id_), _id(message.id_),
      _message(CopyMessage(message)), _timer([this] { refresh(); }) {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 1200);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1200);
  adw_dialog_set_presentation_mode(ADW_DIALOG(_dialog), ADW_DIALOG_FLOATING);
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  _title = adw_window_title_new("", "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), _title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  _map = MakeMap();
  auto *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), _map);
  auto *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_valign(buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_end(buttons, 12);
  gtk_widget_set_margin_bottom(buttons, 64);  // Above the attribution (two lines on a phone).
  auto *place = MapButton("contrail-map-pin-symbolic", Tr("lng_maps_point"));
  _signals.clicked(place, [this] {
    _moved = false;
    MapCenterOn(_map, _lat, _lon, std::max(MapZoom(_map), kPlaceZoom), true);
  });
  gtk_box_append(GTK_BOX(buttons), place);
  _mine = MapButton("contrail-my-location-symbolic", MyLocationText());
  _signals.clicked(_mine, [this] { toggleMyLocation(); });
  gtk_box_append(GTK_BOX(buttons), _mine);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buttons);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), MakeZoomButtons(_map));
  const auto weak = std::weak_ptr<char>(_alive);
  MapOnUserMove(_map, [this, weak] {
    if (!weak.expired()) _moved = true;
  });
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), overlay);

  // The address or the live location's state, how far, and what can be done.
  auto *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(bottom, "location-details");
  _details = gtk_label_new(nullptr);
  gtk_label_set_wrap(GTK_LABEL(_details), TRUE);
  gtk_label_set_xalign(GTK_LABEL(_details), 0);
  gtk_box_append(GTK_BOX(bottom), _details);
  _distance = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_distance, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(_distance), 0);
  gtk_widget_set_visible(_distance, FALSE);
  gtk_box_append(GTK_BOX(bottom), _distance);
  auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);
  auto *open = gtk_button_new_with_label(Translated("Open in Maps", "Открыть в Картах", "Відкрити в Картах").c_str());
  gtk_widget_add_css_class(open, "pill");
  gtk_widget_add_css_class(open, "suggested-action");
  _signals.clicked(open, [this] { OpenInMapsApp(_lat, _lon); });
  gtk_box_append(GTK_BOX(actions), open);
  gtk_box_append(GTK_BOX(bottom), actions);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), bottom);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), clamp);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);

  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) {
    _watch = nullptr;
    _timer.cancel();
    _lifetime.reset();
  });
  // A live location moves as it is edited.
  _store.client().subscribe([this](api::Update &update) { updated(update); }, _lifetime);
  refresh();
  MapCenterOn(_map, _lat, _lon, kPlaceZoom, false);
  g_message("Location viewer: %s", FormatCoordinates(_lat, _lon).c_str());
}

LocationViewer::~LocationViewer() {
  _signals.clear();
  _watch = nullptr;
}

void LocationViewer::updated(api::Update &update) {
  switch (update.get_id()) {
    case api::updateMessageContent::ID: {
      auto &data = static_cast<api::updateMessageContent &>(update);
      if (data.chat_id_ != _chat || data.message_id_ != _id || !data.new_content_) break;
      auto copy = api::make_object<api::message>();
      copy->content_ = std::move(data.new_content_);
      if (MessageLocation(*copy, nullptr)) {
        _message->content_ = std::move(copy->content_);
        refresh();
      }
      break;
    }
    case api::updateMessageEdited::ID: {
      auto &data = static_cast<api::updateMessageEdited &>(update);
      if (data.chat_id_ != _chat || data.message_id_ != _id) break;
      _message->edit_date_ = data.edit_date_;
      refresh();
      break;
    }
    case api::updateMessageSendSucceeded::ID: {
      auto &data = static_cast<api::updateMessageSendSucceeded &>(update);
      if (data.message_ && data.message_->chat_id_ == _chat && data.old_message_id_ == _id) {
        _id = data.message_->id_;
        _message->id_ = _id;
      }
      break;
    }
    case api::updateDeleteMessages::ID: {
      auto &data = static_cast<api::updateDeleteMessages &>(update);
      if (data.chat_id_ == _chat && data.is_permanent_
          && std::ranges::find(data.message_ids_, _id) != data.message_ids_.end()) {
        adw_dialog_close(ADW_DIALOG(_dialog));
      }
      break;
    }
    default: break;
  }
}

void LocationViewer::refresh() {
  auto info = LocationInfo();
  if (!MessageLocation(*_message, &info)) return;
  const auto live = LiveLocationActive(*_message);
  const auto movedFrom = std::pair(_lat, _lon);
  _lat = info.lat;
  _lon = info.lon;
  if (!_marker) {
    auto *avatar = info.livePeriod ? MakeSenderAvatar(_store, *_message, 30) : nullptr;
    _marker = MapAddMarker(_map, MakeMapPin(avatar), _lat, _lon);
  } else if (movedFrom != std::pair(_lat, _lon)) {
    MapMoveMarker(_marker, _lat, _lon);
    if (!_moved) MapCenterOn(_map, _lat, _lon, MapZoom(_map), true);
  }
  std::string title, subtitle, details;
  if (info.livePeriod) {
    title = Tr("lng_live_location");
    subtitle = SenderName(_store, *_message);
    details = live ? LiveLocationUpdated(*_message) : std::string();
  } else if (!info.title.empty()) {
    title = info.title;
    details = info.address;
  } else {
    title = Tr("lng_maps_point");
  }
  if (details.empty()) details = FormatCoordinates(_lat, _lon);
  adw_window_title_set_title(ADW_WINDOW_TITLE(_title), title.c_str());
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(_title), subtitle.c_str());
  gtk_label_set_text(GTK_LABEL(_details), details.c_str());
  if (_fix) {
    auto text = Translated("{distance} away", "{distance} от вас", "{distance} від вас");
    text.replace(text.find("{distance}"), 10, FormatDistance(DistanceMeters(_fix->lat, _fix->lon, _lat, _lon)));
    gtk_label_set_text(GTK_LABEL(_distance), text.c_str());
  }
  gtk_widget_set_visible(_distance, _fix.has_value());
  if (live) _timer.callOnce(30 * 1000);
}

// The device's position is asked for only after this button: looking at a place doesn't turn the GPS on.
void LocationViewer::toggleMyLocation() {
  if (_watch) {
    if (_fix) MapCenterOn(_map, _fix->lat, _fix->lon, MapZoom(_map), true);
    return;
  }
  gtk_widget_add_css_class(_mine, "active");
  const auto weak = std::weak_ptr<char>(_alive);
  _watch = Location().watch(
      [this, weak](const GeoFix &fix) {
        if (!weak.expired()) received(fix);
      },
      [this, weak](LocationService::Failure failure) {
        if (weak.expired()) return;
        ShowToast(FailureText(failure));
        gtk_widget_remove_css_class(_mine, "active");
        struct Drop {
          LocationViewer *self;
          std::weak_ptr<char> weak;
        };
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
          const auto drop = static_cast<Drop *>(data);
          if (!drop->weak.expired()) drop->self->_watch = nullptr;
          return G_SOURCE_REMOVE;
        }, new Drop{this, weak}, +[](gpointer data) { delete static_cast<Drop *>(data); });
      });
}

// loc-mine | loc-state
void LocationViewer::debug(const std::string &name) {
  if (name == "loc-mine") toggleMyLocation();
  g_message("Location viewer: \"%s\" \"%s\" details=\"%s\" distance=\"%s\" at %s watching=%d",
            adw_window_title_get_title(ADW_WINDOW_TITLE(_title)), adw_window_title_get_subtitle(ADW_WINDOW_TITLE(_title)),
            gtk_label_get_text(GTK_LABEL(_details)),
            gtk_widget_get_visible(_distance) ? gtk_label_get_text(GTK_LABEL(_distance)) : "",
            FormatCoordinates(_lat, _lon).c_str(), int(_watch != nullptr));
}

void LocationViewer::received(const GeoFix &fix) {
  const auto first = !_fix;
  _fix = fix;
  if (!_position) {
    _position = MapAddMarker(_map, MakePositionDot(), fix.lat, fix.lon);
  } else {
    MapMoveMarker(_position, fix.lat, fix.lon);
  }
  if (first) {
    _moved = true;
    MapCenterOn(_map, fix.lat, fix.lon, MapZoom(_map), true);
  }
  refresh();
}

#endif  // CONTRAIL_SHUMATE

}  // namespace

void ShowLocationPicker(MainWindow &window, LocationTarget target) {
  if (!window.widget() || !window.store()) return;
  auto picker = std::make_unique<LocationPicker>(window, std::move(target));
  auto *dialog = picker->dialog();
  AttachData(dialog, "contrail-location-picker", std::move(picker));
  adw_dialog_present(ADW_DIALOG(dialog), window.widget());
}

void ShowLocation(MainWindow &window, const api::message &message) {
  auto info = LocationInfo();
  if (!MessageLocation(message, &info) || !window.widget() || !window.store()) return;
#ifdef CONTRAIL_SHUMATE
  auto viewer = std::make_unique<LocationViewer>(window, message);
  auto *dialog = viewer->dialog();
  AttachData(dialog, "contrail-location-viewer", std::move(viewer));
  adw_dialog_present(ADW_DIALOG(dialog), window.widget());
#else
  OpenInMapsApp(info.lat, info.lon);
#endif
}

bool LocationDebugCommand(MainWindow &window, const std::string &name, const std::string &rest) {
  if (!name.starts_with("loc-") || !window.widget()) return false;
  if (name == "loc-fix") {
    // "loc-fix LAT LON [ACCURACY]": a position as if from the location service (while something watches it).
    // Whatever the language's decimal separator.
    auto *end = const_cast<char *>(rest.c_str());
    const auto lat = g_ascii_strtod(end, &end);
    const auto lon = g_ascii_strtod(end, &end);
    const auto accuracy = g_ascii_strtod(end, &end);
    Location().debugFix(lat, lon, accuracy > 0. ? accuracy : 10.);
    return true;
  } else if (name == "loc-service") {
    g_message("Location: %s", Location().debugState().c_str());
    return true;
  }
  auto *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window.widget()));
  if (!dialog) {
    g_message("Location: no dialog shown");
    return true;
  } else if (name == "loc-close") {
    adw_dialog_close(dialog);
    return true;
  }
  if (const auto picker = static_cast<LocationPicker *>(g_object_get_data(G_OBJECT(dialog), "contrail-location-picker"))) {
    picker->debug(name, rest);
    return true;
  }
#ifdef CONTRAIL_SHUMATE
  if (const auto viewer = static_cast<LocationViewer *>(g_object_get_data(G_OBJECT(dialog), "contrail-location-viewer"))) {
    viewer->debug(name);
    return true;
  }
#endif
  g_message("Location: another dialog is shown");
  return true;
}

}  // namespace Contrail
