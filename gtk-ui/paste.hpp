#pragma once

#include <gtk/gtk.h>

#include <QtCore/QStringList>
#include <QtGui/QImage>

#include <algorithm>
#include <cstring>

// Telling what a paste brings and reading it, without the core (tests/paste.cpp).
namespace Contrail {

// What a paste brings. Files come first: a file manager offers a copied file's thumbnail as a picture
// too, and both must not be sent. Then a picture (a screenshot, an image copied in a browser). Anything
// else is text for the message field.
enum class PasteKind { Text, Files, Image };

// Pasted or chosen files (GFile): local ones are sent, folders and unreadable files are refused, remote
// ones (http://, an unmounted server) stay text. Flatpak's document portal gives local paths too.
struct PastedFiles {
  QStringList local;
  QStringList remote;
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
      result.local.push_back(QString::fromUtf8(path));
      g_free(path);
    } else if (auto *uri = g_file_get_uri(file)) {
      result.remote.push_back(QString::fromUtf8(uri));
      g_free(uri);
    }
  }
  return result;
}

inline QImage TextureToImage(GdkTexture *texture) {
  if (!texture) return QImage();
  const auto width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
  auto *downloader = gdk_texture_downloader_new(texture);
  // BGRA in memory is Qt's ARGB32 on little-endian machines.
  gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
  gsize stride = 0;
  auto *bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
  gdk_texture_downloader_free(downloader);
  auto image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
  gsize size = 0;
  const auto data = static_cast<const uchar *>(g_bytes_get_data(bytes, &size));
  const auto line = std::min(gsize(image.bytesPerLine()), stride);
  for (auto y = 0; y != height && gsize(y) * stride + line <= size; ++y) {
    memcpy(image.scanLine(y), data + y * stride, line);
  }
  g_bytes_unref(bytes);
  return image;
}

}  // namespace Contrail
