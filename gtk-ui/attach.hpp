#pragma once

#include "app.hpp"
#include "paste.hpp"

#include "base/weak_ptr.h"

#include <QtCore/QStringList>

#include <memory>
#include <vector>

namespace Ui {
struct PreparedFile;
struct PreparedList;
class SendFilesWay;
} // namespace Ui

namespace Contrail {

// Reads the clipboard in the background: the files, else the picture (with the text that came with it),
// else the text. Destroying the reader cancels the reading, so an answer that comes later is dropped.
class ClipboardReader final {
public:
  struct Result {
    PastedFiles files;
    QImage image;
    QString text;
  };
  ClipboardReader(GdkClipboard *clipboard, PasteKind kind, std::function<void(Result &&)> done);
  ~ClipboardReader();
  ClipboardReader(const ClipboardReader &) = delete;
  ClipboardReader &operator=(const ClipboardReader &) = delete;

  struct Pending;

private:
  Pending *_pending = nullptr;
};

// The screen before files are sent, for files from the file chooser and from the clipboard: previews
// (prepared in the background), removing single files, the caption, "Send as documents" and "Group
// items". Nothing is sent before the Send button. Closing it cancels everything not prepared yet.
class AttachDialog final : public base::has_weak_ptr {
public:
  struct Args {
    GtkWidget *parent = nullptr;
    QStringList paths;
    QImage image;           // A pasted picture, instead of paths.
    QString caption;        // Taken from the message field.
    bool premium = false;
    // Sends the files (taking them out of the list) or shows why not and leaves the list as it was.
    std::function<bool(Ui::PreparedList &list, const Ui::SendFilesWay &way, const QString &caption)> send;
    std::function<void()> cancelled;
  };
  // The dialog lives as long as its window; the returned pointer is for closing it early.
  static base::weak_ptr<AttachDialog> Show(Args &&args);
  ~AttachDialog();

  void close();
  // For the debug actions: "send", "cancel", "documents on|off", "group on|off", "state".
  void debugAction(const QString &action);

private:
  struct Entry;

  explicit AttachDialog(Args &&args);
  void build();
  void prepare(const QStringList &paths);
  void prepareImage(QImage &&image);
  void startPreparing(std::unique_ptr<Ui::PreparedFile> file);
  void prepared(int64 id, std::unique_ptr<Ui::PreparedFile> file);
  void addEntry(int64 id, const QString &name);
  void fillTile(Entry &entry);
  void remove(int64 id);
  void refresh();
  void send();

  Args _args;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_flow = nullptr;
  GtkWidget *_refused = nullptr;  // Why some files can't be sent.
  GtkWidget *_sendButton = nullptr;
  GtkWidget *_asDocuments = nullptr;
  GtkWidget *_group = nullptr;
  GtkWidget *_caption = nullptr;
  std::vector<std::unique_ptr<Entry>> _entries;
  int64 _nextId = 0;
  bool _sent = false;
  SignalConnections _signals;
};

}  // namespace Contrail
