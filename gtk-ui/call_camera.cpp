#include "call_camera.hpp"
#include "td/session.hpp"
#include <glib.h>
#include <algorithm>

namespace Contrail {
namespace {
std::string CameraFile() { return Td::DataDirectory() + "/call-camera.ini"; }
std::string Lower(const std::string &value) {
  auto *text = g_ascii_strdown(value.c_str(), -1);
  auto result = std::string(text);
  g_free(text);
  return result;
}
}

std::string PreferredCallCamera(const std::vector<Calls::Camera> &cameras) {
  if (cameras.empty()) return {};
  auto saved = std::string();
  if (!g_getenv("CONTRAIL_SCENARIO")) {
    auto *settings = g_key_file_new();
    if (g_key_file_load_from_file(settings, CameraFile().c_str(), G_KEY_FILE_NONE, nullptr)) {
      auto *id = g_key_file_get_string(settings, "camera", "device", nullptr);
      if (id) { saved = id; g_free(id); }
    }
    g_key_file_unref(settings);
  }
  const auto found = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) { return camera.id == saved; });
  return found != cameras.end() ? found->id : cameras.front().id;
}

std::string NextCallCamera(const std::vector<Calls::Camera> &cameras, const std::string &current) {
  if (cameras.size() < 2 || current.empty()) return {};
  auto from = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) { return camera.id == current; });
  const auto front = from != cameras.end() && Lower(from->name).find("front") != std::string::npos;
  const auto other = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) {
    const auto name = Lower(camera.name);
    return camera.id != current && (front ? name.find("back") != std::string::npos || name.find("rear") != std::string::npos
        : name.find("front") != std::string::npos);
  });
  // A phone's button flips front/back even when the HAL also exposes a second rear sensor.
  if (other != cameras.end()) return other->id;
  if (from == cameras.end() || ++from == cameras.end()) from = cameras.begin();
  return from->id;
}

void RememberCallCamera(const std::string &id) {
  if (id.empty() || g_getenv("CONTRAIL_SCENARIO")) return;
  if (g_mkdir_with_parents(Td::DataDirectory().c_str(), 0700) != 0) return;
  auto *settings = g_key_file_new();
  g_key_file_set_string(settings, "camera", "device", id.c_str());
  g_key_file_save_to_file(settings, CameraFile().c_str(), nullptr);
  g_key_file_unref(settings);
}
} // namespace Contrail
