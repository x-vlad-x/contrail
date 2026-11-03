#pragma once

#include "app.hpp"
#include "td/client.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <string>

namespace Contrail {

class ChatView;
class VoiceRecorder;

// A voice message recorded from the composer, as in 0.3.1 and the phone apps: with an empty field the send button
// is a microphone, recording while it is held. Let go, it is sent; slid left, deleted; slid up, it records on
// without the finger, with Delete and Send beside the time. A short tap only says to hold it. The recording itself
// is the window's (media/voice_recorder.hpp): the chat closed, it goes on, and this chat shows it again.
class VoiceRecordBar final {
 public:
  // `sendButton` goes into a stack with the microphone, where it was. The bar is added to `composerStack` as
  // "record"; the gesture is on `composer`, which stays under the finger when the bar takes the field's place. The
  // lock rises over the chat, in the overlay of `toBottom`, which is hidden while the finger holds.
  VoiceRecordBar(ChatView &chat, GtkWidget *composer, GtkWidget *composerStack, GtkWidget *sendButton,
                 GtkWidget *toBottom);
  ~VoiceRecordBar();
  VoiceRecordBar(const VoiceRecordBar &) = delete;
  VoiceRecordBar &operator=(const VoiceRecordBar &) = delete;

  // The microphone stands in for the send button while the field is empty and a voice message may be recorded.
  void setFieldEmpty(bool empty);
  [[nodiscard]] bool recording() const { return _recording; }
  // The composer shows the field again: a recording keeps its bar, unless the chat can't be written to any more.
  void composerShown(bool writable);
  // Escape: the recording is deleted. False when nothing records.
  bool cancel();
  // For the debug commands: state | start (as the keyboard does: locked) | hold | drag DX DY | release | send |
  // delete.
  void debug(const std::string &what);

 private:
  [[nodiscard]] VoiceRecorder *recorder() const;
  void setupGesture(GtkWidget *composer);
  void start();
  void attach();
  void lock();
  void showState();
  void drag(double dx, double dy);
  void finish(bool send);
  void release();

  ChatView &_chat;
  GtkWidget *_composer = nullptr;
  GtkWidget *_composerStack = nullptr;
  GtkWidget *_sendStack = nullptr;
  GtkWidget *_mic = nullptr;
  GtkWidget *_toBottom = nullptr;
  GtkWidget *_bar = nullptr;
  GtkWidget *_cancel = nullptr;
  GtkWidget *_dot = nullptr;
  GtkWidget *_time = nullptr;
  GtkWidget *_hint = nullptr;
  GtkWidget *_send = nullptr;
  GtkWidget *_micIndicator = nullptr;
  GtkWidget *_lockHint = nullptr;
  bool _recording = false;
  bool _holding = false;  // The finger is on the microphone.
  bool _locked = false;
  bool _fieldEmpty = true;
  gint64 _started = 0;
  Td::Lifetime _recordLifetime;
  SignalConnections _signals;
};

}  // namespace Contrail
