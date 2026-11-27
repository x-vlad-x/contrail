// What a paste brings (gtk-ui/paste.hpp): files before a picture before text, local files apart from
// remote ones, a picture written to a PNG and read back exactly.
#include "paste.hpp"

#include <glib/gstdio.h>

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
  // GTK's readers of the clipboard formats are registered with the display, as in the app (tools/td-ui-test.sh
  // runs this under Xvfb).
  gtk_init();
  // A file manager's copy offers the file and its thumbnail: the file wins.
  g_assert_true(Kind({"x-special/gnome-copied-files", "text/uri-list", "image/png", "text/plain;charset=utf-8"}) == PasteKind::Files);
  // Flatpak: files come through the document portal. GTK reads them only where the portal runs: text elsewhere.
  const auto portalKind = Kind({"application/vnd.portal.filetransfer"});
  g_assert_true(portalKind == PasteKind::Files || portalKind == PasteKind::Text);
  // A screenshot, an image copied in a browser (which may add HTML).
  g_assert_true(Kind({"image/png"}) == PasteKind::Image);
  g_assert_true(Kind({"text/html", "image/png"}) == PasteKind::Image);
  g_assert_true(Kind({"image/jpeg"}) == PasteKind::Image);
  // Text stays text.
  g_assert_true(Kind({"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}) == PasteKind::Text);
  g_assert_true(Kind({"text/html", "text/plain"}) == PasteKind::Text);
  g_assert_true(ClassifyPaste(nullptr) == PasteKind::Text);

  // Local paths are sent, remote URIs stay text.
  auto *local = g_file_new_for_path("/tmp/photo 1.jpg");
  auto *portal = g_file_new_for_path("/run/user/1000/doc/4f2a/scan.pdf");
  auto *remote = g_file_new_for_uri("https://example.org/picture.png");
  auto *list = g_slist_append(g_slist_append(g_slist_append(nullptr, local), remote), portal);
  const auto split = SplitPastedFiles(list);
  g_assert_true((split.local == std::vector<std::string>{"/tmp/photo 1.jpg", "/run/user/1000/doc/4f2a/scan.pdf"}));
  g_assert_true((split.remote == std::vector<std::string>{"https://example.org/picture.png"}));
  g_slist_free_full(list, g_object_unref);

  // A pasted picture is written and read back pixel for pixel, with an odd width (a stride wider than the row).
  const auto width = 7, height = 3, stride = 32;
  auto *data = static_cast<guchar *>(g_malloc0(stride * height));
  for (auto y = 0; y != height; ++y) {
    for (auto x = 0; x != width; ++x) {
      auto *pixel = data + y * stride + x * 4;
      pixel[0] = 200;             // R
      pixel[1] = guchar(y * 80);  // G
      pixel[2] = guchar(x * 30);  // B
      pixel[3] = 255;             // A
    }
  }
  auto *bytes = g_bytes_new_take(data, stride * height);
  auto *texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, stride);
  g_bytes_unref(bytes);
  auto *directory = g_dir_make_tmp("contrail-paste-XXXXXX", nullptr);
  g_assert_true(directory);
  const auto path = std::string(directory) + "/pasted.png";
  g_assert_true(SaveTexturePng(texture, path));
  g_object_unref(texture);
  auto *read = gdk_texture_new_from_filename(path.c_str(), nullptr);
  g_assert_true(read && gdk_texture_get_width(read) == width && gdk_texture_get_height(read) == height);
  guchar pixels[width * height * 4];
  gdk_texture_download(read, pixels, width * 4);  // Cairo's ARGB32: B, G, R, A in memory.
  for (auto y = 0; y != height; ++y) {
    for (auto x = 0; x != width; ++x) {
      const auto *pixel = pixels + (y * width + x) * 4;
      g_assert_true(pixel[0] == x * 30 && pixel[1] == y * 80 && pixel[2] == 200 && pixel[3] == 255);
    }
  }
  g_object_unref(read);
  g_assert_true(!SaveTexturePng(nullptr, path + ".none"));
  g_unlink(path.c_str());
  g_rmdir(directory);
  g_free(directory);
  std::puts("paste: ok");
  return 0;
}
