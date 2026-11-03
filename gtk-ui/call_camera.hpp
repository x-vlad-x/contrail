#pragma once
#include "call_media.hpp"

namespace Contrail {
[[nodiscard]] std::string PreferredCallCamera(const std::vector<Calls::Camera> &cameras);
[[nodiscard]] std::string NextCallCamera(const std::vector<Calls::Camera> &cameras, const std::string &current);
void RememberCallCamera(const std::string &id);
} // namespace Contrail
