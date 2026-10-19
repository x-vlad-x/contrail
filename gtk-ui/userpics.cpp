#include "userpics.hpp"

#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/userpic_view.h"

#include "base/flat_set.h"

#include <rpl/lifetime.h>

#include <unordered_map>
#include <vector>

namespace Contrail {
namespace {

// The textures kept for the chats on the screen and the ones seen recently. Avatars still showing an
// evicted texture keep it until they go; the next one gets a new one from the core's cache on disk.
constexpr auto kBudget = int64(8 * 1024 * 1024);

// One texture per peer and session, plus the avatars waiting for the download (weak: an avatar that
// goes away before its picture comes leaves nothing behind).
struct Cached {
  Ui::PeerUserpicView view;  // Only while the picture is coming: the texture holds what is shown.
  PhotoId photoId = 0;
  GdkTexture *texture = nullptr;
  std::vector<GWeakRef *> waiting;
  uint64 used = 0;
};

int64 TextureBytes(GdkTexture *texture) {
  return texture ? int64(gdk_texture_get_width(texture)) * gdk_texture_get_height(texture) * 4 : 0;
}

void ClearWaiting(Cached &cached) {
  for (auto *ref : cached.waiting) {
    g_weak_ref_clear(ref);
    g_free(ref);
  }
  cached.waiting.clear();
}

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

  ~Cache() {
    _lifetime.destroy();
    for (auto &[peer, cached] : _cached) {
      setTexture(cached, nullptr);
      ClearWaiting(cached);
    }
  }

  void bind(AdwAvatar *avatar, not_null<PeerData *> peer) {
    auto &cached = _cached[peer];
    cached.used = ++_clock;
    if (!cached.texture || cached.photoId != peer->userpicPhotoId()) {
      refresh(peer, cached);
    }
    adw_avatar_set_custom_image(avatar, cached.texture ? GDK_PAINTABLE(cached.texture) : nullptr);
    if (!cached.texture && peer->hasUserpic()) {
      // Avatars already gone don't wait any longer.
      std::erase_if(cached.waiting, [](GWeakRef *ref) {
        auto *object = g_weak_ref_get(ref);
        if (object) {
          g_object_unref(object);
          return false;
        }
        g_weak_ref_clear(ref);
        g_free(ref);
        return true;
      });
      auto *ref = g_new0(GWeakRef, 1);
      g_weak_ref_init(ref, avatar);
      cached.waiting.push_back(ref);
    }
    trim();
  }

  [[nodiscard]] int64 bytes() const { return _bytes; }

private:
  void refresh(not_null<PeerData *> peer, Cached &cached) {
    if (!peer->hasUserpic()) {
      setTexture(cached, nullptr);
      cached.view = {};
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
      cached.view = {};
      flushWaiting(cached);
    }
  }

  void setTexture(Cached &cached, GdkTexture *texture) {
    _bytes -= TextureBytes(cached.texture);
    if (cached.texture) {
      g_object_unref(cached.texture);
    }
    cached.texture = texture;
    _bytes += TextureBytes(texture);
  }

  static void flushWaiting(Cached &cached) {
    for (auto *ref : cached.waiting) {
      if (auto *avatar = g_weak_ref_get(ref)) {
        adw_avatar_set_custom_image(ADW_AVATAR(avatar), cached.texture ? GDK_PAINTABLE(cached.texture) : nullptr);
        g_object_unref(avatar);
      }
    }
    ClearWaiting(cached);
  }

  void checkWaiting() {
    for (auto &[peer, cached] : _cached) {
      if (!cached.waiting.empty() && !cached.texture) {
        if (const auto image = peer->userpicCloudImage(cached.view)) {
          setTexture(cached, ToTexture(*image));
          cached.view = {};
          flushWaiting(cached);
        }
      }
    }
  }

  // Least recently shown first; the ones avatars still wait for stay.
  void trim() {
    while (_bytes > kBudget) {
      auto oldest = _cached.end();
      for (auto i = _cached.begin(); i != _cached.end(); ++i) {
        if (i->second.texture && i->second.waiting.empty() && (oldest == _cached.end() || i->second.used < oldest->second.used)) {
          oldest = i;
        }
      }
      if (oldest == _cached.end()) break;
      setTexture(oldest->second, nullptr);
      _cached.erase(oldest);
    }
  }

  std::unordered_map<PeerData *, Cached> _cached;
  int64 _bytes = 0;
  uint64 _clock = 0;
  rpl::lifetime _lifetime;
};

std::unordered_map<Main::Session *, std::unique_ptr<Cache>> &Caches() {
  static std::unordered_map<Main::Session *, std::unique_ptr<Cache>> caches;
  return caches;
}

Cache &CacheFor(not_null<Main::Session *> session) {
  // The cache is dropped with its session; once per session, however often it is released.
  static auto watched = base::flat_set<Main::Session *>();
  if (watched.emplace(session).second) {
    session->lifetime().add([=] {
      Caches().erase(session);
      watched.remove(session);
    });
  }
  auto &result = Caches()[session];
  if (!result) {
    result = std::make_unique<Cache>(session);
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

void ReleaseAvatars() {
  Caches().clear();
}

int64 AvatarCacheBytes() {
  auto result = int64();
  for (const auto &[session, cache] : Caches()) {
    result += cache->bytes();
  }
  return result;
}

}  // namespace Contrail
