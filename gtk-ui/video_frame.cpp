#include "video_frame.hpp"

#include <api/video/video_frame.h>
#include <api/video/i420_buffer.h>
#include <libyuv.h>

namespace Contrail {
Calls::Frame ConvertCallVideoFrame(const webrtc::VideoFrame &frame, bool mirror) {
  const auto source = frame.video_frame_buffer()->ToI420();
  if (!source || source->width() <= 0 || source->height() <= 0) return {};
  const auto rotation = static_cast<libyuv::RotationMode>(frame.rotation());
  const auto sideways = rotation == libyuv::kRotate90 || rotation == libyuv::kRotate270;
  const auto width = sideways ? source->height() : source->width();
  const auto height = sideways ? source->width() : source->height();
  const auto upright = webrtc::I420Buffer::Rotate(*source, frame.rotation());
  auto pixels = std::make_shared<std::vector<std::uint8_t>>(std::size_t(width) * height * 4);
  if (libyuv::I420ToARGB(upright->DataY(), upright->StrideY(), upright->DataU(), upright->StrideU(),
      upright->DataV(), upright->StrideV(), pixels->data(), width * 4, width, height) != 0) return {};
  if (mirror) {
    auto mirrored = std::make_shared<std::vector<std::uint8_t>>(pixels->size());
    if (libyuv::ARGBMirror(pixels->data(), width * 4, mirrored->data(), width * 4, width, height) != 0) return {};
    pixels = std::move(mirrored);
  }
  return {.width = width, .height = height, .stride = width * 4, .pixels = std::move(pixels)};
}
} // namespace Contrail
