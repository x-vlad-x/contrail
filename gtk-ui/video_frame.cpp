#include "video_frame.hpp"

#include <QtGui/QTransform>

extern "C" {
#include <libswscale/swscale.h>
}

namespace Contrail {

QImage OrientVideoImage(const QImage &image, int rotation, bool mirror) {
  if (image.isNull()) return {};
  const auto degrees = ((rotation % 360) + 360) % 360;
  auto oriented = degrees ? image.transformed(QTransform().rotate(degrees)) : image;
  return mirror ? oriented.transformed(QTransform().scale(-1, 1)) : oriented;
}

VideoFrameConverter::~VideoFrameConverter() {
  sws_freeContext(_context);
}

QImage VideoFrameConverter::fromYuv420(
    const unsigned char *const planes[3], const int strides[3], int width, int height) {
  if (width <= 0 || height <= 0 || !planes[0] || !planes[1] || !planes[2]) return {};
  _context = sws_getCachedContext(_context, width, height, AV_PIX_FMT_YUV420P,
      width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!_context) return {};
  auto image = QImage(width, height, QImage::Format_ARGB32);
  if (image.isNull()) return {};
  unsigned char *destinations[4] = {image.bits(), nullptr, nullptr, nullptr};
  const int destinationStrides[4] = {int(image.bytesPerLine()), 0, 0, 0};
  const unsigned char *sources[4] = {planes[0], planes[1], planes[2], nullptr};
  const int sourceStrides[4] = {strides[0], strides[1], strides[2], 0};
  if (sws_scale(_context, sources, sourceStrides, 0, height, destinations, destinationStrides) != height) return {};
  return image;
}

} // namespace Contrail
