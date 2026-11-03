#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

// Telling what a paste brings and reading it, without the rest of the program (tests/paste.cpp).
namespace Contrail {

// What a paste brings. Files come first: a file manager offers a copied file's thumbnail as a picture
// too, and both must not be sent. Then a picture (a screenshot, an image copied in a browser). Anything
// else is text for the message field.
enum class PasteKind { Text, Files, Image };

// Pasted or chosen files (GFile): local ones are sent, folders and unreadable files are refused, remote
// ones (http://, an unmounted server) stay text. Flatpak's document portal gives local paths too.
struct PastedFiles {
  std::vector<std::string> local;
  std::vector<std::string> remote;
};

inline PasteKind ClassifyPaste(GdkContentFormats *formats) {
  if (!formats) return PasteKind::Text;
  // The clipboard of another program lists MIME types: add the types GTK can read them as.
  auto *types = gdk_content_formats_union_deserialize_gtypes(gdk_content_formats_ref(formats));
  const auto files = gdk_content_formats_contain_gtype(types, GDK_TYPE_FILE_LIST);
  const auto image = gdk_content_formats_contain_gtype(types, GDK_TYPE_TEXTURE);
  gdk_content_formats_unref(types);
  return files ? PasteKind::Files : image ? PasteKind::Image : PasteKind::Text;
}

inline PastedFiles SplitPastedFiles(GSList *files) {
  auto result = PastedFiles();
  for (auto *i = files; i; i = i->next) {
    auto *file = G_FILE(i->data);
    // Remote locations mounted by GVfs have a local path too.
    if (auto *path = g_file_get_path(file)) {
      result.local.emplace_back(path);
      g_free(path);
    } else if (auto *uri = g_file_get_uri(file)) {
      result.remote.emplace_back(uri);
      g_free(uri);
    }
  }
  return result;
}

// A pasted picture becomes a PNG file to send: TDLib uploads files, and the picture is lossless that way.
// False when it couldn't be written.
inline bool SaveTexturePng(GdkTexture *texture, const std::string &path) {
  if (!texture) return false;
  auto *bytes = gdk_texture_save_to_png_bytes(texture);
  if (!bytes) return false;
  gsize size = 0;
  const auto data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
  // Written whole or not at all: a half-written picture must not be sent.
  const auto written = size > 0 && g_file_set_contents(path.c_str(), data, gssize(size), nullptr);
  g_bytes_unref(bytes);
  return written;
}

}  // namespace Contrail
