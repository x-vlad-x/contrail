#include "userpics.hpp"

#include "device_scale.hpp"
#include "picture_decode.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Contrail {
namespace {

// The textures kept for the chats on the screen and the ones seen recently. Avatars still showing an evicted texture
// keep it until they go; the next one reads the file TDLib keeps on disk again. Each texture is as large as its avatar
// on the screen (84 px for the chat list on the FLX1s, against the file's 160): about 140 of them fit.
constexpr auto kBudget = std::int64_t(3 * 1024 * 1024);
constexpr auto kFileKey = "contrail-avatar-file";

// The picture of one file at one size in device pixels.
struct Sized {
  int pixels = 0;
  GdkTexture *texture = nullptr;
  std::uint64_t used = 0;
};

// The textures of one file, plus the avatars waiting for the download (weak: an avatar that goes away before its
// picture comes leaves nothing behind).
struct Cached {
  std::vector<Sized> sizes;
  std::vector<GWeakRef *> waiting;
  bool requested = false;
  bool broken = false;  // The file isn't a picture: initials stay.
};

std::int64_t TextureBytes(GdkTexture *texture) {
  if (!texture) return 0;
  const auto bytes = (gdk_texture_get_format(texture) == GDK_MEMORY_R8G8B8) ? 3 : 4;
  return std::int64_t(gdk_texture_get_width(texture)) * gdk_texture_get_height(texture) * bytes;
}

[[nodiscard]] int AvatarPixels(AdwAvatar *avatar) {
  return DevicePixels(GTK_WIDGET(avatar), adw_avatar_get_size(avatar));
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
      for (auto &sized : cached.sizes) g_clear_object(&sized.texture);
      ClearWaiting(cached);
    }
  }

  [[nodiscard]] const Td::Store *store() const { return &_store; }
  [[nodiscard]] std::int64_t bytes() const { return _bytes; }

  void bind(AdwAvatar *avatar, Td::FileId file) {
    g_object_set_data(G_OBJECT(avatar), kFileKey, GINT_TO_POINTER(file));
    auto &cached = _cached[file];
    const auto known = _store.file(file);
    auto *texture = (known && known->complete) ? textureFor(cached, *known, AvatarPixels(avatar)) : nullptr;
    adw_avatar_set_custom_image(avatar, texture ? GDK_PAINTABLE(texture) : nullptr);
    if (!texture && !cached.broken) {
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

  // The file's texture at that size, read from the downloaded file if it isn't kept.
  GdkTexture *textureFor(Cached &cached, const Td::File &file, int pixels) {
    if (cached.broken) return nullptr;
    for (auto &sized : cached.sizes) {
      if (sized.pixels == pixels) {
        sized.used = ++_clock;
        return sized.texture;
      }
    }
    auto *texture = load(file.path, pixels);
    if (!texture) {
      cached.broken = true;
      return nullptr;
    }
    cached.sizes.push_back({pixels, texture, ++_clock});
    _bytes += TextureBytes(texture);
    return texture;
  }

  // Small enough to read right here, before the avatar is drawn: the chat list shows no initials for a moment.
  static GdkTexture *load(const std::string &path, int pixels) {
    if (auto *pixbuf = DecodePictureFile(path.c_str(), pixels, pixels, false)) return TextureFromPixbuf(pixbuf);
    // Neither JPEG nor PNG: GTK reads the rest, whole.
    auto *error = static_cast<GError *>(nullptr);
    auto *texture = gdk_texture_new_from_filename(path.c_str(), &error);
    if (error) {
      g_warning("Couldn't read a picture: %s", error->message);
      g_error_free(error);
    }
    return texture;
  }

  void fileChanged(const Td::File &file) {
    const auto it = _cached.find(file.id);
    if (it == _cached.end() || !file.complete) return;
    auto &cached = it->second;
    for (auto *ref : cached.waiting) {
      auto *avatar = g_weak_ref_get(ref);
      if (!avatar) continue;
      // A recycled avatar may be showing another chat by now.
      if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(avatar), kFileKey)) == file.id) {
        if (auto *texture = textureFor(cached, file, AvatarPixels(ADW_AVATAR(avatar)))) {
          adw_avatar_set_custom_image(ADW_AVATAR(avatar), GDK_PAINTABLE(texture));
        }
      }
      g_object_unref(avatar);
    }
    ClearWaiting(cached);
    trim();
  }

  // The textures used longest ago go first; a file left with none is forgotten.
  void trim() {
    while (_bytes > kBudget) {
      auto oldest = _cached.end();
      auto oldestSize = std::size_t(0);
      for (auto i = _cached.begin(); i != _cached.end(); ++i) {
        for (auto k = std::size_t(0); k < i->second.sizes.size(); ++k) {
          if (oldest == _cached.end() || i->second.sizes[k].used < oldest->second.sizes[oldestSize].used) {
            oldest = i;
            oldestSize = k;
          }
        }
      }
      if (oldest == _cached.end()) break;
      auto &sizes = oldest->second.sizes;
      _bytes -= TextureBytes(sizes[oldestSize].texture);
      g_object_unref(sizes[oldestSize].texture);
      sizes.erase(sizes.begin() + std::ptrdiff_t(oldestSize));
      if (sizes.empty()) {
        ClearWaiting(oldest->second);
        _cached.erase(oldest);
      }
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
