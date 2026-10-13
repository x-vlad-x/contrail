#include "userpics.hpp"

#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/userpic_view.h"

#include <rpl/lifetime.h>

#include <unordered_map>
#include <vector>

namespace Contrail {
namespace {

// One cached texture per peer and session, plus the avatars waiting for the download.
struct Cached {
  Ui::PeerUserpicView view;
  PhotoId photoId = 0;
  GdkTexture *texture = nullptr;
  std::vector<GWeakRef *> waiting;
};

class Cache final {
public:
  explicit Cache(not_null<Main::Session *> session) {
    session->downloaderTaskFinished() | rpl::on_next([=] { checkWaiting(); }, _lifetime);
    session->changes().peerUpdates(Data::PeerUpdate::Flag::Photo) | rpl::on_next([=](const Data::PeerUpdate &update) {
      if (const auto i = _cached.find(update.peer); i != _cached.end()) {
        refresh(update.peer, i->second);
      }
    }, _lifetime);
  }

  void bind(AdwAvatar *avatar, not_null<PeerData *> peer) {
    auto &cached = _cached[peer];
    if (!cached.texture || cached.photoId != peer->userpicPhotoId()) {
      refresh(peer, cached);
    }
    adw_avatar_set_custom_image(avatar, cached.texture ? GDK_PAINTABLE(cached.texture) : nullptr);
    if (!cached.texture && peer->hasUserpic()) {
      auto *ref = g_new0(GWeakRef, 1);
      g_weak_ref_init(ref, avatar);
      cached.waiting.push_back(ref);
    }
  }

private:
  void refresh(not_null<PeerData *> peer, Cached &cached) {
    if (!peer->hasUserpic()) {
      setTexture(cached, nullptr);
      cached.photoId = 0;
      return;
    }
    if (cached.photoId != peer->userpicPhotoId()) {
      cached.view = peer->createUserpicView();
      cached.photoId = peer->userpicPhotoId();
    }
    peer->loadUserpic();
    if (const auto image = peer->userpicCloudImage(cached.view)) {
      setTexture(cached, ToTexture(*image));
      flushWaiting(cached);
    }
  }

  static void setTexture(Cached &cached, GdkTexture *texture) {
    if (cached.texture) {
      g_object_unref(cached.texture);
    }
    cached.texture = texture;
  }

  static void flushWaiting(Cached &cached) {
    for (auto *ref : cached.waiting) {
      if (auto *avatar = g_weak_ref_get(ref)) {
        adw_avatar_set_custom_image(ADW_AVATAR(avatar), cached.texture ? GDK_PAINTABLE(cached.texture) : nullptr);
        g_object_unref(avatar);
      }
      g_weak_ref_clear(ref);
      g_free(ref);
    }
    cached.waiting.clear();
  }

  void checkWaiting() {
    for (auto &[peer, cached] : _cached) {
      if (!cached.waiting.empty() && !cached.texture) {
        if (const auto image = peer->userpicCloudImage(cached.view)) {
          setTexture(cached, ToTexture(*image));
          flushWaiting(cached);
        }
      }
    }
  }

  std::unordered_map<PeerData *, Cached> _cached;
  rpl::lifetime _lifetime;
};

Cache &CacheFor(not_null<Main::Session *> session) {
  static std::unordered_map<Main::Session *, std::unique_ptr<Cache>> caches;
  auto &result = caches[session];
  if (!result) {
    result = std::make_unique<Cache>(session);
    session->lifetime().add([=] { caches.erase(session); });
  }
  return *result;
}

}  // namespace

void BindAvatar(AdwAvatar *avatar, not_null<PeerData *> peer, bool forSavedMessages) {
  if (forSavedMessages && peer->isSelf()) {
    adw_avatar_set_text(avatar, "");
    adw_avatar_set_icon_name(avatar, "user-bookmarks-symbolic");
    adw_avatar_set_show_initials(avatar, FALSE);
    adw_avatar_set_custom_image(avatar, nullptr);
    gtk_widget_add_css_class(GTK_WIDGET(avatar), "saved-messages");
    return;
  }
  adw_avatar_set_text(avatar, U8(peer->name()).c_str());
  adw_avatar_set_show_initials(avatar, TRUE);
  CacheFor(&peer->session()).bind(avatar, peer);
}

GtkWidget *MakeAvatar(not_null<PeerData *> peer, int size, bool forSavedMessages) {
  auto *avatar = adw_avatar_new(size, nullptr, TRUE);
  BindAvatar(ADW_AVATAR(avatar), peer, forSavedMessages);
  return avatar;
}

}  // namespace Contrail
