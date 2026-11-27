#include "legacy_settings.hpp"

#include <glib.h>
#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace Contrail {
namespace {

using Bytes = std::vector<std::uint8_t>;

// Telegram Desktop's storage (storage/details/storage_file_utilities.cpp, storage/localstorage.cpp,
// core/core_settings.cpp at the tag 0.3.1 was built on): files start with "TDF$" and the writer's version and end
// with an MD5 of the rest; their data is Qt's QDataStream (big-endian); the settings are encrypted with a key
// derived from an empty passcode and a salt stored beside them.
constexpr char kMagic[] = {'T', 'D', 'F', '$'};
constexpr int kNoPasscodeIterations = 4;
constexpr std::size_t kSaltSize = 32;
constexpr std::size_t kKeySize = 256;

// The settings blocks of storage_settings_scheme.h that a settings file written by 0.3.1 holds.
enum Block : std::uint32_t {
  kAutoStart = 0x06,
  kStartMinimized = 0x07,
  kSeenTrayTooltip = 0x0a,
  kAutoUpdate = 0x0c,
  kLastUpdateCheck = 0x0d,
  kSendToMenu = 0x1d,
  kDialogLastPath = 0x23,
  kLangPackKey = 0x4e,
  kThemeKey = 0x54,
  kTileBackground = 0x55,
  kPowerSaving = 0x57,
  kScalePercent = 0x58,
  kLanguagesKey = 0x5a,
  kApplicationSettings = 0x5e,
  kFallbackProductionConfig = 0x60,
  kBackgroundKey = 0x61,
};

class Reader {
 public:
  explicit Reader(const Bytes &data) : _data(data) {}

  [[nodiscard]] bool ok() const { return _ok; }
  [[nodiscard]] bool atEnd() const { return _position >= _data.size(); }

  std::uint64_t number(std::size_t size) {
    if (!_ok || _data.size() - _position < size) {
      _ok = false;
      return 0;
    }
    auto result = std::uint64_t();
    for (std::size_t i = 0; i != size; ++i) result = (result << 8) | _data[_position++];
    return result;
  }
  std::int32_t int32() { return static_cast<std::int32_t>(number(4)); }
  std::uint64_t uint64() { return number(8); }

  // QByteArray: its length, 0xFFFFFFFF for a null one, then the bytes.
  Bytes bytes() {
    const auto size = number(4);
    if (!_ok || size == 0xFFFFFFFF) return {};
    if (_data.size() - _position < size) {
      _ok = false;
      return {};
    }
    auto result = Bytes(_data.begin() + _position, _data.begin() + _position + size);
    _position += size;
    return result;
  }
  // QString: the same, in UTF-16 big-endian.
  std::string string() {
    const auto raw = bytes();
    if (!_ok || raw.size() % 2) {
      _ok = false;
      return {};
    }
    if (raw.empty()) return {};
    auto units = std::vector<gunichar2>(raw.size() / 2);
    for (std::size_t i = 0; i != units.size(); ++i) units[i] = gunichar2((raw[2 * i] << 8) | raw[2 * i + 1]);
    auto *text = g_utf16_to_utf8(units.data(), glong(units.size()), nullptr, nullptr, nullptr);
    auto result = std::string(text ? text : "");
    g_free(text);
    return result;
  }

 private:
  const Bytes &_data;
  std::size_t _position = 0;
  bool _ok = true;
};

[[nodiscard]] bool Digest(const EVP_MD *type, const Bytes &data, std::uint8_t *result) {
  return EVP_Digest(data.data(), data.size(), result, nullptr, type, nullptr) == 1;
}

[[nodiscard]] std::optional<Bytes> ReadContents(const std::string &path) {
  auto stream = std::ifstream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// A file of tdata ("settings" is "settingss", or the older "settings0"/"settings1"), checked against its MD5.
[[nodiscard]] std::optional<Bytes> ReadFile(const std::string &tdata, const std::string &name) {
  for (const auto *suffix : {"s", "1", "0"}) {
    const auto contents = ReadContents(tdata + "/" + name + suffix);
    if (!contents || contents->size() < sizeof(kMagic) + 4 + 16) continue;
    if (std::memcmp(contents->data(), kMagic, sizeof(kMagic)) != 0) continue;
    const auto version = Bytes(contents->begin() + 4, contents->begin() + 8);
    auto data = Bytes(contents->begin() + 8, contents->end() - 16);
    // MD5 of the data, its size and the version (both little-endian, as written from memory) and the magic.
    auto signed_ = data;
    const auto size = std::uint32_t(data.size());
    for (auto i = 0; i != 4; ++i) signed_.push_back(std::uint8_t(size >> (8 * i)));
    signed_.insert(signed_.end(), version.begin(), version.end());
    signed_.insert(signed_.end(), std::begin(kMagic), std::end(kMagic));
    auto md5 = std::array<std::uint8_t, 16>();
    if (!Digest(EVP_md5(), signed_, md5.data()) || std::memcmp(md5.data(), &*(contents->end() - 16), 16) != 0) {
      continue;
    }
    return data;
  }
  return std::nullopt;
}

using Key = std::array<std::uint8_t, kKeySize>;

[[nodiscard]] std::optional<Key> LocalKey(const Bytes &salt) {
  auto key = Key();
  if (PKCS5_PBKDF2_HMAC_SHA1("", 0, salt.data(), int(salt.size()), kNoPasscodeIterations, int(key.size()),
                             key.data()) != 1) {
    return std::nullopt;
  }
  return key;
}

// AES-256-IGE with the key and IV of MTProto 1.0 for a message key (AuthKey::prepareAES_oldmtp, receiving side).
[[nodiscard]] std::optional<Bytes> Decrypt(const Bytes &encrypted, const Key &authKey) {
  if (encrypted.size() <= 16 || encrypted.size() % 16) return std::nullopt;
  const auto *msgKey = encrypted.data();
  constexpr auto x = 8;
  auto sha1 = [](std::initializer_list<std::pair<const std::uint8_t *, std::size_t>> parts) {
    auto data = Bytes();
    for (const auto &[pointer, size] : parts) data.insert(data.end(), pointer, pointer + size);
    auto result = std::array<std::uint8_t, 20>();
    if (!Digest(EVP_sha1(), data, result.data())) result.fill(0);
    return result;
  };
  const auto *k = authKey.data();
  const auto a = sha1({{msgKey, 16}, {k + x, 32}});
  const auto b = sha1({{k + 32 + x, 16}, {msgKey, 16}, {k + 48 + x, 16}});
  const auto c = sha1({{k + 64 + x, 32}, {msgKey, 16}});
  const auto d = sha1({{msgKey, 16}, {k + 96 + x, 32}});
  auto aesKey = std::array<std::uint8_t, 32>();
  auto iv = std::array<std::uint8_t, 32>();
  std::memcpy(aesKey.data(), a.data(), 8);
  std::memcpy(aesKey.data() + 8, b.data() + 8, 12);
  std::memcpy(aesKey.data() + 20, c.data() + 4, 12);
  std::memcpy(iv.data(), a.data() + 8, 12);
  std::memcpy(iv.data() + 12, b.data(), 8);
  std::memcpy(iv.data() + 20, c.data() + 16, 4);
  std::memcpy(iv.data() + 24, d.data(), 8);

  // IGE over plain AES blocks: p[i] = D(c[i] ^ p[i-1]) ^ c[i-1], starting from the two halves of the IV.
  auto *context = EVP_CIPHER_CTX_new();
  if (!context) return std::nullopt;
  auto result = Bytes(encrypted.size() - 16);
  auto ok = EVP_DecryptInit_ex(context, EVP_aes_256_ecb(), nullptr, aesKey.data(), nullptr) == 1
      && EVP_CIPHER_CTX_set_padding(context, 0) == 1;
  auto previousCipher = std::array<std::uint8_t, 16>();
  auto previousPlain = std::array<std::uint8_t, 16>();
  std::memcpy(previousCipher.data(), iv.data(), 16);
  std::memcpy(previousPlain.data(), iv.data() + 16, 16);
  for (std::size_t offset = 0; ok && offset != result.size(); offset += 16) {
    const auto *cipher = encrypted.data() + 16 + offset;
    auto block = std::array<std::uint8_t, 16>();
    for (auto i = 0; i != 16; ++i) block[i] = cipher[i] ^ previousPlain[i];
    auto length = 0;
    ok = EVP_DecryptUpdate(context, result.data() + offset, &length, block.data(), 16) == 1 && length == 16;
    for (auto i = 0; i != 16; ++i) result[offset + i] ^= previousCipher[i];
    std::memcpy(previousCipher.data(), cipher, 16);
    std::memcpy(previousPlain.data(), result.data() + offset, 16);
  }
  EVP_CIPHER_CTX_free(context);
  if (!ok) return std::nullopt;

  // The message key is the start of the SHA-1 of the decrypted data, which begins with its own length.
  auto check = std::array<std::uint8_t, 20>();
  if (!Digest(EVP_sha1(), result, check.data()) || std::memcmp(check.data(), msgKey, 16) != 0) return std::nullopt;
  const auto length = std::uint32_t(result[0]) | (std::uint32_t(result[1]) << 8) | (std::uint32_t(result[2]) << 16)
      | (std::uint32_t(result[3]) << 24);
  if (length > result.size() || length <= result.size() - 16 || length < 4) return std::nullopt;
  return Bytes(result.begin() + 4, result.begin() + length);
}

// Core::Settings::addFromSerialized, as far as the close behaviour: each part may end the data.
void ReadApplicationSettings(const Bytes &data, LegacySettings &result) {
  auto stream = Reader(data);
  auto skip = [&](int count) {
    for (auto i = 0; i != count; ++i) stream.int32();
  };
  stream.bytes();  // Theme accent colours.
  if (stream.atEnd()) return;
  skip(5);  // Adaptive layout, moderate mode, song and video volume, asking for the download path.
  stream.string();
  stream.bytes();
  skip(1);  // Voice playback speed (old).
  const auto sound = stream.int32();
  const auto notify = stream.int32();
  skip(1);  // Flash and bounce.
  const auto view = stream.int32();
  skip(4);  // Native notifications, their count and corner, auto lock.
  if (!stream.ok()) return;
  result.sound = (sound == 1);
  result.notify = (notify == 1);
  result.preview = (view == 0);  // NotifyView::ShowPreview; ShowName and ShowNothing hide the text.
  stream.string();
  stream.string();
  skip(4);  // Call volumes, ducking, the last seen warning.
  const auto overrides = stream.int32();
  for (auto i = 0; stream.ok() && i < overrides; ++i) {
    stream.string();
    stream.string();
  }
  skip(13);  // Sending, counters, stickers and emoji, spell checking, video speed.
  stream.bytes();
  const auto dictionaries = stream.int32();
  for (auto i = 0; stream.ok() && i < dictionaries; ++i) stream.uint64();
  skip(2);
  if (stream.atEnd()) return;
  skip(8);  // Columns and the floating player.
  if (stream.atEnd()) return;
  skip(1);  // Native window frame.
  if (stream.atEnd()) return;
  skip(1);  // Following the system's dark mode.
  if (stream.atEnd()) return;
  stream.string();  // Camera.
  if (stream.atEnd()) return;
  skip(1);  // IP reveal warning.
  if (stream.atEnd()) return;
  skip(1);  // Push to talk, its shortcut and delay.
  stream.bytes();
  stream.uint64();
  if (stream.atEnd()) return;
  skip(1);  // Call audio backend (old).
  if (stream.atEnd()) return;
  skip(1);  // Calls disabled (old).
  if (stream.atEnd()) return;
  stream.bytes();  // Window position.
  if (stream.atEnd()) return;
  const auto recent = stream.int32();
  if (recent > 0 && recent < 10000) {
    for (auto i = 0; stream.ok() && i != recent; ++i) {
      stream.string();
      stream.number(2);
    }
  }
  const auto variants = stream.int32();
  if (variants > 0 && variants < 10000) {
    for (auto i = 0; stream.ok() && i != variants; ++i) {
      stream.string();
      stream.number(1);
    }
  }
  // OpenGL and noise suppression (old), the work mode, the proxy, hidden tooltips, OpenGL, the photo editor's
  // brush, noise suppression, voice playback speed: then the close behaviour.
  for (const auto isBytes : {false, false, false, true, false, false, true, false, false}) {
    if (stream.atEnd()) return;
    if (isBytes) {
      stream.bytes();
    } else {
      skip(1);
    }
  }
  if (stream.atEnd()) return;
  const auto close = stream.int32();
  // The device model follows when 0.3.1 wrote it: a string that reads well means nothing above was misread.
  if (!stream.atEnd()) stream.string();
  if (stream.ok()) result.background = (close == 2);  // CloseBehavior::RunInBackground.
}

// Lang::Instance::fillFromSerialized: the language pack's ID.
[[nodiscard]] std::string ReadLanguage(const std::string &tdata, std::uint64_t key, const Key &settingsKey) {
  // The file name is the key's 16 hexadecimal digits, the lowest first (ToFilePart).
  auto name = std::string();
  for (auto i = 0; i != 16; ++i, key >>= 4) name.push_back("0123456789ABCDEF"[key & 0x0F]);
  const auto file = ReadFile(tdata, name);
  if (!file) return {};
  auto outer = Reader(*file);
  const auto decrypted = Decrypt(outer.bytes(), settingsKey);
  if (!outer.ok() || !decrypted) return {};
  auto inner = Reader(*decrypted);
  const auto serialized = inner.bytes();
  auto stream = Reader(serialized);
  auto id = stream.string();
  if (id == "#new") {
    if (stream.int32() != 1) return {};
    id = stream.string();
  }
  // A custom language file ("#custom") has no pack on Telegram's servers.
  if (!stream.ok() || id.empty() || id.front() == '#') return {};
  return id;
}

}  // namespace

std::optional<LegacySettings> ReadLegacySettings(const std::string &tdata) {
  const auto file = ReadFile(tdata, "settings");
  if (!file) return std::nullopt;
  auto outer = Reader(*file);
  const auto salt = outer.bytes();
  const auto encrypted = outer.bytes();
  if (!outer.ok() || salt.size() != kSaltSize) return std::nullopt;
  const auto key = LocalKey(salt);
  const auto settings = key ? Decrypt(encrypted, *key) : std::nullopt;
  if (!settings) return std::nullopt;

  auto result = LegacySettings();
  auto stream = Reader(*settings);
  auto languageKey = std::uint64_t();
  auto found = false;
  auto known = true;
  while (known && stream.ok() && !stream.atEnd()) {
    switch (stream.int32()) {
      case kAutoStart:
      case kStartMinimized:
      case kSendToMenu:
      case kSeenTrayTooltip:
      case kAutoUpdate:
      case kLastUpdateCheck:
      case kScalePercent:
      case kPowerSaving: stream.int32(); break;
      case kFallbackProductionConfig: stream.bytes(); break;
      case kApplicationSettings: {
        const auto data = stream.bytes();
        if (stream.ok()) {
          ReadApplicationSettings(data, result);
          found = true;
        }
        break;
      }
      case kDialogLastPath: stream.string(); break;
      case kThemeKey:
        stream.uint64();
        stream.uint64();
        stream.int32();
        break;
      case kBackgroundKey:
        stream.uint64();
        stream.uint64();
        break;
      case kTileBackground:
        stream.int32();
        stream.int32();
        break;
      case kLangPackKey: languageKey = stream.uint64(); break;
      case kLanguagesKey: stream.uint64(); break;
      default:
        // Written by another Telegram Desktop: what was read so far stays.
        known = false;
        break;
    }
  }
  if (!found) return std::nullopt;
  if (languageKey) result.language = ReadLanguage(tdata, languageKey, *key);
  // Storage::Domain keeps the accounts' keys in "key_data" (with the passcode, if there is one).
  for (const auto *name : {"/key_datas", "/key_data0", "/key_data1"}) {
    if (g_file_test((tdata + name).c_str(), G_FILE_TEST_IS_REGULAR)) result.account = true;
  }
  return result;
}

}  // namespace Contrail
