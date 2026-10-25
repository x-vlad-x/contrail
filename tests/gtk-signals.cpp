#include "app.hpp"
#include "layout.hpp"
#include "call_layout.hpp"
#include "scroll_helpers.hpp"

using Contrail::SignalConnections;

static int coreEventDepth = 0;

static void nativeSignalsEnterCore() {
  const auto previous = Contrail::coreEventDispatcher;
  Contrail::coreEventDispatcher = [](const std::function<void()> &fn) {
    ++coreEventDepth;
    fn();
    --coreEventDepth;
  };
  auto *button = gtk_button_new();
  auto *label = gtk_label_new("");
  g_object_ref_sink(button);
  g_object_ref_sink(label);
  auto clicks = 0;
  Contrail::OnClicked(button, [&] { g_assert_cmpint(coreEventDepth, >, 0); ++clicks; });
  SignalConnections connections;
  connections.connect<gboolean(GtkLabel *, const char *)>(label, "activate-link", [](GtkLabel *, const char *) -> gboolean {
    g_assert_cmpint(coreEventDepth, >, 0);
    return TRUE;
  });
  g_signal_emit_by_name(button, "clicked");
  gboolean handled = FALSE;
  g_signal_emit_by_name(label, "activate-link", "test:", &handled);
  g_assert_true(handled);
  g_assert_cmpint(clicks, ==, 1);
  g_assert_cmpint(coreEventDepth, ==, 0);
  connections.clear();
  g_object_unref(button);
  g_object_unref(label);
  Contrail::coreEventDispatcher = previous;
}

// GTK can keep emitters alive while a navigation transition releases their C++ view.
static void disconnectedView() {
  auto *adjustment = gtk_adjustment_new(0, 0, 100, 1, 10, 10);
  g_object_ref_sink(adjustment);
  auto hits = 0;
  {
    SignalConnections view;
    view.connect<void(GtkAdjustment *)>(adjustment, "value-changed", [&](GtkAdjustment *) { ++hits; });
    gtk_adjustment_set_value(adjustment, 10);
    g_assert_cmpint(hits, ==, 1);
  }
  gtk_adjustment_set_value(adjustment, 20);
  g_assert_cmpint(hits, ==, 1);
  g_object_unref(adjustment);
}

// A signal can close its own view; its callable must survive closure destruction.
static void reentrantTeardown() {
  auto *button = gtk_button_new();
  g_object_ref_sink(button);
  SignalConnections view;
  auto state = std::make_shared<int>(42);
  auto completed = false;
  view.clicked(button, [&, state] {
    view.clear();
    g_assert_cmpint(*state, ==, 42);
    completed = true;
  });
  state.reset();
  g_signal_emit_by_name(button, "clicked");
  g_assert_true(completed);
  completed = false;
  g_signal_emit_by_name(button, "clicked");
  g_assert_false(completed);
  view.clear(); // Explicit teardown followed by destruction is safe.
  g_object_unref(button);
}

static void emitterReleasedBeforeView() {
  auto *button = gtk_button_new();
  g_object_ref_sink(button);
  auto *weak = G_OBJECT(button);
  g_object_add_weak_pointer(G_OBJECT(button), reinterpret_cast<gpointer *>(&weak));
  SignalConnections view;
  view.clicked(button, [] {});
  g_object_unref(button);
  g_assert_nonnull(weak);
  view.clear();
  g_assert_null(weak);
}

static void longMessageFitsPhone() {
  auto *label = gtk_label_new(std::string(4096, 'W').c_str());
  g_object_ref_sink(label);
  Contrail::ConfigureMessageWrapping(GTK_LABEL(label));
  int minimum = 0, natural = 0;
  gtk_widget_measure(label, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, nullptr, nullptr);
  g_assert_cmpint(minimum, <, 200);
  g_assert_cmpint(natural, <, 800);
  gtk_widget_measure(label, GTK_ORIENTATION_VERTICAL, 200, &minimum, &natural, nullptr, nullptr);
  g_assert_cmpint(minimum, >, 40); // The entire unbroken message wraps to multiple lines.
  g_object_unref(label);
}

// A bubble aligned to one side of a wide chat, with something that expands in it (the voice player's
// column): it gets the height its text needs at the width it is given, so the text stays inside it.
static void bubbleKeepsTextInside() {
  auto *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  g_object_ref_sink(line);
  gtk_widget_set_halign(line, GTK_ALIGN_START);
  auto *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  auto *bin = Contrail::MakeHeightForWidthBin(bubble);
  gtk_box_append(GTK_BOX(line), bin);
  auto *player = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(player, TRUE);
  gtk_box_append(GTK_BOX(player), gtk_progress_bar_new());
  gtk_box_append(GTK_BOX(bubble), player);
  std::string text;
  for (auto i = 0; i != 60; ++i) text += "Words wrap differently at every width of the bubble. ";
  auto *label = gtk_label_new(text.c_str());
  Contrail::ConfigureMessageWrapping(GTK_LABEL(label));
  gtk_box_append(GTK_BOX(bubble), label);
  for (const auto width : {360, 600, 900, 1400}) {
    int height = 0;
    gtk_widget_measure(line, GTK_ORIENTATION_VERTICAL, width, &height, nullptr, nullptr, nullptr);
    gtk_widget_allocate(line, width, height, -1, nullptr);
    int needed = 0;
    gtk_widget_measure(bubble, GTK_ORIENTATION_VERTICAL, gtk_widget_get_width(bin), &needed, nullptr, nullptr, nullptr);
    g_assert_cmpint(gtk_widget_get_height(bin), >=, needed);
  }
  g_object_unref(line);
}

static void mediaFitsPhone() {
  auto *picture = Contrail::MakeMediaPicture(280, 210);
  auto *frame = Contrail::MakeMediaFrame(picture, 280, 210);
  g_object_ref_sink(frame);
  // A tiny inline thumbnail, then a downloaded 4K image: the size never follows the picture.
  for (const auto [w, h] : {std::pair{0, 0}, std::pair{40, 30}, std::pair{3840, 2160}}) {
    if (w) {
      auto *image = gdk_paintable_new_empty(w, h);
      gtk_picture_set_paintable(GTK_PICTURE(picture), image);
      g_object_unref(image);
    }
    int minimum = 0, natural = 0;
    gtk_widget_measure(frame, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, nullptr, nullptr);
    g_assert_cmpint(minimum, <=, 180);  // Fits a phone.
    g_assert_cmpint(natural, ==, 280);
    // GtkListBox gives rows their minimum height: it must be the real one, not 0.
    gtk_widget_measure(frame, GTK_ORIENTATION_VERTICAL, 280, &minimum, &natural, nullptr, nullptr);
    g_assert_cmpint(minimum, ==, 210);
    gtk_widget_measure(frame, GTK_ORIENTATION_VERTICAL, 180, &minimum, &natural, nullptr, nullptr);
    g_assert_cmpint(minimum, ==, 135);
    gtk_widget_allocate(frame, 180, 135, -1, nullptr);
    g_assert_cmpint(gtk_widget_get_width(picture), ==, 180);
    g_assert_cmpint(gtk_widget_get_height(picture), ==, 135);
  }
  g_object_unref(frame);
}

static void labelLinksLeaveTouchesToScrolling() {
  auto *label = gtk_label_new(nullptr);
  g_object_ref_sink(label);
  auto *own = gtk_gesture_click_new();
  g_object_set_data(G_OBJECT(own), "contrail-own", GINT_TO_POINTER(1));
  gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(own));
  for (auto i = 0; i != 2; ++i) {  // Every markup with links brings the label's own gestures back.
    gtk_label_set_markup(GTK_LABEL(label), "text <a href=\"https://example.com\">link</a> text");
    Contrail::DisableLabelGestures(label);
    auto *controllers = gtk_widget_observe_controllers(label);
    auto gestures = 0;
    for (guint j = 0; j != g_list_model_get_n_items(controllers); ++j) {
      auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, j));
      if (GTK_IS_GESTURE(controller)) {
        ++gestures;
        const auto phase = gtk_event_controller_get_propagation_phase(controller);
        g_assert_cmpint(phase, ==, (controller == GTK_EVENT_CONTROLLER(own)) ? GTK_PHASE_BUBBLE : GTK_PHASE_NONE);
      }
      g_object_unref(controller);
    }
    g_object_unref(controllers);
    g_assert_cmpint(gestures, >=, 3);  // Ours, GTK's click and selection drag.
  }
  g_object_unref(label);
}

static void tapGuard() {
  // Without a press there is nothing to block.
  Contrail::tapState = {};
  g_assert_true(Contrail::TapAllowed());
  Contrail::tapState = {10, 10, true, false, false};
  g_assert_true(Contrail::TapAllowed());
  Contrail::tapState.moved = true;  // A flick shorter than GTK's drag threshold.
  g_assert_false(Contrail::TapAllowed());
  Contrail::lastListMotion = 0;
  g_assert_false(Contrail::ListMoving());
  Contrail::NoteListMotion();
  g_assert_true(Contrail::ListMoving());  // A press now only stops the fling.
}

static void callPreviewStaysSmall() {
  auto *remote = gtk_picture_new();
  auto *local = gtk_picture_new();
  GtkWidget *preview = nullptr;
  auto *stage = Contrail::MakeCallVideoStage(remote, local, &preview);
  g_object_ref_sink(stage);
  auto *image = gdk_paintable_new_empty(1080, 1920);
  gtk_picture_set_paintable(GTK_PICTURE(remote), image);
  gtk_picture_set_paintable(GTK_PICTURE(local), image);
  g_object_unref(image);
  for (const auto width : {320, 413, 900}) {
    gtk_widget_allocate(stage, width, 600, -1, nullptr);
    g_assert_cmpint(gtk_widget_get_width(remote), ==, width);
    g_assert_cmpint(gtk_widget_get_height(remote), ==, 600);
    g_assert_cmpint(gtk_widget_get_width(preview), <=, 96);
    g_assert_cmpint(gtk_widget_get_height(preview), <=, 144);
  }
  g_object_unref(stage);
}

static void scrollCoordinatesIncludeCurrentOffset() {
  auto *scroll = gtk_scrolled_window_new();
  g_object_ref_sink(scroll);
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *target = nullptr;
  for (auto i = 0; i != 15; ++i) {
    auto *row = gtk_drawing_area_new();
    gtk_widget_set_size_request(row, -1, 100);
    gtk_box_append(GTK_BOX(box), row);
    if (i == 6) target = row;
  }
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  gtk_widget_allocate(scroll, 320, 300, -1, nullptr);
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
  gtk_adjustment_set_value(adjustment, 400);
  gtk_widget_allocate(scroll, 320, 300, -1, nullptr);
  double y = 0;
  g_assert_true(Contrail::WidgetViewportY(target, scroll, y));
  g_assert_cmpfloat(y, ==, 200);
  g_assert_true(Contrail::ScrollWidgetToY(target, scroll, adjustment, 100));
  g_assert_cmpfloat(gtk_adjustment_get_value(adjustment), ==, 500);

  gtk_widget_allocate(scroll, 320, 300, -1, nullptr);
  g_assert_true(Contrail::WidgetViewportY(target, scroll, y));
  const auto anchorY = y;
  for (auto i = 0; i != 3; ++i) {
    auto *older = gtk_drawing_area_new();
    gtk_widget_set_size_request(older, -1, 100);
    gtk_box_prepend(GTK_BOX(box), older);
  }
  gtk_widget_allocate(scroll, 320, 300, -1, nullptr);
  g_assert_true(Contrail::ScrollWidgetToY(target, scroll, adjustment, anchorY));
  g_assert_cmpfloat(gtk_adjustment_get_value(adjustment), ==, 800);
  g_object_unref(scroll);
}

int main(int argc, char **argv) {
  gtk_init();
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/gtk/view/disconnect", disconnectedView);
  g_test_add_func("/gtk/core/native-event-entry", nativeSignalsEnterCore);
  g_test_add_func("/gtk/view/reentrant-teardown", reentrantTeardown);
  g_test_add_func("/gtk/view/emitter-lifetime", emitterReleasedBeforeView);
  g_test_add_func("/gtk/layout/long-message", longMessageFitsPhone);
  g_test_add_func("/gtk/layout/media", mediaFitsPhone);
  g_test_add_func("/gtk/layout/bubble-height", bubbleKeepsTextInside);
  g_test_add_func("/gtk/scroll/tap-guard", tapGuard);
  g_test_add_func("/gtk/scroll/label-links", labelLinksLeaveTouchesToScrolling);
  g_test_add_func("/gtk/layout/call-preview", callPreviewStaysSmall);
  g_test_add_func("/gtk/scroll/offset-and-prepend-anchor", scrollCoordinatesIncludeCurrentOffset);
  return g_test_run();
}
