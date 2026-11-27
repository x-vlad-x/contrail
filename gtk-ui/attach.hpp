#pragma once

#include "app.hpp"
#include "attach_media.hpp"
#include "paste.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Contrail {

// Reads the clipboard in the background: the files, else the picture (with the text that came with it), else the
// text. Destroying the reader cancels the reading, so an answer that comes later is dropped.
class ClipboardReader final {
 public:
  struct Result {
    PastedFiles files;
    GdkTexture *image = nullptr;  // Owned by the result.
    std::string text;
    Result() = default;
    Result(Result &&other) noexcept;
    Result &operator=(Result &&other) noexcept;
    ~Result();
  };
  ClipboardReader(GdkClipboard *clipboard, PasteKind kind, std::function<void(Result &&)> done);
  ~ClipboardReader();
  ClipboardReader(const ClipboardReader &) = delete;
  ClipboardReader &operator=(const ClipboardReader &) = delete;

  struct Pending;

 private:
  Pending *_pending = nullptr;
};

// The screen before files are sent, for files from the file chooser and from the clipboard: previews (prepared in
// the background), removing single files, the caption, "Send as documents" and "Group items". Nothing is sent
// before the Send button. Closing it cancels everything not prepared yet.
class AttachDialog final {
 public:
  struct Args {
    GtkWidget *parent = nullptr;
    std::vector<std::string> paths;
    GdkTexture *image = nullptr;  // A pasted picture, instead of paths; taken over.
    std::string caption;          // Taken from the message field.
    bool premium = false;
    // Sends the files or shows why not and leaves them here: false.
    std::function<bool(const std::vector<AttachFile> &files, bool compress, bool group, const std::string &caption)>
        send;
    std::function<void()> cancelled;
  };
  // The dialog lives as long as its window; the pointer returned is for closing it early.
  static std::weak_ptr<AttachDialog> Show(Args &&args);
  explicit AttachDialog(Args &&args);
  ~AttachDialog();
  AttachDialog(const AttachDialog &) = delete;
  AttachDialog &operator=(const AttachDialog &) = delete;

  void close();
  // For the debug actions: "send", "cancel", "documents on|off", "group on|off", "caption TEXT", "remove N";
  // anything else (and each of these) prints the state.
  void debugAction(const std::string &action);

  struct Entry;

 private:
  void build();
  void prepare(const std::vector<std::string> &paths);
  void prepareImage(GdkTexture *image);
  void startPreparing(const std::string &path, GdkTexture *image);
  void prepared(std::int64_t id, AttachFile file, GdkTexture *preview);
  void addEntry(std::int64_t id, const std::string &name);
  void fillTile(Entry &entry, GdkTexture *preview);
  void remove(std::int64_t id);
  void refresh();
  void send();
  [[nodiscard]] std::string caption() const;

  Args _args;
  std::shared_ptr<bool> _alive = std::make_shared<bool>(true);  // What finishes in the background checks it.
  GCancellable *_cancellable = nullptr;
  std::string _directory;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_flow = nullptr;
  GtkWidget *_refused = nullptr;  // Why some files can't be sent.
  GtkWidget *_sendButton = nullptr;
  GtkWidget *_asDocuments = nullptr;
  GtkWidget *_group = nullptr;
  GtkWidget *_caption = nullptr;
  std::vector<std::unique_ptr<Entry>> _entries;
  std::int64_t _nextId = 0;
  bool _sent = false;
  bool _closing = false;
  SignalConnections _signals;
};

}  // namespace Contrail
