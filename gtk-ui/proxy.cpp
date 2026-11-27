#include "proxy.hpp"

#include "main_window.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace Contrail {
namespace {

namespace api = Td::api;

enum TypeIndex : guint { kSocks5 = 0, kHttp = 1, kMtproto = 2 };

[[nodiscard]] api::object_ptr<api::ProxyType> CopyType(const api::ProxyType *type) {
  if (!type) return nullptr;
  switch (type->get_id()) {
    case api::proxyTypeSocks5::ID: {
      const auto &socks = static_cast<const api::proxyTypeSocks5 &>(*type);
      return api::make_object<api::proxyTypeSocks5>(socks.username_, socks.password_);
    }
    case api::proxyTypeHttp::ID: {
      const auto &http = static_cast<const api::proxyTypeHttp &>(*type);
      return api::make_object<api::proxyTypeHttp>(http.username_, http.password_, http.http_only_);
    }
    case api::proxyTypeMtproto::ID:
      return api::make_object<api::proxyTypeMtproto>(static_cast<const api::proxyTypeMtproto &>(*type).secret_);
    case api::proxyTypeWeb::ID: {
      const auto &web = static_cast<const api::proxyTypeWeb &>(*type);
      return api::make_object<api::proxyTypeWeb>(web.path_, web.secret_);
    }
    default: return nullptr;
  }
}

[[nodiscard]] api::object_ptr<api::proxy> CopyProxy(const api::proxy &proxy) {
  return api::make_object<api::proxy>(proxy.server_, proxy.port_, CopyType(proxy.type_.get()));
}

[[nodiscard]] std::string Address(const api::proxy &proxy) {
  return proxy.server_ + ":" + std::to_string(proxy.port_);
}

[[nodiscard]] GtkWidget *EntryRow(GtkWidget *group, const std::string &title) {
  auto *row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  return row;
}

[[nodiscard]] std::string Text(GtkWidget *editable) {
  const auto *text = gtk_editable_get_text(GTK_EDITABLE(editable));
  auto result = std::string(text ? text : "");
  // Pasted with spaces around it, as often.
  const auto first = result.find_first_not_of(" \t\n");
  const auto last = result.find_last_not_of(" \t\n");
  return first == std::string::npos ? std::string() : result.substr(first, last - first + 1);
}

}  // namespace

std::string ProxyTypeName(const api::ProxyType *type) {
  switch (type ? type->get_id() : 0) {
    case api::proxyTypeSocks5::ID: return "SOCKS5";
    case api::proxyTypeHttp::ID: return "HTTP";
    case api::proxyTypeMtproto::ID: return "MTProto";
    case api::proxyTypeWeb::ID: return "Web";
    default: return {};
  }
}

// --- the dialog of one proxy ----------------------------------------------------

class ProxyView::Editor final {
 public:
  using Save = std::function<void(api::object_ptr<api::proxy> proxy)>;

  Editor(GtkWidget *parent, const api::proxy *existing, Save onSave, std::function<void()> onClosed)
      : _save(std::move(onSave)), _closed(std::move(onClosed)) {
    _dialog = adw_dialog_new();
    g_object_ref_sink(_dialog);
    adw_dialog_set_title(_dialog, Tr(existing ? "lng_proxy_edit" : "lng_proxy_add").c_str());
    adw_dialog_set_content_width(_dialog, 400);
    auto *view = adw_toolbar_view_new();
    auto *header = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    auto *cancel = gtk_button_new_with_label(Tr("lng_cancel").c_str());
    _connections.clicked(cancel, [this] { adw_dialog_close(_dialog); });
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    _saveButton = gtk_button_new_with_label(Tr("lng_settings_save").c_str());
    gtk_widget_add_css_class(_saveButton, "suggested-action");
    _connections.clicked(_saveButton, [this] { save(); });
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), _saveButton);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    auto *page = adw_preferences_page_new();
    auto *group = adw_preferences_group_new();
    _type = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_type), Translated("Type", "Тип", "Тип").c_str());
    const char *types[] = {"SOCKS5", "HTTP", "MTProto", nullptr};
    auto *model = gtk_string_list_new(types);
    adw_combo_row_set_model(ADW_COMBO_ROW(_type), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _type);
    _server = EntryRow(group, Tr("lng_proxy_box_server"));
    _port = EntryRow(group, Tr("lng_proxy_box_port"));
    gtk_editable_set_text(GTK_EDITABLE(_port), "1080");
    adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_port), GTK_INPUT_PURPOSE_DIGITS);
    _username = EntryRow(group, Tr("lng_proxy_box_username"));
    _password = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_password), Tr("lng_proxy_box_password").c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _password);
    _secret = EntryRow(group, Tr("lng_proxy_box_secret"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(_dialog, view);

    if (existing) fill(*existing);
    _connections.connect<void(GObject *, GParamSpec *)>(_type, "notify::selected", [this](GObject *, GParamSpec *) { sync(); });
    for (auto *entry : {_server, _port, _secret}) {
      _connections.connect<void(GtkEditable *)>(entry, "changed", [this](GtkEditable *) { sync(); });
    }
    _connections.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) { _closed(); });
    sync();
    adw_dialog_present(_dialog, parent);
  }

  ~Editor() {
    _connections.clear();
    adw_dialog_force_close(_dialog);
    g_object_unref(_dialog);
  }

  // "TYPE SERVER PORT [SECRET | USER PASSWORD]", as typed into the fields; then Save.
  void debugFill(const std::string &arguments) {
    auto stream = std::istringstream(arguments);
    auto type = std::string(), server = std::string(), port = std::string(), first = std::string(), second = std::string();
    stream >> type >> server >> port >> first >> second;
    adw_combo_row_set_selected(ADW_COMBO_ROW(_type), type == "mtproto" ? kMtproto : type == "http" ? kHttp : kSocks5);
    gtk_editable_set_text(GTK_EDITABLE(_server), server.c_str());
    gtk_editable_set_text(GTK_EDITABLE(_port), port.c_str());
    if (type == "mtproto") {
      gtk_editable_set_text(GTK_EDITABLE(_secret), first.c_str());
    } else {
      gtk_editable_set_text(GTK_EDITABLE(_username), first.c_str());
      gtk_editable_set_text(GTK_EDITABLE(_password), second.c_str());
    }
    g_message("proxy: dialog save=%d", int(gtk_widget_get_sensitive(_saveButton)));
    if (gtk_widget_get_sensitive(_saveButton)) save();
  }

 private:
  void fill(const api::proxy &proxy) {
    gtk_editable_set_text(GTK_EDITABLE(_server), proxy.server_.c_str());
    gtk_editable_set_text(GTK_EDITABLE(_port), std::to_string(proxy.port_).c_str());
    switch (proxy.type_ ? proxy.type_->get_id() : 0) {
      case api::proxyTypeSocks5::ID: {
        const auto &socks = static_cast<const api::proxyTypeSocks5 &>(*proxy.type_);
        adw_combo_row_set_selected(ADW_COMBO_ROW(_type), kSocks5);
        gtk_editable_set_text(GTK_EDITABLE(_username), socks.username_.c_str());
        gtk_editable_set_text(GTK_EDITABLE(_password), socks.password_.c_str());
        break;
      }
      case api::proxyTypeHttp::ID: {
        const auto &http = static_cast<const api::proxyTypeHttp &>(*proxy.type_);
        adw_combo_row_set_selected(ADW_COMBO_ROW(_type), kHttp);
        gtk_editable_set_text(GTK_EDITABLE(_username), http.username_.c_str());
        gtk_editable_set_text(GTK_EDITABLE(_password), http.password_.c_str());
        break;
      }
      case api::proxyTypeMtproto::ID:
        adw_combo_row_set_selected(ADW_COMBO_ROW(_type), kMtproto);
        gtk_editable_set_text(GTK_EDITABLE(_secret),
                              static_cast<const api::proxyTypeMtproto &>(*proxy.type_).secret_.c_str());
        break;
      default: break;
    }
  }

  // MTProto has a secret, the others a user name and a password, both optional.
  void sync() {
    const auto mtproto = adw_combo_row_get_selected(ADW_COMBO_ROW(_type)) == kMtproto;
    gtk_widget_set_visible(_username, !mtproto);
    gtk_widget_set_visible(_password, !mtproto);
    gtk_widget_set_visible(_secret, mtproto);
    gtk_widget_set_sensitive(_saveButton, valid());
  }

  [[nodiscard]] int port() const {
    const auto text = Text(_port);
    if (text.empty() || text.size() > 5 || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
      return 0;
    }
    const auto value = std::atoi(text.c_str());
    return value > 0 && value < 65536 ? value : 0;
  }

  [[nodiscard]] bool valid() const {
    const auto mtproto = adw_combo_row_get_selected(ADW_COMBO_ROW(_type)) == kMtproto;
    return !Text(_server).empty() && port() && (!mtproto || !Text(_secret).empty());
  }

  void save() {
    if (!valid()) return;
    auto type = api::object_ptr<api::ProxyType>();
    switch (adw_combo_row_get_selected(ADW_COMBO_ROW(_type))) {
      case kMtproto: type = api::make_object<api::proxyTypeMtproto>(Text(_secret)); break;
      case kHttp: type = api::make_object<api::proxyTypeHttp>(Text(_username), Text(_password), false); break;
      default: type = api::make_object<api::proxyTypeSocks5>(Text(_username), Text(_password)); break;
    }
    _save(api::make_object<api::proxy>(Text(_server), port(), std::move(type)));
    adw_dialog_close(_dialog);
  }

  Save _save;
  std::function<void()> _closed;
  SignalConnections _connections;
  AdwDialog *_dialog = nullptr;
  GtkWidget *_type = nullptr;
  GtkWidget *_server = nullptr;
  GtkWidget *_port = nullptr;
  GtkWidget *_username = nullptr;
  GtkWidget *_password = nullptr;
  GtkWidget *_secret = nullptr;
  GtkWidget *_saveButton = nullptr;
};

// --- the page -------------------------------------------------------------------

ProxyView::ProxyView(Td::Store &store, MainWindow &window) : _store(store), _window(window) {
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  auto *prefs = adw_preferences_page_new();
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), prefs);

  auto *top = adw_preferences_group_new();
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(top), Tr("lng_proxy_about").c_str());
  _use = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_use), Tr("lng_proxy_use_custom").c_str());
  Connect<void(GObject *, GParamSpec *)>(_use, "notify::active", [this](GObject *row, GParamSpec *) {
    if (!_syncing) useChanged(adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(top), _use);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(top));

  _group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(_group), Tr("lng_proxy_box_table_title").c_str());
  auto *add = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), Tr("lng_proxy_add").c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(add), "list-add-symbolic");
  Connect<void(AdwButtonRow *)>(add, "activated", [this](AdwButtonRow *) { edit(nullptr); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(_group), add);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(_group));

  _page = adw_navigation_page_new(view, Tr("lng_proxy_settings").c_str());
  adw_navigation_page_set_tag(_page, "proxy");
  g_object_ref_sink(_page);
  _store.onConnection([this] { refreshStatus(); }, _lifetime);
  reload();
}

ProxyView::~ProxyView() {
  if (_editorSource) g_source_remove(_editorSource);
  _editor = nullptr;
  g_object_unref(_page);
}

void ProxyView::reload() {
  _store.client().request<api::addedProxies>(api::make_object<api::getProxies>(), _lifetime,
                                             [this](api::addedProxies &proxies) { loaded(proxies); });
}

void ProxyView::loaded(api::addedProxies &proxies) {
  auto old = std::move(_entries);
  _entries.clear();
  for (auto &added : proxies.proxies_) {
    if (!added || !added->proxy_) continue;
    auto entry = Entry{.id = added->id_, .proxy = std::move(added->proxy_), .enabled = added->is_enabled_,
                       .lastUsed = added->last_used_date_};
    const auto before = std::ranges::find(old, entry.id, &Entry::id);
    if (before != old.end() && Address(*before->proxy) == Address(*entry.proxy)) entry.ping = before->ping;
    _entries.push_back(std::move(entry));
  }
  rebuild();
  for (const auto &entry : _entries) {
    if (entry.ping.empty()) ping(entry.id);
  }
}

void ProxyView::rebuild() {
  for (auto *row : _rows) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(_group), row);
  _rows.clear();
  GtkCheckButton *radios = nullptr;
  for (const auto &entry : _entries) {
    auto *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Address(*entry.proxy).c_str());
    // Which one is used, as a choice among them: a tap on a row uses it.
    auto *radio = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), entry.enabled);
    if (radios) gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), radios);
    else radios = GTK_CHECK_BUTTON(radio);
    gtk_widget_set_can_target(radio, FALSE);
    gtk_widget_set_valign(radio, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), radio);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    const auto id = entry.id;
    Connect<void(AdwActionRow *)>(row, "activated", [this, id](AdwActionRow *) { enable(id); });
    auto *editButton = MakeIconButton("document-edit-symbolic", Tr("lng_proxy_menu_edit").c_str(), "flat");
    gtk_widget_set_valign(editButton, GTK_ALIGN_CENTER);
    OnClicked(editButton, [this, id] {
      const auto it = std::ranges::find(_entries, id, &Entry::id);
      if (it != _entries.end()) edit(&*it);
    });
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), editButton);
    auto *removeButton = MakeIconButton("user-trash-symbolic", Tr("lng_proxy_menu_delete").c_str(), "flat");
    gtk_widget_set_valign(removeButton, GTK_ALIGN_CENTER);
    OnClicked(removeButton, [this, id] { remove(id); });
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), removeButton);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(_group), row);
    _rows.push_back(row);
  }
  refreshStatus();
}

std::string ProxyView::status(const Entry &entry) const {
  if (entry.enabled) {
    const auto *state = _store.connection();
    const auto id = state ? state->get_id() : 0;
    if (id == api::connectionStateReady::ID || id == api::connectionStateUpdating::ID) return Tr("lng_proxy_online");
    if (id) return Tr("lng_proxy_connecting");
  }
  return entry.ping.empty() ? Tr("lng_proxy_checking") : entry.ping;
}

void ProxyView::refreshStatus() {
  auto any = false;
  for (std::size_t i = 0; i != _entries.size() && i != _rows.size(); ++i) {
    const auto &entry = _entries[i];
    any = any || entry.enabled;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(_rows[i]),
                                (ProxyTypeName(entry.proxy->type_.get()) + " · " + status(entry)).c_str());
  }
  _syncing = true;
  adw_switch_row_set_active(ADW_SWITCH_ROW(_use), any);
  _syncing = false;
}

void ProxyView::ping(std::int32_t id) {
  const auto it = std::ranges::find(_entries, id, &Entry::id);
  if (it == _entries.end()) return;
  const auto address = Address(*it->proxy);
  const auto done = [this, id, address](std::string text) {
    const auto it = std::ranges::find(_entries, id, &Entry::id);
    if (it == _entries.end() || Address(*it->proxy) != address) return;
    it->ping = std::move(text);
    refreshStatus();
  };
  _store.client().request<api::seconds>(
      api::make_object<api::pingProxy>(CopyProxy(*it->proxy)), _lifetime,
      [done](api::seconds &seconds) {
        done(Tr("lng_proxy_available", {{"ping", std::to_string(std::max(1, int(seconds.seconds_ * 1000 + 0.5)))}}));
      },
      [done](api::error &) { done(Tr("lng_proxy_unavailable")); });
}

void ProxyView::enable(std::int32_t id) {
  _store.client().request<api::ok>(api::make_object<api::enableProxy>(id), _lifetime, [this](api::ok &) { reload(); },
                                   [](api::error &error) { ShowToast(error.message_); });
}

void ProxyView::remove(std::int32_t id) {
  _store.client().request<api::ok>(api::make_object<api::removeProxy>(id), _lifetime, [this](api::ok &) { reload(); },
                                   [](api::error &error) { ShowToast(error.message_); });
}

// On: the one used last (or the first); with none in the list, the dialog of a new one.
void ProxyView::useChanged(bool use) {
  if (!use) {
    _store.client().request<api::ok>(api::make_object<api::disableProxy>(), _lifetime, [this](api::ok &) { reload(); });
    return;
  }
  if (_entries.empty()) {
    refreshStatus();
    edit(nullptr);
    return;
  }
  const auto last = std::ranges::max_element(_entries, {}, &Entry::lastUsed);
  enable(last->id);
}

void ProxyView::edit(const Entry *entry) {
  if (_editor || !_window.widget()) return;
  const auto id = entry ? entry->id : 0;
  _editor = std::make_unique<Editor>(
      _window.widget(), entry ? entry->proxy.get() : nullptr,
      [this, id](api::object_ptr<api::proxy> proxy) {
        const auto failed = [](api::error &error) { ShowToast(error.message_); };
        const auto added = [this](api::addedProxy &) { reload(); };
        if (id) {
          _store.client().request<api::addedProxy>(api::make_object<api::editProxy>(id, std::move(proxy), true, ""),
                                                   _lifetime, added, failed);
        } else {
          _store.client().request<api::addedProxy>(api::make_object<api::addProxy>(std::move(proxy), true, ""),
                                                   _lifetime, added, failed);
        }
      },
      [this] { editorClosed(); });
}

// Not from inside the dialog's own signal.
void ProxyView::editorClosed() {
  if (_editorSource) return;
  _editorSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<ProxyView *>(data);
    self->_editorSource = 0;
    self->_editor = nullptr;
  }, this);
}

void ProxyView::debugState() const {
  auto line = std::string("proxy: use=") + (adw_switch_row_get_active(ADW_SWITCH_ROW(_use)) ? "1" : "0");
  for (std::size_t i = 0; i != _entries.size(); ++i) {
    const auto &entry = _entries[i];
    line += " [" + std::to_string(i) + " " + ProxyTypeName(entry.proxy->type_.get()) + " " + Address(*entry.proxy)
        + (entry.enabled ? " enabled" : "") + " status=\"" + status(entry) + "\"]";
  }
  g_message("%s", line.c_str());
}

void ProxyView::debugAdd(const std::string &arguments, int edit) {
  if (edit >= 0 && std::size_t(edit) < _entries.size()) this->edit(&_entries[std::size_t(edit)]);
  else this->edit(nullptr);
  if (_editor) _editor->debugFill(arguments);
}

void ProxyView::debugUse(bool use) {
  adw_switch_row_set_active(ADW_SWITCH_ROW(_use), use);
}

void ProxyView::debugEnable(int index) {
  if (index >= 0 && std::size_t(index) < _entries.size()) enable(_entries[std::size_t(index)].id);
}

void ProxyView::debugRemove(int index) {
  if (index >= 0 && std::size_t(index) < _entries.size()) remove(_entries[std::size_t(index)].id);
}

void ConfirmAddProxy(MainWindow &window, api::object_ptr<api::proxy> proxy) {
  if (!proxy || !window.widget() || !window.store()) return;
  const auto body = Address(*proxy) + " (" + ProxyTypeName(proxy->type_.get()) + ")";
  auto *dialog = adw_alert_dialog_new(Tr("lng_proxy_box_title").c_str(), body.c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "connect",
                                 Tr("lng_proxy_box_table_button").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "connect", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  auto shared = std::make_shared<api::object_ptr<api::proxy>>(std::move(proxy));
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [shared](AdwAlertDialog *, const char *response) {
    auto *window = GetApp().window;
    if (g_strcmp0(response, "connect") != 0 || !window || !window->store() || !*shared) return;
    window->store()->client().send(api::make_object<api::addProxy>(std::move(*shared), true, ""));
  });
  adw_dialog_present(ADW_DIALOG(dialog), window.widget());
}

}  // namespace Contrail
