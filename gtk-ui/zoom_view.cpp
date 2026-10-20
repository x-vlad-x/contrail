#include "zoom_view.hpp"

#include "zoom_geometry.hpp"

#include <cmath>

namespace Contrail {
namespace {

struct ZoomData {
  GdkTexture *texture = nullptr;
  Zoom::Size image;
  Zoom::Size view;
  Zoom::State state;
  // A pinch going on: the state and the point between the fingers it started from.
  Zoom::State pinchStart;
  Zoom::Point pinchCenter;
  bool pinching = false;
  // A drag going on: how far it went already (it goes on with one finger after a pinch).
  Zoom::Point dragged;
  Zoom::Point pointer;
};

}  // namespace
}  // namespace Contrail

struct _ContrailZoomView {
  GtkWidget parent_instance;
  Contrail::ZoomData *data;
};

G_DECLARE_FINAL_TYPE(ContrailZoomView, contrail_zoom_view, CONTRAIL, ZOOM_VIEW, GtkWidget)
G_DEFINE_FINAL_TYPE(ContrailZoomView, contrail_zoom_view, GTK_TYPE_WIDGET)

namespace Contrail {
namespace {

ZoomData &Data(GtkWidget *widget) {
  return *CONTRAIL_ZOOM_VIEW(widget)->data;
}

void Apply(GtkWidget *widget, const Zoom::State &state) {
  auto &data = Data(widget);
  data.state = Zoom::Clamp(state, data.image, data.view);
  gtk_widget_queue_draw(widget);
}

Zoom::Point Center(GtkGesture *gesture) {
  double x = 0, y = 0;
  gtk_gesture_get_bounding_box_center(gesture, &x, &y);
  return {x, y};
}

void SetupGestures(GtkWidget *widget) {
  auto *pinch = gtk_gesture_zoom_new();
  g_signal_connect(pinch, "begin", G_CALLBACK((+[](GtkGesture *gesture, GdkEventSequence *, GtkWidget *widget) {
    auto &data = Data(widget);
    data.pinching = true;
    data.pinchStart = data.state;
    data.pinchCenter = Center(gesture);
  })), widget);
  g_signal_connect(pinch, "scale-changed", G_CALLBACK((+[](GtkGestureZoom *gesture, double scale, GtkWidget *widget) {
    auto &data = Data(widget);
    if (!data.pinching) return;
    Apply(widget, Zoom::Pinch(data.pinchStart, data.pinchCenter, scale, Center(GTK_GESTURE(gesture)), data.image,
                              data.view));
  })), widget);
  g_signal_connect(pinch, "end", G_CALLBACK((+[](GtkGesture *, GdkEventSequence *, GtkWidget *widget) {
    Data(widget).pinching = false;
  })), widget);
  g_signal_connect(pinch, "cancel", G_CALLBACK((+[](GtkGesture *, GdkEventSequence *, GtkWidget *widget) {
    Data(widget).pinching = false;
  })), widget);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(pinch));

  // Moving a zoomed photo. A second finger turns the drag into a pinch.
  auto *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK((+[](GtkGestureDrag *, double, double, GtkWidget *widget) {
    Data(widget).dragged = {};
  })), widget);
  g_signal_connect(drag, "drag-update", G_CALLBACK((+[](GtkGestureDrag *, double dx, double dy, GtkWidget *widget) {
    auto &data = Data(widget);
    const auto delta = Zoom::Point{dx - data.dragged.x, dy - data.dragged.y};
    data.dragged = {dx, dy};
    // While pinching the fingers move the photo already.
    if (data.pinching || data.state.scale <= Zoom::kMinScale) return;
    Apply(widget, Zoom::Pan(data.state, delta, data.image, data.view));
  })), widget);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
  gtk_gesture_group(GTK_GESTURE(pinch), GTK_GESTURE(drag));

  auto *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK((+[](GtkGestureClick *, int count, double x, double y, GtkWidget *widget) {
    if (count != 2) return;
    auto &data = Data(widget);
    Apply(widget, Zoom::DoubleTap(data.state, {x, y}, data.image, data.view));
  })), widget);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));

  // With a mouse: Ctrl+wheel (or a touchpad's pinch, sent as one) zooms around the pointer.
  auto *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK((+[](GtkEventControllerMotion *, double x, double y, GtkWidget *widget) {
    Data(widget).pointer = {x, y};
  })), widget);
  gtk_widget_add_controller(widget, motion);
  auto *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll, "scroll", G_CALLBACK((+[](GtkEventControllerScroll *controller, double, double dy, GtkWidget *widget) -> gboolean {
    const auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    auto &data = Data(widget);
    const auto anchor = Zoom::ImagePointAt(data.state, data.image, data.view, data.pointer);
    Apply(widget, Zoom::Place(data.state.scale * std::pow(1.15, -dy), anchor, data.pointer, data.image, data.view));
    return TRUE;
  })), widget);
  gtk_widget_add_controller(widget, scroll);
}

}  // namespace

GtkWidget *ZoomViewNew() {
  return GTK_WIDGET(g_object_new(contrail_zoom_view_get_type(), nullptr));
}

void ZoomViewSetPhoto(GtkWidget *view, GdkTexture *texture, int width, int height) {
  auto &data = Data(view);
  g_set_object(&data.texture, texture);
  data.image = {double(std::max(width, 1)), double(std::max(height, 1))};
  data.state = Zoom::State();
  gtk_widget_queue_draw(view);
}

void ZoomViewSetTexture(GtkWidget *view, GdkTexture *texture) {
  g_set_object(&Data(view).texture, texture);
  gtk_widget_queue_draw(view);
}

GtkWidget *ZoomViewFind(GtkWidget *root) {
  if (!root) return nullptr;
  if (G_TYPE_CHECK_INSTANCE_TYPE(root, contrail_zoom_view_get_type())) return root;
  for (auto *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    if (auto *found = ZoomViewFind(child)) return found;
  }
  return nullptr;
}

std::string ZoomViewState(GtkWidget *view) {
  const auto &data = Data(view);
  const auto rect = Zoom::Displayed(data.state, data.image, data.view);
  const auto middle = Zoom::ImagePointAt(data.state, data.image, data.view, {data.view.width / 2., data.view.height / 2.});
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "scale=%.3f photo=%.1f,%.1f %.1fx%.1f view=%.0fx%.0f middle=%.4f,%.4f texture=%dx%d",
           data.state.scale, rect.x, rect.y, rect.width, rect.height, data.view.width, data.view.height, middle.x,
           middle.y, data.texture ? gdk_texture_get_width(data.texture) : 0,
           data.texture ? gdk_texture_get_height(data.texture) : 0);
  return buffer;
}

}  // namespace Contrail

static void contrail_zoom_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  const auto &data = Contrail::Data(widget);
  if (!data.texture) return;
  const auto rect = Contrail::Zoom::Displayed(data.state, data.image, data.view);
  const auto bounds = graphene_rect_t{{float(rect.x), float(rect.y)}, {float(rect.width), float(rect.height)}};
  // Mipmapped: a 2560 px photo shown small stays sharp, without shimmering while it is zoomed.
  gtk_snapshot_append_scaled_texture(snapshot, data.texture, GSK_SCALING_FILTER_TRILINEAR, &bounds);
}

static void contrail_zoom_view_size_allocate(GtkWidget *widget, int width, int height, int) {
  auto &data = Contrail::Data(widget);
  const auto size = Contrail::Zoom::Size{double(width), double(height)};
  if (size.width != data.view.width || size.height != data.view.height) {
    data.state = Contrail::Zoom::Resize(data.state, data.image, data.view, size);
    data.view = size;
  }
}

static void contrail_zoom_view_measure(GtkWidget *, GtkOrientation, int, int *minimum, int *natural, int *, int *) {
  *minimum = *natural = 0;
}

static void contrail_zoom_view_finalize(GObject *object) {
  auto *self = CONTRAIL_ZOOM_VIEW(object);
  g_clear_object(&self->data->texture);
  delete self->data;
  G_OBJECT_CLASS(contrail_zoom_view_parent_class)->finalize(object);
}

static void contrail_zoom_view_class_init(ContrailZoomViewClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = contrail_zoom_view_finalize;
  auto *widget = GTK_WIDGET_CLASS(klass);
  widget->snapshot = contrail_zoom_view_snapshot;
  widget->size_allocate = contrail_zoom_view_size_allocate;
  widget->measure = contrail_zoom_view_measure;
  gtk_widget_class_set_css_name(widget, "zoom-view");
}

static void contrail_zoom_view_init(ContrailZoomView *self) {
  self->data = new Contrail::ZoomData();
  auto *widget = GTK_WIDGET(self);
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_widget_set_vexpand(widget, TRUE);
  Contrail::SetupGestures(widget);
}
