// What a paste brings (gtk-ui/paste.hpp): files before a picture before text, local files apart from
// remote ones, a picture read back exactly.
#include "paste.hpp"

#include <cassert>
#include <cstdio>

using namespace Contrail;

namespace {

PasteKind Kind(std::initializer_list<const char *> mimeTypes) {
  auto *builder = gdk_content_formats_builder_new();
  for (const auto mime : mimeTypes) {
    gdk_content_formats_builder_add_mime_type(builder, mime);
  }
  auto *formats = gdk_content_formats_builder_free_to_formats(builder);
  const auto result = ClassifyPaste(formats);
  gdk_content_formats_unref(formats);
  return result;
}

}  // namespace

int main() {
  // GTK's readers of the clipboard formats are registered with the display, as in the app.
  gtk_init();
  // A file manager's copy offers the file and its thumbnail: the file wins.
  assert(Kind({"x-special/gnome-copied-files", "text/uri-list", "image/png", "text/plain;charset=utf-8"}) == PasteKind::Files);
  // Flatpak: files come through the document portal.
  assert(Kind({"application/vnd.portal.filetransfer"}) == PasteKind::Files);
  // A screenshot, an image copied in a browser (which may add HTML).
  assert(Kind({"image/png"}) == PasteKind::Image);
  assert(Kind({"text/html", "image/png"}) == PasteKind::Image);
  assert(Kind({"image/jpeg"}) == PasteKind::Image);
  // Text stays text.
  assert(Kind({"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}) == PasteKind::Text);
  assert(Kind({"text/html", "text/plain"}) == PasteKind::Text);
  assert(ClassifyPaste(nullptr) == PasteKind::Text);

  // Local paths are sent, remote URIs stay text.
  auto *local = g_file_new_for_path("/tmp/photo 1.jpg");
  auto *portal = g_file_new_for_path("/run/user/1000/doc/4f2a/scan.pdf");
  auto *remote = g_file_new_for_uri("https://example.org/picture.png");
  auto *list = g_slist_append(g_slist_append(g_slist_append(nullptr, local), remote), portal);
  const auto split = SplitPastedFiles(list);
  assert(split.local == QStringList({QString::fromUtf8("/tmp/photo 1.jpg"), QString::fromUtf8("/run/user/1000/doc/4f2a/scan.pdf")}));
  assert(split.remote == QStringList({QString::fromUtf8("https://example.org/picture.png")}));
  g_slist_free_full(list, g_object_unref);

  // A pasted picture is read back pixel for pixel, with an odd width (a stride wider than the row).
  const auto width = 7, height = 3, stride = 32;
  auto *data = static_cast<guchar *>(g_malloc0(stride * height));
  for (auto y = 0; y != height; ++y) {
    for (auto x = 0; x != width; ++x) {
      auto *pixel = data + y * stride + x * 4;
      pixel[0] = guchar(x * 30);  // B
      pixel[1] = guchar(y * 80);  // G
      pixel[2] = 200;             // R
      pixel[3] = 255;             // A
    }
  }
  auto *bytes = g_bytes_new_take(data, stride * height);
  auto *texture = gdk_memory_texture_new(width, height, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride);
  g_bytes_unref(bytes);
  const auto image = TextureToImage(texture);
  g_object_unref(texture);
  assert(image.size() == QSize(width, height));
  for (auto y = 0; y != height; ++y) {
    for (auto x = 0; x != width; ++x) {
      assert(image.pixel(x, y) == qRgba(200, y * 80, x * 30, 255));
    }
  }
  assert(TextureToImage(nullptr).isNull());
  std::puts("paste: ok");
  return 0;
}
