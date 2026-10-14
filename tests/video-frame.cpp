#include "video_frame.hpp"

#include <QtCore/QCoreApplication>
#include <cassert>
#include <array>
#include <vector>
#include <cstdio>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  auto source = QImage(640, 360, QImage::Format_RGB32);
  source.fill(Qt::blue);
  source.setPixelColor(0, 0, Qt::red);
  for (const auto rotation : {0, 90, 180, 270, -90, 450}) {
    const auto image = Contrail::OrientVideoImage(source, rotation, false);
    const auto portrait = (rotation % 180) != 0;
    assert(image.size() == (portrait ? QSize(360, 640) : QSize(640, 360)));
    assert(image.sizeInBytes() == source.sizeInBytes()); // No preview cache or upscaling.
  }
  const auto portrait = Contrail::OrientVideoImage(source, 90, false);
  assert(portrait.pixelColor(359, 0) == Qt::red);
  const auto mirrored = Contrail::OrientVideoImage(source, 90, true);
  assert(mirrored.pixelColor(0, 0) == Qt::red); // Mirror after rotation, only for the local front camera.

  Contrail::VideoFrameConverter converter;
  for (const auto width : {640, 1280, 320}) {
    const auto height = width * 3 / 4;
    std::vector<unsigned char> y(width * height, 235);
    std::vector<unsigned char> u(width * height / 4, 128), v(width * height / 4, 128);
    const unsigned char *planes[3] = {y.data(), u.data(), v.data()};
    const int strides[3] = {width, width / 2, width / 2};
    const auto image = converter.fromYuv420(planes, strides, width, height);
    assert(image.size() == QSize(width, height));
    assert(image.pixelColor(width / 2, height / 2).red() >= 250);
    assert(image.pixelColor(width / 2, height / 2).alpha() == 255);
    assert(Contrail::OrientVideoImage(image, 90, false).size() == QSize(height, width));
  }
  const unsigned char *empty[3] = {};
  const int strides[3] = {};
  assert(converter.fromYuv420(empty, strides, 0, 0).isNull());
  std::puts("Video regressions: rotation, front-camera mirroring, full resolution, YUV conversion and resolution changes passed");
}
