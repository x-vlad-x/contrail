// ReadLegacySettings against tdata folders written the way Telegram Desktop writes them for Contrail 0.3: the
// switches, the language pack, an account, damaged and missing files.
//   legacy-settings-test            the checks
//   legacy-settings-test TDATA      prints what a real folder gives (only reads it)
#include "legacy_settings.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace Contrail {
namespace {

using Bytes = std::vector<std::uint8_t>;

// QDataStream, big-endian.
struct Writer {
  Bytes data;
  Writer &number(std::uint64_t value, int size) {
    for (auto i = size - 1; i >= 0; --i) data.push_back(std::uint8_t(value >> (8 * i)));
    return *this;
  }
  Writer &int32(std::int32_t value) { return number(std::uint32_t(value), 4); }
  Writer &uint64(std::uint64_t value) { return number(value, 8); }
  Writer &bytes(const Bytes &value) {
    number(value.size(), 4);
    data.insert(data.end(), value.begin(), value.end());
    return *this;
  }
  Writer &string(const std::string &value) {
    auto raw = Bytes();
    for (const auto c : value) raw.insert(raw.end(), {0, std::uint8_t(c)});
    return bytes(raw);
  }
};

std::array<std::uint8_t, 20> Sha1(const Bytes &data) {
  auto result = std::array<std::uint8_t, 20>();
  EVP_Digest(data.data(), data.size(), result.data(), nullptr, EVP_sha1(), nullptr);
  return result;
}

// EncryptedDescriptor + writeEncrypted: the length, the data, random padding; the message key; AES-256-IGE.
Bytes Encrypt(const Bytes &payload, const Bytes &salt) {
  auto key = std::array<std::uint8_t, 256>();
  PKCS5_PBKDF2_HMAC_SHA1("", 0, salt.data(), int(salt.size()), 4, int(key.size()), key.data());
  auto plain = Bytes{0, 0, 0, 0};
  plain.insert(plain.end(), payload.begin(), payload.end());
  const auto length = std::uint32_t(plain.size());
  for (auto i = 0; i != 4; ++i) plain[i] = std::uint8_t(length >> (8 * i));
  while (plain.size() % 16) plain.push_back(0x5a);
  const auto hash = Sha1(plain);
  const auto *msgKey = hash.data();
  const auto *k = key.data();
  constexpr auto x = 8;
  auto part = [](std::initializer_list<std::pair<const std::uint8_t *, int>> parts) {
    auto data = Bytes();
    for (const auto &[p, n] : parts) data.insert(data.end(), p, p + n);
    return Sha1(data);
  };
  const auto a = part({{msgKey, 16}, {k + x, 32}});
  const auto b = part({{k + 32 + x, 16}, {msgKey, 16}, {k + 48 + x, 16}});
  const auto c = part({{k + 64 + x, 32}, {msgKey, 16}});
  const auto d = part({{msgKey, 16}, {k + 96 + x, 32}});
  auto aesKey = std::array<std::uint8_t, 32>(), iv = std::array<std::uint8_t, 32>();
  std::memcpy(aesKey.data(), a.data(), 8);
  std::memcpy(aesKey.data() + 8, b.data() + 8, 12);
  std::memcpy(aesKey.data() + 20, c.data() + 4, 12);
  std::memcpy(iv.data(), a.data() + 8, 12);
  std::memcpy(iv.data() + 12, b.data(), 8);
  std::memcpy(iv.data() + 20, c.data() + 16, 4);
  std::memcpy(iv.data() + 24, d.data(), 8);
  // c[i] = E(p[i] ^ c[i-1]) ^ p[i-1]
  auto *context = EVP_CIPHER_CTX_new();
  EVP_EncryptInit_ex(context, EVP_aes_256_ecb(), nullptr, aesKey.data(), nullptr);
  EVP_CIPHER_CTX_set_padding(context, 0);
  auto result = Bytes(msgKey, msgKey + 16);
  auto previousCipher = Bytes(iv.begin(), iv.begin() + 16), previousPlain = Bytes(iv.begin() + 16, iv.end());
  for (std::size_t offset = 0; offset != plain.size(); offset += 16) {
    auto block = std::array<std::uint8_t, 16>(), out = std::array<std::uint8_t, 16>();
    for (auto i = 0; i != 16; ++i) block[i] = plain[offset + i] ^ previousCipher[i];
    auto n = 0;
    EVP_EncryptUpdate(context, out.data(), &n, block.data(), 16);
    for (auto i = 0; i != 16; ++i) out[i] ^= previousPlain[i];
    result.insert(result.end(), out.begin(), out.end());
    previousCipher.assign(out.begin(), out.end());
    previousPlain.assign(plain.begin() + offset, plain.begin() + offset + 16);
  }
  EVP_CIPHER_CTX_free(context);
  return result;
}

// FileWriteDescriptor: "TDF$", the version, the data, the MD5 of the data, its size, the version and the magic.
void WriteFile(const std::string &path, const Bytes &data) {
  const auto version = std::uint32_t(6002004);
  auto signed_ = data;
  for (const auto value : {std::uint32_t(data.size()), version}) {
    for (auto i = 0; i != 4; ++i) signed_.push_back(std::uint8_t(value >> (8 * i)));
  }
  signed_.insert(signed_.end(), {'T', 'D', 'F', '$'});
  auto md5 = std::array<std::uint8_t, 16>();
  EVP_Digest(signed_.data(), signed_.size(), md5.data(), nullptr, EVP_md5(), nullptr);
  auto file = Bytes{'T', 'D', 'F', '$'};
  for (auto i = 0; i != 4; ++i) file.push_back(std::uint8_t(version >> (8 * i)));
  file.insert(file.end(), data.begin(), data.end());
  file.insert(file.end(), md5.begin(), md5.end());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
}

struct Options {
  bool sound = true, notify = true;
  int view = 0, close = 0;
  bool full = true;  // Up to the close behaviour, or only the old fields.
  std::string language;
  bool account = false;
};

// Core::Settings::serialize, the fields addFromSerialized reads up to the close behaviour.
Bytes ApplicationSettings(const Options &o) {
  auto w = Writer();
  w.bytes({1, 2, 3});
  for (const auto v : {1, 0, 1000000, 1000000, 0}) w.int32(v);
  w.string("/home/user/Downloads").bytes({});
  w.int32(1).int32(o.sound).int32(o.notify).int32(1).int32(o.view);
  for (const auto v : {0, 3, 3, 3600}) w.int32(v);
  w.string("default").string("default");
  for (const auto v : {100000, 100000, 1, 0}) w.int32(v);
  w.int32(1).string("message").string("/sounds/a.ogg");
  for (auto i = 0; i != 13; ++i) w.int32(i % 2);
  w.bytes({9, 9}).int32(2).uint64(1049).uint64(1033).int32(1).int32(1);
  if (!o.full) return w.data;
  for (auto i = 0; i != 8; ++i) w.int32(i);
  w.int32(0).int32(1).string("camera").int32(1);
  w.int32(0).bytes({1}).uint64(500);
  w.int32(0).int32(0).bytes({7, 7, 7});
  w.int32(2).string("smile").number(5, 2).string("like").number(1, 2);
  w.int32(1).string("wave").number(2, 1);
  w.int32(0).int32(0).int32(0).bytes({4, 4}).int32(0).int32(0).bytes({5}).int32(1).int32(100);
  w.int32(o.close);
  w.string("model").int32(0);  // Fields after it are never read.
  return w.data;
}

std::string MakeTdata(const Options &o) {
  auto *dir = g_dir_make_tmp("legacy-settings-XXXXXX", nullptr);
  g_assert_nonnull(dir);
  const auto tdata = std::string(dir);
  g_free(dir);
  auto salt = Bytes(32);
  RAND_bytes(salt.data(), int(salt.size()));
  auto settings = Writer();
  for (const auto id : {0x06, 0x07, 0x1d, 0x0a, 0x0c, 0x0d, 0x58}) settings.int32(id).int32(0);
  settings.int32(0x60).bytes({1, 2, 3, 4});
  settings.int32(0x5e).bytes(ApplicationSettings(o));
  settings.int32(0x23).string("/tmp").int32(0x57).int32(0);
  settings.int32(0x54).uint64(1).uint64(2).int32(0);
  settings.int32(0x61).uint64(3).uint64(4).int32(0x55).int32(0).int32(0);
  if (!o.language.empty()) {
    const auto key = std::uint64_t(0xA336C3412CE7CFEAULL);
    settings.int32(0x4e).uint64(key);
    auto name = std::string();
    for (auto value = key, i = std::uint64_t(0); i != 16; ++i, value >>= 4) name.push_back("0123456789ABCDEF"[value & 0xF]);
    auto lang = Writer();
    lang.string("#new").int32(1).string(o.language).string(o.language).string("Name").string("Native").int32(42);
    WriteFile(tdata + "/" + name + "s", Writer().bytes(Encrypt(Writer().bytes(lang.data).data, salt)).data);
  }
  settings.int32(0x5a).uint64(77);
  WriteFile(tdata + "/settingss", Writer().bytes(salt).bytes(Encrypt(settings.data, salt)).data);
  if (o.account) WriteFile(tdata + "/key_datas", {1, 2, 3});
  return tdata;
}

void Check(const Options &o, bool preview, bool background) {
  const auto result = ReadLegacySettings(MakeTdata(o));
  g_assert_true(result.has_value());
  g_assert_cmpint(result->sound, ==, o.sound);
  g_assert_cmpint(result->notify, ==, o.notify);
  g_assert_cmpint(result->preview, ==, preview);
  g_assert_cmpint(result->background, ==, background);
  g_assert_cmpstr(result->language.c_str(), ==, o.language.c_str());
  g_assert_cmpint(result->account, ==, o.account);
}

void TestSwitches() {
  Check({}, true, false);
  Check({.sound = false, .notify = false, .view = 1, .close = 2, .language = "ru", .account = true}, false, true);
  Check({.view = 2, .close = 1, .language = "pt-br"}, false, false);
  // Written before the close behaviour was saved: the switches, and the default for the rest.
  Check({.sound = false, .full = false}, true, false);
}

void TestDamaged() {
  g_assert_false(ReadLegacySettings("/nonexistent/tdata").has_value());
  const auto tdata = MakeTdata({});
  // A changed byte breaks the MD5 signature.
  std::fstream file(tdata + "/settingss", std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(40);
  file.put('\x55');
  file.close();
  g_assert_false(ReadLegacySettings(tdata).has_value());
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  if (argc == 2) {
    const auto result = Contrail::ReadLegacySettings(argv[1]);
    if (!result) {
      std::puts("no settings");
      return 1;
    }
    std::printf("notify=%d sound=%d preview=%d background=%d language=%s account=%d\n", result->notify, result->sound,
                result->preview, result->background, result->language.c_str(), result->account);
    return 0;
  }
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/legacy/switches", Contrail::TestSwitches);
  g_test_add_func("/legacy/damaged", Contrail::TestDamaged);
  return g_test_run();
}
