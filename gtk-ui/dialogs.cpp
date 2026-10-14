#include "dialogs.hpp"

#include "main_window.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/version.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_file_origin.h"
#include "data/data_peer_values.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/image/image.h"
#include "ui/text/format_values.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>

#include <rpl/lifetime.h>

#include <algorithm>

namespace Contrail {
namespace {

GtkWidget *InfoRow(const QString &title, const QString &value) {
  auto *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), U8(value).c_str());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), U8(title).c_str());
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), FALSE);
  gtk_widget_add_css_class(row, "property");
  auto *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_add_css_class(copy, "flat");
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  OnClicked(copy, [value] {
    CopyToClipboard(value);
    ShowToast(tr::lng_text_copied(tr::now));
  });
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), copy);
  return row;
}

QString PeerStatus(not_null<PeerData *> peer) {
  if (const auto user = peer->asUser()) {
    return user->isBot() ? tr::lng_status_bot(tr::now) : Data::OnlineText(user, base::unixtime::now());
  } else if (const auto chat = peer->asChat()) {
    return tr::lng_chat_status_members(tr::now, lt_count, chat->count);
  } else if (const auto channel = peer->asChannel()) {
    const auto count = channel->membersCount();
    return channel->isBroadcast() ? tr::lng_chat_status_subscribers(tr::now, lt_count, count)
                                  : tr::lng_chat_status_members(tr::now, lt_count, count);
  }
  return QString();
}

}  // namespace

void ShowProfile(not_null<MainWindow *> window, not_null<PeerData *> peer) {
  peer->updateFull();
  auto *dialog = adw_dialog_new();
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 420);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 560);
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());

  auto *page = adw_preferences_page_new();
  auto *top = adw_preferences_group_new();
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(box), MakeAvatar(peer, 96, true));
  auto *name = gtk_label_new(U8(peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name()).c_str());
  gtk_widget_add_css_class(name, "title-2");
  gtk_label_set_wrap(GTK_LABEL(name), TRUE);
  gtk_label_set_justify(GTK_LABEL(name), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(box), name);
  auto *status = gtk_label_new(U8(PeerStatus(peer)).c_str());
  gtk_widget_add_css_class(status, "dim-label");
  gtk_box_append(GTK_BOX(box), status);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(top), box);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(top));

  auto *info = adw_preferences_group_new();
  if (const auto user = peer->asUser(); user && !user->phone().isEmpty()) {
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(info),
                              InfoRow(tr::lng_info_mobile_label(tr::now), Ui::FormatPhone(user->phone())));
  }
  if (!peer->username().isEmpty()) {
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(info),
                              InfoRow(tr::lng_info_username_label(tr::now), u"@"_q + peer->username()));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(info),
                              InfoRow(tr::lng_info_link_label(tr::now), u"https://t.me/"_q + peer->username()));
  }
  if (!peer->about().isEmpty()) {
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(info),
                              InfoRow(peer->isUser() ? tr::lng_info_bio_label(tr::now) : tr::lng_info_about_label(tr::now),
                                      peer->about()));
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(info));

  auto *actions = adw_preferences_group_new();
  auto *send = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(send), U8(tr::lng_profile_send_message(tr::now)).c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(send), "chat-message-new-symbolic");
  Connect<void(AdwButtonRow *)>(send, "activated", [=](AdwButtonRow *) {
    adw_dialog_close(ADW_DIALOG(dialog));
    window->openPeer(peer);
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), send);
  if (const auto user = peer->asUser(); user && !user->isSelf() && !user->isBot()) {
    for (const auto video : {false, true}) {
      auto *call = adw_button_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(call),
          U8(video ? tr::lng_call_start_video(tr::now) : tr::lng_call_start(tr::now)).c_str());
      adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(call),
          video ? "camera-video-symbolic" : "call-start-symbolic");
      Connect<void(AdwButtonRow *)>(call, "activated", [=](AdwButtonRow *) {
        adw_dialog_close(ADW_DIALOG(dialog));
        window->startCall(peer, video);
      });
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), call);
    }
  }
  if (!peer->isSelf()) {
    const auto history = peer->owner().history(peer);
    auto *mute = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mute), U8(tr::lng_context_mute(tr::now)).c_str());
    adw_switch_row_set_active(ADW_SWITCH_ROW(mute), history->muted());
    Connect<void(GObject *, GParamSpec *)>(mute, "notify::active", [=](GObject *row, GParamSpec *) {
      auto value = Data::MuteValue();
      if (adw_switch_row_get_active(ADW_SWITCH_ROW(row))) {
        value.forever = true;
      } else {
        value.unmute = true;
      }
      peer->owner().notifySettings().update(history, value);
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), mute);
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(actions));

  // The bio and the counters come with the full info.
  auto *lifetime = new rpl::lifetime();
  peer->session().changes().peerUpdates(peer, Data::PeerUpdate::Flag::About | Data::PeerUpdate::Flag::Members)
      | rpl::on_next([=] { gtk_label_set_text(GTK_LABEL(status), U8(PeerStatus(peer)).c_str()); }, *lifetime);
  Connect<void(AdwDialog *)>(dialog, "closed", [=](AdwDialog *) { delete lifetime; });

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
  adw_dialog_set_child(ADW_DIALOG(dialog), view);
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
}

void ShowPhoto(not_null<MainWindow *> window, not_null<PhotoData *> photo, FullMsgId origin) {
  auto media = photo->createMediaView();
  media->wanted(Data::PhotoSize::Large, origin);

  auto *dialog = adw_dialog_new();
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 1200);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 1200);
  adw_dialog_set_presentation_mode(ADW_DIALOG(dialog), ADW_DIALOG_FLOATING);
  auto *view = adw_toolbar_view_new();
  gtk_widget_add_css_class(view, "photo-viewer");
  adw_toolbar_view_set_extend_content_to_top_edge(ADW_TOOLBAR_VIEW(view), TRUE);
  auto *header = adw_header_bar_new();
  gtk_widget_add_css_class(header, "osd");
  auto *save = MakeIconButton("document-save-symbolic", U8(tr::lng_mediaview_save_as(tr::now)).c_str());
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), save);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  auto *picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_hexpand(picture, TRUE);
  gtk_widget_set_vexpand(picture, TRUE);
  // Pinch or double tap to zoom: the picture grows inside a scrolled window.
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), picture);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
  adw_dialog_set_child(ADW_DIALOG(dialog), view);
  struct Zoom {
    double scale = 1.;
    double start = 1.;
  };
  auto *zoom = AttachData(picture, "contrail-zoom", std::make_unique<Zoom>());
  const auto applyZoom = [=](double scale) {
    zoom->scale = std::clamp(scale, 1., 6.);
    const auto w = gtk_widget_get_width(scroll), h = gtk_widget_get_height(scroll);
    if (zoom->scale <= 1.01) {
      gtk_widget_set_size_request(picture, -1, -1);
    } else {
      gtk_widget_set_size_request(picture, int(w * zoom->scale), int(h * zoom->scale));
    }
  };
  auto *pinch = gtk_gesture_zoom_new();
  Connect<void(GtkGesture *, GdkEventSequence *)>(pinch, "begin", [=](GtkGesture *, GdkEventSequence *) {
    zoom->start = zoom->scale;
  });
  Connect<void(GtkGestureZoom *, double)>(pinch, "scale-changed", [=](GtkGestureZoom *, double scale) {
    applyZoom(zoom->start * scale);
  });
  gtk_widget_add_controller(scroll, GTK_EVENT_CONTROLLER(pinch));

  const auto show = [=] {
    auto *image = media->image(Data::PhotoSize::Large);
    if (!image) {
      image = media->image(Data::PhotoSize::Thumbnail);
    }
    if (!image) {
      image = media->thumbnailInline();
    }
    if (image) {
      if (auto *texture = ToTexture(image->original())) {
        gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
        g_object_unref(texture);
      }
    }
    return media->image(Data::PhotoSize::Large) != nullptr;
  };
  auto *lifetime = new rpl::lifetime();
  if (!show()) {
    photo->session().downloaderTaskFinished() | rpl::on_next([=] { show(); }, *lifetime);
  }
  OnClicked(save, [=] {
    if (const auto image = media->image(Data::PhotoSize::Large)) {
      auto dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
      QDir().mkpath(dir);
      const auto path = dir + u"/photo_"_q + QDateTime::currentDateTime().toString(u"yyyy-MM-dd_HH-mm-ss"_q) + u".jpg"_q;
      image->original().save(path, "JPG");
      ShowToast(path);
    }
  });
  // A double tap zooms in and out.
  auto *click = gtk_gesture_click_new();
  Connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int count, double, double) {
    if (count == 2) {
      applyZoom(zoom->scale > 1.01 ? 1. : 2.5);
    }
  });
  gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click));
  Connect<void(AdwDialog *)>(dialog, "closed", [=](AdwDialog *) { delete lifetime; });
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
}

void ShowAbout(not_null<MainWindow *> window) {
  auto *about = adw_about_dialog_new();
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Contrail");
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), kAppId);
  adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Vladislav Shadiuk");
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), AppVersionStr);
  adw_about_dialog_set_comments(
      ADW_ABOUT_DIALOG(about),
      "An unofficial GNOME interface for Telegram, built on the Telegram Desktop core. "
      "Not affiliated with or endorsed by Telegram.");
  adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), kProjectUrl);
  adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about), (std::string(kProjectUrl) + "/issues").c_str());
  adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_credit_section(ADW_ABOUT_DIALOG(about), "Telegram Desktop",
                                      (const char *[]){"Telegram Desktop authors https://github.com/telegramdesktop/tdesktop", nullptr});
  adw_dialog_present(ADW_DIALOG(about), window->widget());
}

void ConfirmDeleteChat(not_null<MainWindow *> window, not_null<PeerData *> peer) {
  const auto channel = peer->asChannel();
  const auto chat = peer->asChat();
  QString text, button;
  if (channel) {
    text = channel->isBroadcast() ? tr::lng_sure_leave_channel(tr::now) : tr::lng_sure_leave_group(tr::now);
    button = tr::lng_box_leave(tr::now);
  } else if (chat) {
    text = tr::lng_sure_delete_and_exit(tr::now, lt_group, chat->name());
    button = tr::lng_box_delete(tr::now);
  } else if (peer->isSelf()) {
    text = tr::lng_sure_delete_saved_messages(tr::now);
    button = tr::lng_box_delete(tr::now);
  } else {
    text = tr::lng_sure_delete_history(tr::now, lt_contact, peer->name());
    button = tr::lng_box_delete(tr::now);
  }
  auto *dialog = adw_alert_dialog_new(nullptr, U8(text.replace('\n', ' ')).c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "delete",
                                 U8(button).c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  GtkWidget *revoke = nullptr;
  if (const auto user = peer->asUser(); user && !user->isSelf() && !user->isBot()) {
    revoke = gtk_check_button_new_with_label(
        U8(tr::lng_delete_for_other_check(tr::now, lt_user, user->firstName.isEmpty() ? user->name() : user->firstName))
            .c_str());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), revoke);
  }
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "delete") != 0) {
      return;
    }
    if (window->currentHistory() && window->currentHistory()->peer == peer) {
      window->closeChat();
    }
    if (channel) {
      peer->session().api().leaveChannel(channel);
    } else {
      const auto all = revoke && gtk_check_button_get_active(GTK_CHECK_BUTTON(revoke));
      peer->session().api().deleteConversation(peer, all);
    }
  });
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
}

AdwNavigationPage *MakeContactsPage(not_null<Main::Session *> session, not_null<MainWindow *> window) {
  session->api().requestContacts();
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  auto *list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "navigation-sidebar");
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);

  const auto fill = [=] {
    ClearBox(list);
    for (const auto &row : session->data().contactsList()->all()) {
      const auto history = row->history();
      if (!history) {
        continue;
      }
      const auto peer = history->peer;
      auto *item = gtk_list_box_row_new();
      g_object_set_data(G_OBJECT(item), "contrail-peer", peer.get());
      auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_widget_set_margin_top(box, 6);
      gtk_widget_set_margin_bottom(box, 6);
      gtk_box_append(GTK_BOX(box), MakeAvatar(peer, 40));
      auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
      auto *name = gtk_label_new(U8(peer->name()).c_str());
      gtk_label_set_xalign(GTK_LABEL(name), 0);
      gtk_widget_add_css_class(name, "chat-name");
      gtk_box_append(GTK_BOX(column), name);
      if (const auto user = peer->asUser()) {
        auto *status = gtk_label_new(U8(Data::OnlineText(user, base::unixtime::now())).c_str());
        gtk_label_set_xalign(GTK_LABEL(status), 0);
        gtk_widget_add_css_class(status, "dim-label");
        gtk_widget_add_css_class(status, "caption");
        gtk_box_append(GTK_BOX(column), status);
      }
      gtk_box_append(GTK_BOX(box), column);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(item), box);
      gtk_list_box_append(GTK_LIST_BOX(list), item);
    }
  };
  fill();
  Connect<void(GtkListBox *, GtkListBoxRow *)>(list, "row-activated", [=](GtkListBox *, GtkListBoxRow *row) {
    if (auto *peer = static_cast<PeerData *>(g_object_get_data(G_OBJECT(row), "contrail-peer"))) {
      window->openPeer(peer);
    }
  });

  auto *page = adw_navigation_page_new(view, U8(tr::lng_contacts_header(tr::now)).c_str());
  auto *lifetime = AttachData(page, "contrail-lifetime", std::make_unique<rpl::lifetime>());
  session->data().contactsLoaded().changes() | rpl::on_next([=] { fill(); }, *lifetime);
  return page;
}

}  // namespace Contrail
