// What a file to send is and how it goes to TDLib, without the interface (tests/td/attach-test.cpp).
#pragma once

#include "td/client.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Contrail {

// What a file is sent as, chosen as Telegram Desktop chose it for 0.3.1: a photo, a video, a GIF, music, or a file.
enum class AttachType { File, Photo, Video, Animation, Music };

struct AttachFile {
  std::string path;
  std::string name;  // The file's name, shown and sent.
  std::string mime;
  std::int64_t size = 0;
  AttachType type = AttachType::File;
  int width = 0;  // Of a photo, a video or a GIF, as it is shown (turned by its EXIF or its matrix).
  int height = 0;
  int duration = 0;  // Seconds, of a video or music; 0 when it can't be told.
  std::string title;  // Of music, from its tags.
  std::string performer;
  bool ogg = false;        // Ogg music goes on its own: Telegram would take it for a voice message in an album.
  bool streaming = false;  // MP4 and MOV play while they download.
  // Made by PrepareAttachFile. A photo as it is sent as one: a JPEG at most kPhotoSideLimit a side, turned upright.
  std::string photoPath;
  // A video's first frame, at most kThumbnailSide a side: its thumbnail, and the picture of its tile.
  std::string thumbnailPath;
  int thumbnailWidth = 0;
  int thumbnailHeight = 0;
};

// Telegram's limits for one file (the server's, as Telegram Desktop has them; TDLib doesn't tell them).
inline constexpr std::int64_t kFileSizeLimit = std::int64_t(2000) * 1024 * 1024;
inline constexpr std::int64_t kFileSizePremiumLimit = std::int64_t(4000) * 1024 * 1024;
// What Telegram takes as a photo; a larger picture goes as a file.
inline constexpr std::int64_t kPhotoSizeLimit = std::int64_t(10) * 1024 * 1024;
inline constexpr int kPhotoSideLimit = 2560;
inline constexpr int kThumbnailSide = 320;
inline constexpr int kMaxAlbumItems = 10;

// Why a file can't be sent, as Telegram Desktop says it: a folder, a missing or empty file, one too large. Empty
// when it can be.
[[nodiscard]] std::string AttachRefusal(const std::string &path, bool premium);

// Tells what the file is and makes what it needs in `directory`: the photo to send, a video's thumbnail. Reads
// headers and tags; decodes only a picture and a video's first frame. Runs in another thread.
[[nodiscard]] AttachFile PrepareAttachFile(const std::string &path, const std::string &directory);

// Where prepared and pasted files wait until TDLib has uploaded them; those older than a week go.
[[nodiscard]] std::string AttachDirectory();

// What a video or music file says about itself (FFmpeg reads it).
struct MediaInfo {
  int width = 0;  // Turned by the video's matrix.
  int height = 0;
  int duration = 0;  // Seconds, rounded.
  int rotation = 0;  // Clockwise degrees the video is turned when shown.
  bool video = false;
  bool audio = false;
  std::string title;
  std::string performer;
};
[[nodiscard]] bool ReadMediaInfo(const std::string &path, MediaInfo &info);

// An album of photos and videos, of files, of music, or a message on its own; as Ui::DivideByGroups did.
enum class AlbumKind { None, PhotoVideo, File, Music };
struct AttachGroup {
  std::vector<AttachFile> files;
  AlbumKind album = AlbumKind::None;
};
// `compress`: pictures and videos go as media ("Send as documents" off). `group`: in albums of up to ten.
[[nodiscard]] std::vector<AttachGroup> DivideAttachments(std::vector<AttachFile> files, bool compress, bool group);
// Which file of the group carries the caption: the first of an album of photos, the last of the others.
[[nodiscard]] std::size_t CaptionIndex(const AttachGroup &group);

// What TDLib sends for the file.
[[nodiscard]] Td::api::object_ptr<Td::api::InputMessageContent> AttachContent(const AttachFile &file, bool compress,
                                                                              const std::string &caption);

// Length in UTF-16 units, as Telegram counts the caption.
[[nodiscard]] int CaptionLength(const std::string &text);

}  // namespace Contrail
