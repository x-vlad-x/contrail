// Decodes a picture file to the size it is shown at. Safe on any thread.
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>

namespace Contrail {

struct DecodeOptions {
  bool pixbufFallback = true;  // false: only JPEG and PNG, nullptr for anything else.
  // A large picture (a progressive JPEG or a PNG over 2 Mpx) holds a lot of memory while it is decoded, so those go
  // one at a time. false: when another one is being decoded, this gives nullptr at once and sets *deferred.
  bool waitForLarge = true;
  bool *deferred = nullptr;
  // Checked before the decoding starts and after the wait for a large one: cancelled gives nullptr.
  GCancellable *cancellable = nullptr;
};

// A new pixbuf covering width×height pixels (no smaller than either, the picture's own proportions), or the whole
// picture when it is smaller than that or width/height is 0; turned by its EXIF orientation. nullptr if the file
// can't be read as a picture.
//
// JPEG and PNG are decoded here, with libjpeg and libpng: since 2.44 gdk-pixbuf hands them to glycin, which starts
// a sandboxed loader process for every picture (250 ms a photo on the FLX1s against 25–50 ms here) and keeps
// threads of its own in ours. libjpeg also decodes a JPEG straight at 1/2…1/8 of its size, so the full-size photo
// is never in memory. Other formats, and files these two refuse, still go to gdk-pixbuf.
[[nodiscard]] GdkPixbuf *DecodePictureFile(const char *path, int width, int height, const DecodeOptions &options = {});

// A texture on the pixbuf's own pixels, without a copy; `pixbuf` is taken over. 8 bits a sample.
[[nodiscard]] GdkTexture *TextureFromPixbuf(GdkPixbuf *pixbuf);

// For the tests: the most large decodes that have run at the same time, and how many run now.
[[nodiscard]] int LargeDecodesAtOnce();
[[nodiscard]] int LargeDecodesNow();

}  // namespace Contrail
