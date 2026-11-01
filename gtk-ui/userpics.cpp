#include "userpics.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Contrail {
namespace {

// The textures kept for the chats on the screen and the ones seen recently. Avatars still showing an evicted texture
// keep it until they go; the next one reads the file TDLib keeps on disk again.
constexpr auto kBudget = std::int64_t(8 * 1024 * 1024);
constexpr auto kFileKey = "contrail-avatar-file";

// One texture per file, plus the avatars waiting for the download (weak: an avatar that goes away before its picture
// comes leaves nothing behind).
struct Cached {
  GdkTexture *texture = nullptr;
  std::vector<GWeakRef *> waiting;
  std::uint64_t used = 0;
  bool requested = false;
  bool broken = false;  // The file isn't a picture: initials stay.
};

std::int64_t TextureBytes(GdkTexture *texture) {
  return texture ? std::int64_t(gdk_texture_get_width(texture)) * gdk_texture_get_height(texture) * 4 : 0;
}

void FreeWeak(GWeakRef *ref) {
  g_weak_ref_clear(ref);
  g_free(ref);
}

void ClearWaiting(Cached &cached) {
  std::ranges::for_each(cached.waiting, FreeWeak);
  cached.waiting.clear();
}

class Cache final {
 public:
  explicit Cache(Td::Store &store) : _store(store) {
    _store.onFile([this](const Td::File &file) { fileChanged(file); }, _lifetime);
  }

  ~Cache() {
    for (auto &[file, cached] : _cached) {
      setTexture(cached, nullptr);
      ClearWaiting(cached);
    }
  }

  [[nodiscard]] const Td::Store *store() const { return &_store; }
  [[nodiscard]] std::int64_t bytes() const { return _bytes; }

  void bind(AdwAvatar *avatar, Td::FileId file) {
    g_object_set_data(G_OBJECT(avatar), kFileKey, GINT_TO_POINTER(file));
    auto &cached = _cached[file];
    cached.used = ++_clock;
    if (const auto known = _store.file(file); !cached.texture && known && known->complete) load(cached, *known);
    adw_avatar_set_custom_image(avatar, cached.texture ? GDK_PAINTABLE(cached.texture) : nullptr);
    if (!cached.texture && !cached.broken) {
      wait(cached, avatar);
      if (!cached.requested) {
        cached.requested = true;
        _store.download(file);  // Can answer before this returns, so `cached` isn't used after it.
      }
    }
    trim();
  }

 private:
  static void wait(Cached &cached, AdwAvatar *avatar) {
    auto present = false;
    // Avatars already gone don't wait any longer.
    std::erase_if(cached.waiting, [&](GWeakRef *ref) {
      auto *object = g_weak_ref_get(ref);
      if (!object) {
        FreeWeak(ref);
        return true;
      }
      present = present || object == static_cast<gpointer>(avatar);
      g_object_unref(object);
      return false;
    });
    if (present) return;
    auto *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, avatar);
    cached.waiting.push_back(ref);
  }

  void load(Cached &cached, const Td::File &file) {
    auto *error = static_cast<GError *>(nullptr);
    auto *texture = gdk_texture_new_from_filename(file.path.c_str(), &error);
    if (error) {
      g_warning("Couldn't read a picture: %s", error->message);
      g_error_free(error);
      cached.broken = true;
    }
    setTexture(cached, texture);
  }

  void fileChanged(const Td::File &file) {
    const auto it = _cached.find(file.id);
    if (it == _cached.end() || !file.complete) return;
    auto &cached = it->second;
    if (!cached.texture) load(cached, file);
    for (auto *ref : cached.waiting) {
      auto *avatar = g_weak_ref_get(ref);
      if (!avatar) continue;
      // A recycled avatar may be showing another chat by now.
      if (cached.texture && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(avatar), kFileKey)) == file.id) {
        adw_avatar_set_custom_image(ADW_AVATAR(avatar), GDK_PAINTABLE(cached.texture));
      }
      g_object_unref(avatar);
    }
    ClearWaiting(cached);
    trim();
  }

  void setTexture(Cached &cached, GdkTexture *texture) {
    _bytes += TextureBytes(texture) - TextureBytes(cached.texture);
    if (cached.texture) g_object_unref(cached.texture);
    cached.texture = texture;
  }

  void trim() {
    while (_bytes > kBudget) {
      auto oldest = _cached.end();
      for (auto i = _cached.begin(); i != _cached.end(); ++i) {
        if (i->second.texture && (oldest == _cached.end() || i->second.used < oldest->second.used)) oldest = i;
      }
      if (oldest == _cached.end()) break;
      setTexture(oldest->second, nullptr);
      ClearWaiting(oldest->second);
      _cached.erase(oldest);
    }
  }

  Td::Store &_store;
  Td::Lifetime _lifetime;
  std::unordered_map<Td::FileId, Cached> _cached;
  std::int64_t _bytes = 0;
  std::uint64_t _clock = 0;
};

std::unique_ptr<Cache> &CurrentCache() {
  static std::unique_ptr<Cache> cache;
  return cache;
}

Cache &CacheFor(Td::Store &store) {
  auto &cache = CurrentCache();
  if (!cache || cache->store() != &store) cache = std::make_unique<Cache>(store);
  return *cache;
}

void Show(AdwAvatar *avatar, Td::Store &store, const std::string &name, Td::FileId photo) {
  gtk_widget_remove_css_class(GTK_WIDGET(avatar), "saved-messages");
  adw_avatar_set_icon_name(avatar, nullptr);
  adw_avatar_set_text(avatar, name.c_str());
  adw_avatar_set_show_initials(avatar, TRUE);
  if (photo) {
    CacheFor(store).bind(avatar, photo);
  } else {
    UnbindAvatar(avatar);
    adw_avatar_set_custom_image(avatar, nullptr);
  }
}

}  // namespace

void BindAvatar(AdwAvatar *avatar, Td::Store &store, Td::ChatId id, bool forSavedMessages) {
  const auto chat = store.chat(id);
  if (!chat) return;
  if (forSavedMessages && chat->kind == Td::ChatKind::Private && chat->peerId == store.selfId()) {
    UnbindAvatar(avatar);
    adw_avatar_set_text(avatar, "");
    adw_avatar_set_icon_name(avatar, "user-bookmarks-symbolic");
    adw_avatar_set_show_initials(avatar, FALSE);
    adw_avatar_set_custom_image(avatar, nullptr);
    gtk_widget_add_css_class(GTK_WIDGET(avatar), "saved-messages");
    return;
  }
  Show(avatar, store, chat->title, chat->photo);
}

void BindUserAvatar(AdwAvatar *avatar, Td::Store &store, Td::UserId id) {
  if (const auto user = store.user(id)) Show(avatar, store, Td::DisplayName(*user), user->photo);
}

GtkWidget *MakeAvatar(Td::Store &store, Td::ChatId chat, int size, bool forSavedMessages) {
  auto *avatar = adw_avatar_new(size, nullptr, TRUE);
  BindAvatar(ADW_AVATAR(avatar), store, chat, forSavedMessages);
  return avatar;
}

GtkWidget *MakeUserAvatar(Td::Store &store, Td::UserId user, int size) {
  auto *avatar = adw_avatar_new(size, nullptr, TRUE);
  BindUserAvatar(ADW_AVATAR(avatar), store, user);
  return avatar;
}

void UnbindAvatar(AdwAvatar *avatar) {
  g_object_set_data(G_OBJECT(avatar), kFileKey, nullptr);
}

void ReleaseAvatars() {
  CurrentCache() = nullptr;
}

std::int64_t AvatarCacheBytes() {
  const auto &cache = CurrentCache();
  return cache ? cache->bytes() : 0;
}

}  // namespace Contrail
