#pragma once

#include <QtGui/QImage>

struct SwsContext;

namespace Contrail {

// Use the source pixels, not VideoTrack::frame()'s preview-sized cache shared with the Qt panel.
QImage OrientVideoImage(const QImage &image, int rotation, bool mirror);

class VideoFrameConverter final {
public:
  VideoFrameConverter() = default;
  VideoFrameConverter(const VideoFrameConverter &) = delete;
  VideoFrameConverter &operator=(const VideoFrameConverter &) = delete;
  ~VideoFrameConverter();
  QImage fromYuv420(const unsigned char *const planes[3], const int strides[3], int width, int height);

private:
  SwsContext *_context = nullptr;
};

} // namespace Contrail
