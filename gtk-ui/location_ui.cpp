#include "location_ui.hpp"

#include "live_location.hpp"
#include "location.hpp"
#include "main_window.hpp"
#include "map_view.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/current_geo_location.h"
#include "data/data_changes.h"
#include "data/data_chat_participant_status.h"
#include "data/data_location.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

#include <rpl/lifetime.h>

namespace Contrail {
namespace {

constexpr auto kPlaceZoom = 16.;
constexpr auto kVenuesDelay = 700;            // Milliseconds after the map stopped or the query changed.
constexpr auto kVenuesMinMove = 150.;         // Meters the map moves before the places nearby are asked again.
constexpr auto kSelectedAway = 30.;           // Meters from the device's position: the place under the pin.

TimeId UpdateDate(not_null<const HistoryItem *> item) {
  const auto edited = item->Get<HistoryMessageEdited>();
  return edited ? edited->date : item->date();
}

GtkWidget *RowIcon(const char *icon, const char *cssClass) {
  auto *image = gtk_image_new_from_icon_name(icon);
  gtk_image_set_pixel_size(GTK_IMAGE(image), 20);
  gtk_widget_add_css_class(image, "location-row-icon");
  gtk_widget_add_css_class(image, cssClass);
  gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
  return image;
}

GtkWidget *MapButton(const char *icon, const QString &tooltip) {
  auto *button = MakeIconButton(icon, U8(tooltip).c_str(), "circular");
  gtk_widget_add_css_class(button, "osd");
  gtk_widget_add_css_class(button, "map-button");
  return button;
}

QString MyLocationText() {
  return Translated("My Location", "Моё местоположение", "Моє місцезнаходження");
}

QString FailureText(LocationService::Failure failure) {
  return (failure == LocationService::Failure::Denied)
             ? Translated("No access to the location", "Нет доступа к геопозиции", "Немає доступу до геопозиції")
             : Translated("Location is unavailable", "Геопозиция недоступна", "Геопозиція недоступна");
}

// Live locations can't go to channels and to Saved Messages, as in the phone apps.
bool CanShareLive(not_null<PeerData *> peer) {
  return !peer->isSelf() && !peer->isBroadcast();
}

void ChooseLivePeriod(GtkWidget *parent, Fn<void(TimeId)> chosen) {
  auto *dialog = adw_alert_dialog_new(
      U8(Translated("Share your live location for…", "Транслировать геопозицию…", "Транслювати геопозицію…")).c_str(),
      U8(Translated("People in this chat will see where you are in real time.",
                    "Участники чата будут видеть, где вы находитесь, в реальном времени.",
                    "Учасники чату бачитимуть, де ви, у реальному часі."))
          .c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                 "15m", U8(tr::lng_minutes(tr::now, lt_count, 15)).c_str(),
                                 "1h", U8(tr::lng_hours(tr::now, lt_count, 1)).c_str(),
                                 "8h", U8(tr::lng_hours(tr::now, lt_count, 8)).c_str(),
                                 "off", U8(Translated("Until I turn it off", "Пока не выключу", "Доки не вимкну")).c_str(),
                                 "cancel", U8(tr::lng_cancel(tr::now)).c_str(), nullptr);
  adw_alert_dialog_set_prefer_wide_layout(ADW_ALERT_DIALOG(dialog), FALSE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    const auto id = std::string(response);
    const auto period = (id == "15m") ? TimeId(15 * 60)
                        : (id == "1h") ? TimeId(3600)
                        : (id == "8h") ? TimeId(8 * 3600)
                        : (id == "off") ? LiveLocations::kUntilOff
                                        : TimeId(0);
    if (period) chosen(period);
  });
  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

// --- the picker --------------------------------------------------------------

class LocationPicker final : public base::has_weak_ptr {
public:
  LocationPicker(not_null<MainWindow *> window, not_null<Data::Thread *> thread, FullMsgId replyTo, Fn<void()> sent);
  ~LocationPicker();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }

private:
  void build();
  GtkWidget *buildMap();
  void updateWatching();
  void received(const GeoFix &fix);
  void failed(LocationService::Failure failure);
  void refreshRows();
  [[nodiscard]] std::optional<std::pair<double, double>> selected() const;
  [[nodiscard]] std::optional<std::pair<double, double>> venuesPoint() const;
  void scheduleVenues();
  void requestVenues();
  void sendVenuesRequest();
  void showVenues(std::vector<Data::InputVenue> venues, bool failed);
  void sendPoint(double lat, double lon);
  void sendVenue(const Data::InputVenue &venue);
  void shareLive();
  void done();

  const not_null<MainWindow *> _window;
  const not_null<Data::Thread *> _thread;
  const not_null<Main::Session *> _session;
  const FullMsgId _replyTo;
  const Fn<void()> _sent;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_map = nullptr;
  GtkWidget *_mapPin = nullptr;
  GtkWidget *_positionMarker = nullptr;
  GtkWidget *_sendRow = nullptr;
  GtkWidget *_sendIcon = nullptr;
  GtkWidget *_liveRow = nullptr;
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
  QString _query;
  UserData *_venuesBot = nullptr;
  mtpRequestId _venuesBotRequest = 0;
  mtpRequestId _venuesRequest = 0;
  std::optional<std::pair<double, double>> _venuesAt;
  QString _venuesQuery;
  base::Timer _venuesTimer;
  SignalConnections _signals;
  rpl::lifetime _lifetime;
};

LocationPicker::LocationPicker(not_null<MainWindow *> window, not_null<Data::Thread *> thread, FullMsgId replyTo,
                               Fn<void()> sent)
    : _window(window), _thread(thread), _session(&thread->session()), _replyTo(replyTo), _sent(std::move(sent)),
      _venuesTimer([=] { requestVenues(); }) {
  build();
  _fix = Location().recent(60);
  Live().changes() | rpl::on_next([=] { refreshRows(); }, _lifetime);
  _signals.connect<void(GObject *, GParamSpec *)>(_window->widget(), "notify::is-active",
                                                  [=](GObject *, GParamSpec *) { updateWatching(); });
  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [=](AdwDialog *) {
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
}

LocationPicker::~LocationPicker() {
  _signals.clear();
  _watch = nullptr;
  if (_venuesRequest) _session->api().request(_venuesRequest).cancel();
  if (_venuesBotRequest) _session->api().request(_venuesBotRequest).cancel();
}

void LocationPicker::build() {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_title(ADW_DIALOG(_dialog), U8(tr::lng_maps_point(tr::now)).c_str());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 560);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1000);
  adw_dialog_set_presentation_mode(ADW_DIALOG(_dialog), ADW_DIALOG_FLOATING);

  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *searchButton = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(searchButton), "system-search-symbolic");
  gtk_widget_set_tooltip_text(searchButton, U8(tr::lng_dlg_filter(tr::now)).c_str());
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), searchButton);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  // Places by name (the venue bot searches around the map's middle).
  _search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(_search), U8(tr::lng_dlg_filter(tr::now)).c_str());
  gtk_widget_set_hexpand(_search, TRUE);
  _searchBar = gtk_search_bar_new();
  auto *searchClamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(searchClamp), _search);
  gtk_search_bar_set_child(GTK_SEARCH_BAR(_searchBar), searchClamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(_searchBar), GTK_EDITABLE(_search));
  g_object_bind_property(searchButton, "active", _searchBar, "search-mode-enabled", G_BINDING_BIDIRECTIONAL);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), _searchBar);
  _signals.connect<void(GtkSearchEntry *)>(_search, "search-changed", [=](GtkSearchEntry *) {
    _query = Q(gtk_editable_get_text(GTK_EDITABLE(_search))).trimmed();
    scheduleVenues();
  });
  _signals.connect<void(GObject *, GParamSpec *)>(_searchBar, "notify::search-mode-enabled", [=](GObject *, GParamSpec *) {
    if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(_searchBar)) && !_query.isEmpty()) {
      _query = QString();
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
  _sendIcon = RowIcon("contrail-map-pin-symbolic", "send-location");
  adw_action_row_add_prefix(ADW_ACTION_ROW(_sendRow), _sendIcon);
  _signals.connect<void(AdwActionRow *)>(_sendRow, "activated", [=](AdwActionRow *) {
    if (const auto point = selected()) sendPoint(point->first, point->second);
  });
  gtk_list_box_append(GTK_LIST_BOX(list), _sendRow);
  if (CanShareLive(_thread->peer())) {
    _liveRow = adw_action_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(_liveRow), TRUE);
    adw_action_row_add_prefix(ADW_ACTION_ROW(_liveRow), RowIcon("contrail-live-location-symbolic", "live-location"));
    _signals.connect<void(AdwActionRow *)>(_liveRow, "activated", [=](AdwActionRow *) { shareLive(); });
    gtk_list_box_append(GTK_LIST_BOX(list), _liveRow);
  }

  // Places nearby.
  auto *venuesTitle = gtk_label_new(U8(tr::lng_maps_places_in_area(tr::now)).c_str());
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
  auto *source = gtk_label_new(U8(tr::lng_maps_venues_source(tr::now)).c_str());
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
  auto initial = Core::ResolveCurrentCountryLocation();
  if (initial) {
    MapCenterOn(_map, initial.point.x(), initial.point.y(), 5., false);
  } else {
    MapCenterOn(_map, 30., 0., 2., false);
  }
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
  _signals.clicked(mine, [=] {
    _follow = true;
    if (_fix) MapCenterOn(_map, _fix->lat, _fix->lon, std::max(MapZoom(_map), kPlaceZoom), true);
    refreshRows();
    scheduleVenues();
  });
  MapOnUserMove(_map, crl::guard(this, [=] {
    if (!_follow) return;
    _follow = false;
    refreshRows();
  }));
  MapOnCenterChanged(_map, crl::guard(this, [=] {
    if (!_follow) {
      refreshRows();
      scheduleVenues();
    }
  }));
  gtk_widget_set_size_request(overlay, -1, 260);
  gtk_widget_set_vexpand(overlay, TRUE);
  return overlay;
#else
  return nullptr;
#endif
}

void LocationPicker::updateWatching() {
  const auto wanted = !_closed && _window->isActive();
  if (!wanted) {
    _watch = nullptr;
    return;
  } else if (_watch) {
    return;
  }
  _failure = std::nullopt;
  const auto weak = base::make_weak(this);
  _watch = Location().watch(
      [=](const GeoFix &fix) {
        if (const auto strong = weak.get()) strong->received(fix);
      },
      [=](LocationService::Failure failure) {
        if (const auto strong = weak.get()) strong->failed(failure);
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
      MapCenterOn(_map, fix.lat, fix.lon, _centered ? MapZoom(_map) : kPlaceZoom, _centered);
      _centered = true;
    }
  }
#endif
  refreshRows();
  if (_follow) scheduleVenues();
}

void LocationPicker::failed(LocationService::Failure failure) {
  _failure = failure;
  // The watch is over: a new one when the window is activated again.
  crl::on_main(this, [=] { _watch = nullptr; });
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
  QString title, subtitle;
  if (chooseOnMap) {
    title = tr::lng_maps_point_send(tr::now);
    subtitle = FormatCoordinates(point->first, point->second);
  } else {
    title = Translated("Send My Current Location", "Отправить своё местоположение", "Надіслати своє місцезнаходження");
    subtitle = _fix ? Translated("Accurate to %1", "Точность: %1", "Точність: %1").arg(FormatDistance(_fix->accuracy))
               : _failure ? FailureText(*_failure)
                          : Translated("Locating…", "Определение местоположения…", "Визначення місцезнаходження…");
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_sendRow), Escape(title).c_str());
  adw_action_row_set_subtitle(ADW_ACTION_ROW(_sendRow), Escape(subtitle).c_str());
  gtk_widget_set_sensitive(_sendRow, point.has_value() && (chooseOnMap || _fix));
  if (_liveRow) {
    const auto sharing = Live().sharingIn(_thread->peer());
    if (sharing) {
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_liveRow), Escape(tr::lng_group_call_screen_share_stop(tr::now)).c_str());
      adw_action_row_set_subtitle(ADW_ACTION_ROW(_liveRow), Escape(tr::lng_live_location(tr::now)).c_str());
      gtk_widget_add_css_class(_liveRow, "error");
    } else {
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_liveRow),
                                    Escape(Translated("Share My Live Location", "Транслировать геопозицию",
                                                      "Транслювати геопозицію")).c_str());
      adw_action_row_set_subtitle(ADW_ACTION_ROW(_liveRow),
                                  Escape(Translated("Updated in real time as you move",
                                                    "Обновляется в реальном времени, пока вы перемещаетесь",
                                                    "Оновлюється в реальному часі, поки ви рухаєтеся")).c_str());
      gtk_widget_remove_css_class(_liveRow, "error");
    }
    gtk_widget_set_sensitive(_liveRow, sharing || _fix.has_value());
  }
}

// --- places ------------------------------------------------------------------

std::optional<std::pair<double, double>> LocationPicker::venuesPoint() const {
  return selected();
}

void LocationPicker::scheduleVenues() {
  _venuesTimer.callOnce(kVenuesDelay);
}

void LocationPicker::requestVenues() {
  const auto point = venuesPoint();
  if (!point) {
    gtk_widget_set_visible(_venuesSpinner, FALSE);
    gtk_label_set_text(GTK_LABEL(_venuesStatus), U8(tr::lng_maps_choose_to_search(tr::now)).c_str());
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
  } else if (_venuesBotRequest) {
    return;
  }
  // The bot the phone apps ask about places (Foursquare's, by the server's configuration).
  const auto username = _session->serverConfig().venueSearchUsername;
  const auto weak = base::make_weak(this);
  const auto session = _session;
  _venuesBotRequest = _session->api()
                          .request(MTPcontacts_ResolveUsername(MTP_flags(0), MTP_string(username), MTP_string()))
                          .done([=](const MTPcontacts_ResolvedPeer &result) {
                            const auto &data = result.data();
                            session->data().processUsers(data.vusers());
                            session->data().processChats(data.vchats());
                            const auto strong = weak.get();
                            if (!strong) return;
                            strong->_venuesBotRequest = 0;
                            const auto peer = session->data().peerLoaded(peerFromMTP(data.vpeer()));
                            const auto user = peer ? peer->asUser() : nullptr;
                            if (user && user->isBotInlineGeo()) {
                              strong->_venuesBot = user;
                              strong->sendVenuesRequest();
                            } else {
                              strong->showVenues({}, true);
                            }
                          })
                          .fail([=] {
                            if (const auto strong = weak.get()) {
                              strong->_venuesBotRequest = 0;
                              strong->showVenues({}, true);
                            }
                          })
                          .send();
}

void LocationPicker::sendVenuesRequest() {
  if (!_venuesAt || !_venuesBot) return;
  if (_venuesRequest) _session->api().request(base::take(_venuesRequest)).cancel();
  const auto weak = base::make_weak(this);
  const auto session = _session;
  _venuesRequest =
      _session->api()
          .request(MTPmessages_GetInlineBotResults(
              MTP_flags(MTPmessages_GetInlineBotResults::Flag::f_geo_point), _venuesBot->inputUser(),
              _thread->peer()->input(),
              MTP_inputGeoPoint(MTP_flags(0), MTP_double(_venuesAt->first), MTP_double(_venuesAt->second), MTP_int(0)),
              MTP_string(_venuesQuery), MTP_string()))
          .done([=](const MTPmessages_BotResults &result) {
            const auto &data = result.data();
            session->data().processUsers(data.vusers());
            auto venues = std::vector<Data::InputVenue>();
            for (const auto &found : data.vresults().v) {
              found.match([&](const auto &data) {
                data.vsend_message().match([&](const MTPDbotInlineMessageMediaVenue &data) {
                  data.vgeo().match([&](const MTPDgeoPoint &geo) {
                    venues.push_back({
                        .lat = geo.vlat().v,
                        .lon = geo.vlong().v,
                        .title = qs(data.vtitle()),
                        .address = qs(data.vaddress()),
                        .provider = qs(data.vprovider()),
                        .id = qs(data.vvenue_id()),
                        .venueType = qs(data.vvenue_type()),
                    });
                  }, [](const auto &) {});
                }, [](const auto &) {});
              });
            }
            if (const auto strong = weak.get()) {
              strong->_venuesRequest = 0;
              strong->showVenues(std::move(venues), false);
            }
          })
          .fail([=] {
            if (const auto strong = weak.get()) {
              strong->_venuesRequest = 0;
              strong->showVenues({}, true);
            }
          })
          .send();
}

void LocationPicker::showVenues(std::vector<Data::InputVenue> venues, bool failed) {
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
    Connect<void(AdwActionRow *)>(row, "activated", crl::guard(this, [=](AdwActionRow *) { sendVenue(venue); }));
    gtk_list_box_append(GTK_LIST_BOX(_venues), row);
  }
  gtk_widget_set_visible(_venues, !venues.empty());
  gtk_label_set_text(GTK_LABEL(_venuesStatus), venues.empty() ? U8(tr::lng_maps_no_places(tr::now)).c_str() : "");
  gtk_widget_set_visible(_venuesStatus, venues.empty());
}

// --- sending -----------------------------------------------------------------

void LocationPicker::sendPoint(double lat, double lon) {
  if (!Data::CanSendTexts(_thread)) return;
  auto action = Api::SendAction(_thread);
  if (_replyTo) action.replyTo.messageId = _replyTo;
  Api::SendLocation(action, lat, lon);
  done();
}

void LocationPicker::sendVenue(const Data::InputVenue &venue) {
  if (!Data::CanSendTexts(_thread)) return;
  auto action = Api::SendAction(_thread);
  if (_replyTo) action.replyTo.messageId = _replyTo;
  Api::SendVenue(action, venue);
  done();
}

void LocationPicker::shareLive() {
  if (const auto sharing = Live().sharingIn(_thread->peer())) {
    Live().stop(sharing);
    return;
  }
  if (!_fix || !Data::CanSendTexts(_thread)) return;
  const auto weak = base::make_weak(this);
  ChooseLivePeriod(_dialog, [=](TimeId period) {
    const auto strong = weak.get();
    if (!strong || !strong->_fix) return;
    Live().start(strong->_thread, period, *strong->_fix, strong->_replyTo);
    strong->done();
  });
}

void LocationPicker::done() {
  if (_sent) _sent();
  adw_dialog_close(ADW_DIALOG(_dialog));
}

// --- a location on the map ---------------------------------------------------

#ifdef CONTRAIL_SHUMATE

class LocationViewer final : public base::has_weak_ptr {
public:
  LocationViewer(not_null<MainWindow *> window, not_null<HistoryItem *> item);
  ~LocationViewer();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }

private:
  void refresh();
  void received(const GeoFix &fix);
  void toggleMyLocation();
  [[nodiscard]] HistoryItem *item() const;

  const not_null<MainWindow *> _window;
  const not_null<Main::Session *> _session;
  const FullMsgId _id;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_map = nullptr;
  GtkWidget *_marker = nullptr;
  GtkWidget *_position = nullptr;
  GtkWidget *_details = nullptr;
  GtkWidget *_distance = nullptr;
  GtkWidget *_stop = nullptr;
  GtkWidget *_mine = nullptr;
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _fix;
  double _lat = 0.;
  double _lon = 0.;
  bool _moved = false;  // By hand: the map doesn't follow a live location then.
  base::Timer _timer;
  SignalConnections _signals;
  rpl::lifetime _lifetime;
};

LocationViewer::LocationViewer(not_null<MainWindow *> window, not_null<HistoryItem *> item)
    : _window(window), _session(&item->history()->session()), _id(item->fullId()), _timer([=] { refresh(); }) {
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
  auto *place = MapButton("contrail-map-pin-symbolic", tr::lng_maps_point(tr::now));
  _signals.clicked(place, [=] {
    _moved = false;
    MapCenterOn(_map, _lat, _lon, std::max(MapZoom(_map), kPlaceZoom), true);
  });
  gtk_box_append(GTK_BOX(buttons), place);
  _mine = MapButton("contrail-my-location-symbolic", MyLocationText());
  _signals.clicked(_mine, [=] { toggleMyLocation(); });
  gtk_box_append(GTK_BOX(buttons), _mine);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), buttons);
  MapOnUserMove(_map, [=] { _moved = true; });
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
  auto *open = gtk_button_new_with_label(U8(Translated("Open in Maps", "Открыть в Картах", "Відкрити в Картах")).c_str());
  gtk_widget_add_css_class(open, "pill");
  gtk_widget_add_css_class(open, "suggested-action");
  _signals.clicked(open, [=] { OpenInMapsApp(_lat, _lon); });
  gtk_box_append(GTK_BOX(actions), open);
  _stop = gtk_button_new_with_label(U8(tr::lng_group_call_screen_share_stop(tr::now)).c_str());
  gtk_widget_add_css_class(_stop, "pill");
  gtk_widget_add_css_class(_stop, "destructive-action");
  _signals.clicked(_stop, [=] { Live().stop(_id); });
  gtk_box_append(GTK_BOX(actions), _stop);
  gtk_box_append(GTK_BOX(bottom), actions);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), bottom);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), clamp);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);

  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [=](AdwDialog *) {
    _watch = nullptr;
    _timer.cancel();
  });
  // A live location moves as it is edited.
  _session->changes().messageUpdates(Data::MessageUpdate::Flag::Edited)
      | rpl::filter([=](const Data::MessageUpdate &update) { return update.item->fullId() == _id; })
      | rpl::on_next([=] { refresh(); }, _lifetime);
  _session->data().itemRemoved(_id) | rpl::on_next([=] { adw_dialog_close(ADW_DIALOG(_dialog)); }, _lifetime);
  Live().changes() | rpl::on_next([=] { refresh(); }, _lifetime);
  refresh();
  MapCenterOn(_map, _lat, _lon, kPlaceZoom, false);
}

LocationViewer::~LocationViewer() {
  _signals.clear();
  _watch = nullptr;
}

HistoryItem *LocationViewer::item() const {
  return _session->data().message(_id);
}

void LocationViewer::refresh() {
  const auto item = this->item();
  const auto location = item ? LocationMedia(item) : nullptr;
  if (!location) return;
  const auto live = LiveLocationActive(item);
  const auto &point = location->point();
  const auto movedFrom = std::pair(_lat, _lon);
  _lat = point.lat();
  _lon = point.lon();
  const auto from = item->displayFrom() ? item->displayFrom() : item->from().get();
  if (!_marker) {
    auto *avatar = location->livePeriod() ? MakeAvatar(from, 30) : nullptr;
    _marker = MapAddMarker(_map, MakeMapPin(avatar), _lat, _lon);
  } else if (movedFrom != std::pair(_lat, _lon)) {
    MapMoveMarker(_marker, _lat, _lon);
    if (!_moved) MapCenterOn(_map, _lat, _lon, MapZoom(_map), true);
  }
  QString title, subtitle, details;
  if (location->livePeriod()) {
    title = tr::lng_live_location(tr::now);
    subtitle = from->name();
    details = live ? LiveLocationUpdated(item) : QString();
  } else if (!location->title().isEmpty()) {
    title = location->title();
    details = location->description();
  } else {
    title = tr::lng_maps_point(tr::now);
  }
  if (details.isEmpty()) details = FormatCoordinates(_lat, _lon);
  adw_window_title_set_title(ADW_WINDOW_TITLE(_title), U8(title).c_str());
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(_title), U8(subtitle).c_str());
  gtk_label_set_text(GTK_LABEL(_details), U8(details).c_str());
  gtk_widget_set_visible(_stop, Live().sharing(_id));
  if (_fix) {
    gtk_label_set_text(GTK_LABEL(_distance),
                       U8(Translated("%1 away", "%1 от вас", "%1 від вас")
                              .arg(FormatDistance(DistanceMeters(_fix->lat, _fix->lon, _lat, _lon))))
                           .c_str());
  }
  gtk_widget_set_visible(_distance, _fix.has_value());
  if (live) _timer.callOnce(30 * crl::time(1000));
}

// The device's position is asked for only after this button: looking at a place doesn't turn the GPS on.
void LocationViewer::toggleMyLocation() {
  if (_watch) {
    if (_fix) MapCenterOn(_map, _fix->lat, _fix->lon, MapZoom(_map), true);
    return;
  }
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
        crl::on_main(strong, [=] { _watch = nullptr; });
      });
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

void ShowLocationPicker(not_null<MainWindow *> window, not_null<Data::Thread *> thread, FullMsgId replyTo,
                        Fn<void()> sent) {
  if (!window->widget()) return;
  auto picker = std::make_unique<LocationPicker>(window, thread, replyTo, std::move(sent));
  auto *dialog = picker->dialog();
  AttachData(dialog, "contrail-location-picker", std::move(picker));
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
}

void ShowLocation(not_null<MainWindow *> window, not_null<HistoryItem *> item) {
  const auto location = LocationMedia(item);
  if (!location || !window->widget()) return;
#ifdef CONTRAIL_SHUMATE
  auto viewer = std::make_unique<LocationViewer>(window, item);
  auto *dialog = viewer->dialog();
  AttachData(dialog, "contrail-location-viewer", std::move(viewer));
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
#else
  OpenInMapsApp(location->point().lat(), location->point().lon());
#endif
}

Data::MediaLocation *LocationMedia(not_null<const HistoryItem *> item) {
  return dynamic_cast<Data::MediaLocation *>(item->media());
}

bool LiveLocationActive(not_null<const HistoryItem *> item) {
  const auto location = LocationMedia(item);
  const auto period = location ? location->livePeriod() : 0;
  if (!period) return false;
  const auto now = base::unixtime::now();
  return period == LiveLocations::kUntilOff || now < item->date() || now - item->date() < period;
}

QString LiveLocationUpdated(not_null<const HistoryItem *> item) {
  const auto date = UpdateDate(item);
  const auto now = base::unixtime::now();
  const auto elapsed = now - date;
  if (elapsed < 60) {
    return tr::lng_live_location_now(tr::now);
  } else if (const auto minutes = elapsed / 60; minutes < 60) {
    return tr::lng_live_location_minutes(tr::now, lt_count, minutes);
  } else if (const auto hours = elapsed / 3600; hours < 12) {
    return tr::lng_live_location_hours(tr::now, lt_count, hours);
  }
  const auto dateFull = base::unixtime::parse(date);
  const auto nowFull = base::unixtime::parse(now);
  const auto locale = QLocale();
  const auto time = locale.toString(dateFull.time(), QLocale::ShortFormat);
  if (dateFull.date() == nowFull.date()) {
    return tr::lng_live_location_today(tr::now, lt_time, time);
  } else if (dateFull.date().addDays(1) == nowFull.date()) {
    return tr::lng_live_location_yesterday(tr::now, lt_time, time);
  }
  return tr::lng_live_location_date_time(tr::now, lt_date, locale.toString(dateFull.date(), QLocale::ShortFormat),
                                         lt_time, time);
}

QString LiveLocationRemaining(not_null<const HistoryItem *> item) {
  const auto location = LocationMedia(item);
  const auto period = location ? location->livePeriod() : 0;
  if (period == LiveLocations::kUntilOff) return QString(1, QChar(0x221E));
  const auto elapsed = base::unixtime::now() - item->date();
  const auto remaining = std::clamp(period - elapsed, 0, period);
  if (remaining < 10) {
    return tr::lng_seconds_tiny(tr::now, lt_count, remaining);
  } else if (remaining < 3600) {
    return tr::lng_minutes_tiny(tr::now, lt_count, std::max(remaining / 60, 1));
  } else if (remaining < 86400) {
    return tr::lng_hours_tiny(tr::now, lt_count, remaining / 3600);
  }
  return tr::lng_days_tiny(tr::now, lt_count, remaining / 86400);
}

double LiveLocationProgress(not_null<const HistoryItem *> item) {
  const auto location = LocationMedia(item);
  const auto period = location ? location->livePeriod() : 0;
  if (period == LiveLocations::kUntilOff) return 1.;
  if (period < 1) return 0.;
  const auto elapsed = base::unixtime::now() - item->date();
  return std::clamp(double(period - elapsed) / period, 0., 1.);
}

}  // namespace Contrail
