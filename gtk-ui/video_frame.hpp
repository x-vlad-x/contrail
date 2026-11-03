#pragma once

#include "call_media.hpp"

namespace webrtc { class VideoFrame; }

namespace Contrail {
// Opaque BGRA, rotated upright, mirrored only for the front camera's local preview.
[[nodiscard]] Calls::Frame ConvertCallVideoFrame(const webrtc::VideoFrame &frame, bool mirror);
} // namespace Contrail
