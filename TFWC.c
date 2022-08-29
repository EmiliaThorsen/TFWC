#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define compositorKey WLR_MODIFIER_LOGO

struct TFWCServer {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;

    struct wlr_xdg_shell *xdgShell;
    struct wl_listener newXdgWindow;
    struct wl_list views;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursorManager;
    struct wl_listener cursorMotion;
    struct wl_listener cursorMotionAbsolute;
    struct wl_listener cursorButton;
    struct wl_listener cursorAxis;
    struct wl_listener cursorFrame;

    struct wlr_seat *seat;
    struct wl_listener newInput;
    struct wl_listener requestCursor;
    struct wl_listener requestSetSelection;
    struct wl_list keyboards;

    struct wlr_output_layout *outputLayout;
    struct wl_list outputs;
    struct wl_listener newOutput;
};

struct TFWCOutput {
    struct wl_list link;
    struct TFWCServer *server;
    struct wlr_output *wlrOutput;
    struct wl_listener frame;
};

struct TFWCKeyboard {
    struct wl_list link;
    struct TFWCServer *server;
    struct wlr_input_device *device;
    struct wl_listener modifiers;
    struct wl_listener key;
};

struct TFWCView {
    struct wl_list link;
    struct TFWCServer *server;
    struct wlr_xdg_surface *xdgSurface;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener requestMove;
    struct wl_listener requestResize;
    bool mapped;
    int x, y;
};

//helper functions

//function to check if a window is at a coordinate
static bool view_at(struct TFWCView *view, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
    double view_sx = lx - view->x;
    double view_sy = ly - view->y;
    double _sx, _sy;
    struct wlr_surface *_surface = NULL;
    _surface = wlr_xdg_surface_surface_at(view->xdgSurface, view_sx, view_sy, &_sx, &_sy);
    if (_surface != NULL) {
        *sx = _sx;
        *sy = _sy;
        *surface = _surface;
        return true;
    }
    return false;
}

//function to check what window is at some coordinate
static struct TFWCView *desktop_view_at(
    struct TFWCServer *server, double lx, double ly,
    struct wlr_surface **surface, double *sx, double *sy) {
    struct TFWMView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view_at(view, lx, ly, surface, sx, sy)) {
            return view;
        }
    }
    return NULL;
}

//setter for window position
void setWindowPosition(struct TFWMView *view, int x, int y) {
    view.x = x;
    view.y = y;
}

//setter for window size
void setWindowSize(struct TFWMView *view, int w, int h) {
    struct wlr_box geo_box;
    wlr_xdg_surface_get_geometry(view->xdgSurface, &geo_box);
    wlr_xdg_toplevel_set_size(view->xdgSurface, w, h);
}

//keyboard stuff

//keyboard modifier key event handling
static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct TFWCKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->device->keyboard->modifiers);
}

//keyboard key press event handeler
static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct TFWCKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct TFWCServer *server = keyboard->server;
    struct wlr_event_keyboard_key *event = data;
    struct wlr_seat *seat = server->seat;

    //handle keycode stuff
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->device->keyboard->xkb_state, keycode, &syms);
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);

    //all events get passed to the focused program if your key isnt held down for compositor keybinds
    if(modifiers & compositorKey) {
        //handle compositor keybinds here
    } else {
        //all other keypresses get sent to the current program
        wlr_seat_set_keyboard(seat, keyboard->device);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

//setting window focus for keyboard inputs
static void focus_view(struct TFWCView *view, struct wlr_surface *surface) {
    if(view == NULL) return;
    struct TFWCServer *server = view->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) return;

    //deactivate previous window
    if (prev_surface) {
        struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(seat->keyboard_state.focused_surface);
        wlr_xdg_toplevel_set_activated(previous, false);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);

    //move the window to the top
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);

    //activate new surface
    wlr_xdg_toplevel_set_activated(view->xdgSurface, true);
    wlr_seat_keyboard_notify_enter(seat, view->xdgSurface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

//handle clipboard setting requests
static void seatRequestSetSelection(struct wl_listener *listener, void *data) {
    struct TFWMServer *server = wl_container_of(listener, server, requestSetSelection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

//mouse stuff

//handle programs sending custom cursor images
static void seatRequestCursor(struct wl_listener *listener, void *data) {
    struct TFWCServer *server = wl_container_of(listener, server, requestCursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if(focused_client == event->seat_client) wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

//handle cursor motion events
static void processCursorMotion(struct TFWCServer *server, uint32_t time) {
    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct TFWMView *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (!view) { //if cursor isnt over a window will it default to the normal pointer
        wlr_xcursor_manager_set_cursor_image(server->cursorManager, "left_ptr", server->cursor);
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

//handle relative cursor movement envents
static void serverCursorMotion(struct wl_listener *listener, void *data) {
    struct TFWMServer *server = wl_container_of(listener, server, cursorMotion);
    struct wlr_event_pointer_motion *event = data;
    wlr_cursor_move(server->cursor, event->device, event->delta_x, event->delta_y);
    processCursorMotion(server, event->time_msec);
}

//handle absolute cursor events, like drawing pads send for example
static void serverCursorMotionAbsolute(struct wl_listener *listener, void *data) {
    struct TFWCServer *server = wl_container_of(listener, server, cursorMotionAbsolute);
    struct wlr_event_pointer_motion_absolute *event = data;
    wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
    processCursorMotion(server, event->time_msec);
}

//handle mouse clicks
static void serverCursorButton(struct wl_listener *listener, void *data) {
    struct TFWMServer *server = wl_container_of(listener, server, cursorButton);
    struct wlr_event_pointer_button *event = data;
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    double sx, sy;
    struct wlr_surface *surface;
    struct TFWCView *view = desktop_view_at(server,server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    focus_view(view, surface);
}

//handle frame events
static void serverCursorFrame(struct wl_listener *listener, void *data) {
    struct TFWMServer *server = wl_container_of(listener, server, cursorFrame);
    wlr_seat_pointer_notify_frame(server->seat);
}

//handle axis events such as scrolling
static void serverCursorAxis(struct wl_listener *listener, void *data) {
    struct TFWCServer *server = wl_container_of(listener, server, cursorAxis);
    struct wlr_event_pointer_axis *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source);
}

//window rendering functions

//main window rendering function
static void renderWindow(struct wlr_surface *surface, int sx, int sy, void *data) {
    struct render_data *rdata = data;
    struct TFWCView *view = rdata->view;
    struct wlr_output *output = rdata->output;

    //we get the texture of the window, basicly what it contains
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if(texture == NULL) return;

    //we get the specific screen coordinates from the whole desktop coordinates
    double ox = 0, oy = 0;
    wlr_output_layout_output_coords(view->server->outputLayout, output, &ox, &oy);
    ox += view->x + sx, oy += view->y + sy;

    //we scale the window to the corect dpi
    struct wlr_box box = {
        .x = ox * output->scale,
        .y = oy * output->scale,
        .width = surface->current.width * output->scale,
        .height = surface->current.height * output->scale,
    };

    //we define a matrix transform for rendering the texture to the screen
    float matrix[9];
    enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
    wlr_matrix_project_box(matrix, &box, transform, 0, output->transform_matrix);

    //we render the window to the screen
    wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);

    //and then lastly tell wl that its rendered
    wlr_surface_send_frame_done(surface, rdata->when);
}

//main rendering function
static void renderScreen(struct wl_listener *listener, void *data) {
    struct TFWCOutput *output = wl_container_of(listener, output, frame);
    struct wlr_renderer *renderer = output->server->renderer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    //we initiate renderer stuff
    if (!wlr_output_attach_render(output->wlrOutput, NULL)) return;
    int width, height;
    wlr_output_effective_resolution(output->wlrOutput, &width, &height);
    wlr_renderer_begin(renderer, width, height);

    //backgrownd is set to a solid color
    float color[4] = {0.3, 0.3, 0.3, 1.0};
    wlr_renderer_clear(renderer, color);

    struct TFWCView *view;
    wl_list_for_each_reverse(view, &output->server->views, link) {
        if(!view->mapped) continue;
        struct render_data rdata = {
                .output = output->wlrOutput,
                .view = view,
                .renderer = renderer,
                .when = &now,
        };
        wlr_xdg_surface_for_each_surface(view->xdgSurface, renderWindow, &rdata);
    }

    //we render the cursor
    wlr_output_render_software_cursors(output->wlrOutput, NULL);

    wlr_renderer_end(renderer);
    wlr_output_commit(output->wlrOutput);
}

//called when a window should be shown
static void xdgUnHideWindow(struct wl_listener *listener, void *data) {
    struct TFWCView *view = wl_container_of(listener, view, map);
    view->mapped = true;
}

//called when a window shouldnt be renderered
static void xdgHideWindow(struct wl_listener *listener, void *data) {
    struct TFWCView *view = wl_container_of(listener, view, unmap);
    view->mapped = false;
}

//clalled when a window shouldnt be rendered again at all and be freed from memory
static void xdgDestroyWindow(struct wl_listener *listener, void *data) {
    struct TFWCView *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

//called when a window tires to move it self
static void xdgHandleToplevelMoveRequest(struct wl_listener *listener, void *data) {
    struct TFWCView *view = wl_container_of(listener, view, requestMove);
    struct TFWCServer *server = view->server;
    struct wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
    if(view->xdgSurface->surface != focused_surface) return;
    server->grabbed_view = view;
    server->grab_x = server->cursor->x - view->x;
    server->grab_y = server->cursor->y - view->y;
}

//called when a window tries to resize it self
static void xdgHandleToplevelResizeRequest(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct TFWCView *view = wl_container_of(listener, view, requestResize);
    struct TFWCServer *server = view->server;
    struct wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
    if(view->xdgSurface->surface != focused_surface) return;
    server->grabbed_view = view;
    struct wlr_box geo_box;
    wlr_xdg_surface_get_geometry(view->xdgSurface, &geo_box);
    double border_x = (view->x + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
    double border_y = (view->y + geo_box.y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
    server->grab_x = server->cursor->x - border_x;
    server->grab_y = server->cursor->y - border_y;
    server->grab_geobox = geo_box;
    server->grab_geobox.x += view->x;
    server->grab_geobox.y += view->y;
    server->resize_edges = edges;
}

//called when a new window is started
static void serverNewWindow(struct wl_listener *listener, void *data) {
    struct TFWCServer *server = wl_container_of(listener, server, newXdgWindow);
    struct wlr_xdg_surface *xdgSurface = data;
    if(xdgSurface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    //alocate window to memory
    struct TFWiew *view = calloc(1, sizeof(struct TFWCView));
    view->server = server;
    view->xdgSurface = xdgSurface;

    //Listen to the various events it can emit
    view->map.notify = xdgUnHideWindow;
    wl_signal_add(&xdgSurface->events.map, &view->map);
    view->unmap.notify = xdgHideWindow;
    wl_signal_add(&xdgSurface->events.unmap, &view->unmap);
    view->destroy.notify = xdgDestroyWindow;
    wl_signal_add(&xdgSurface->events.destroy, &view->destroy);
    struct wlr_xdg_toplevel *toplevel = xdgSurface->toplevel;
    view->requestMove.notify = xdgHandleToplevelMoveRequest;
    wl_signal_add(&toplevel->events.requestMove, &view->request_move);
    view->requestResize.notify = xdgHandleToplevelResizeRequest;
    wl_signal_add(&toplevel->events.requestResize, &view->requestResize);

    //Add it to the list of views.
    wl_list_insert(&server->views, &view->link);
}

//handling of new in and out devices

//event for when a new monitor is connected
static void serverNewOutput(struct wl_listener *listener, void *data) {
    struct TFWCServer *server = wl_container_of(listener, server, newOutput);
    struct wlr_output *wlrOutput = data;

    //setting monitor modes
    if(!wl_list_empty(&wlrOutput->modes)) {
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlrOutput);
	wlr_output_set_mode(wlrOutput, mode);
	wlr_output_enable(wlrOutput, true);
	if(!wlr_output_commit(wlrOutput)) return;
    }

    struct TFWCOutput *output = calloc(1, sizeof(struct TFWCOutput));
    output->wlrOutput = wlrOutput;
    output->server = server;

    //set up event for frame notify
    output->frame.notify = renderScreen;
    wl_signal_add(&wlrOutput->events.frame, &output->frame);
    wl_list_insert(&server->outputs, &output->link);
    
    //auto layouts screens for now
    wlr_output_layout_add_auto(server->outputLayout, wlrOutput);
}

//event for when a new input device is connected
static void serverNewInput(struct wl_listener *listener, void *data) {
    struct TFWCServer *server = wl_container_of(listener, server, newInput);
    struct wlr_input_device *device = data;
    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            struct TFWCKeyboard *keyboard = calloc(1, sizeof(struct TFWCKeyboard));
            keyboard->server = server;
            keyboard->device = device;

            //set correct keymap and rules for the new keyboard
            struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            struct xkb_rule_names rules = { 0 };
            rules.rules = getenv("XKB_DEFAULT_RULES");
            rules.model = getenv("XKB_DEFAULT_MODEL");
            rules.layout = getenv("XKB_DEFAULT_LAYOUT");
            rules.variant = getenv("XKB_DEFAULT_VARIANT");
            rules.options = getenv("XKB_DEFAULT_OPTIONS");
            struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
            wlr_keyboard_set_keymap(device->keyboard, keymap);
            xkb_keymap_unref(keymap);
            xkb_context_unref(context);
            wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

            //define listeners for the new keyboard
            keyboard->modifiers.notify = keyboard_handle_modifiers;
            wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
            keyboard->key.notify = keyboard_handle_key;
            wl_signal_add(&device->keyboard->events.key, &keyboard->key);
            wlr_seat_set_keyboard(server->seat, device);

            //add it to the list of keyboards
            wl_list_insert(&server->keyboards, &keyboard->link);
            break;

        case WLR_INPUT_DEVICE_POINTER:
            wlr_cursor_attach_input_device(server->cursor, device);
            break;

        default:
            break;
    }
    //setting input capabilities
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if(!wl_list_empty(&server->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

//main function duh
int main(int argc, char *argv[]) {
    //start main wayland things
    struct TFWCServer server;
    server.display = wl_display_create();
    server.backend = wlr_backend_autocreate(server.display);
    server.renderer = wlr_backend_get_renderer(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);
    wlr_compositor_create(server.display, server.renderer);
    wlr_data_device_manager_create(server.display);

    //screen layout and new screen connection listener
    server.outputLayout = wlr_output_layout_create();
    wl_list_init(&server.outputs);
    server.newOutput.notify = serverNewOutput;
    wl_signal_add(&server.backend->events.newOutput, &server.newOutput);
    
    //new window listener
    server.scene = wlr_scene_create();
    wlr_scene_attach_output_layout(server.scene, server.outputLayout);
    wl_list_init(&server.views);
    server.xdgShell = wlr_xdg_shell_create(server.display, 3);
    server.newXdgWindow.notify = serverNewWindow;
    wl_signal_add(&server.xdgShell->events.new_surface, &server.newXdgWindow);

    //setup cursor and mouse events
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.outputLayout);
    server.cursorManager = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server.cursorManager, 1);
    server.cursorMotion.notify = serverCursorMotion;
    wl_signal_add(&server.cursor->events.motion, &server.cursorMotion);
    server.cursorMotionAbsolute.notify = serverCursorMotionAbsolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursorMotionAbsolute);
    server.cursorButton.notify = serverCursorButton;
    wl_signal_add(&server.cursor->events.button, &server.cursorButton);
    server.cursorAxis.notify = serverCursorAxis;
    wl_signal_add(&server.cursor->events.axis, &server.cursorAxis);
    server.cursorFrame.notify = serverCursorFrame;
    wl_signal_add(&server.cursor->events.frame, &server.cursorFrame);

    //setup keyboard event stuff
    wl_list_init(&server.keyboards);
    server.newInput.notify = serverNewInput;
    wl_signal_add(&server.backend->events.newInput, &server.new_input);
    server.seat = wlr_seat_create(server.display, "seat0");
    server.requestCursor.notify = seatRequestCursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.requestCursor);
    server.requestSetSelection.notify = seatRequestSetSelection;
    wl_signal_add(&server.seat->events.requestSetSelection, &server.requestSetSelection);

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    return 0;
}
