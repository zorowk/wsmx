/*
MIT License

Copyright (c) 2024 YaoBing Xiao

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "wsm_xwayland.h"

#if HAVE_XWAYLAND
#include "wsm_server.h"
#include "wsm_log.h"
#include "wsm_view.h"
#include "wsm_output.h"
#include "wsm_scene.h"
#include "wsm_seat.h"
#include "wsm_config.h"
#include "wsm_arrange.h"
#include "wsm_workspace.h"
#include "wsm_transaction.h"
#include "wsm_output_manager.h"
#include "wsm_input_manager.h"
#include "node/wsm_node_descriptor.h"
#include "wsm_seatop_move_floating.h"
#include "wsm_seatop_resize_floating.h"

#include <stdlib.h>
#include <assert.h>
#include <float.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>

#include <wayland-util.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

static const char *atom_map[ATOM_LAST] = {
    [NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
    [NET_WM_WINDOW_TYPE_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
    [NET_WM_WINDOW_TYPE_UTILITY] = "_NET_WM_WINDOW_TYPE_UTILITY",
    [NET_WM_WINDOW_TYPE_TOOLBAR] = "_NET_WM_WINDOW_TYPE_TOOLBAR",
    [NET_WM_WINDOW_TYPE_SPLASH] = "_NET_WM_WINDOW_TYPE_SPLASH",
    [NET_WM_WINDOW_TYPE_MENU] = "_NET_WM_WINDOW_TYPE_MENU",
    [NET_WM_WINDOW_TYPE_DROPDOWN_MENU] = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
    [NET_WM_WINDOW_TYPE_POPUP_MENU] = "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    [NET_WM_WINDOW_TYPE_TOOLTIP] = "_NET_WM_WINDOW_TYPE_TOOLTIP",
    [NET_WM_WINDOW_TYPE_NOTIFICATION] = "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    [NET_WM_STATE_MODAL] = "_NET_WM_STATE_MODAL",
};

static void unmanaged_handle_request_configure(struct wl_listener *listener,
                                               void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, request_configure);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;
    struct wlr_xwayland_surface_configure_event *ev = data;
    wlr_xwayland_surface_configure(xsurface, ev->x, ev->y,
                                   ev->width, ev->height);
}

static void unmanaged_handle_set_geometry(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, set_geometry);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;

    wlr_scene_node_set_position(&surface->surface_scene->buffer->node, xsurface->x, xsurface->y);
}

static void unmanaged_handle_map(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, map);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;

    surface->surface_scene = wlr_scene_surface_create(global_server.wsm_scene->layers.unmanaged,
                                                      xsurface->surface);

    if (surface->surface_scene) {
        wsm_scene_descriptor_assign(&surface->surface_scene->buffer->node,
                                WSM_SCENE_DESC_XWAYLAND_UNMANAGED, surface);
        wlr_scene_node_set_position(&surface->surface_scene->buffer->node,
                                    xsurface->x, xsurface->y);

        wl_signal_add(&xsurface->events.set_geometry, &surface->set_geometry);
        surface->set_geometry.notify = unmanaged_handle_set_geometry;
    }

    if (wlr_xwayland_or_surface_wants_focus(xsurface)) {
        struct wsm_seat *seat = input_manager_current_seat();
        struct wlr_xwayland *xwayland = global_server.xwayland.wlr_xwayland;
        wlr_xwayland_set_seat(xwayland, seat->wlr_seat);
        seat_set_focus_surface(seat, xsurface->surface, false);
    }
}

static void unmanaged_handle_unmap(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, unmap);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;

    if (surface->surface_scene) {
        wl_list_remove(&surface->set_geometry.link);

        wlr_scene_node_destroy(&surface->surface_scene->buffer->node);
        surface->surface_scene = NULL;
    }

    struct wsm_seat *seat = input_manager_current_seat();
    if (seat->wlr_seat->keyboard_state.focused_surface == xsurface->surface) {
        // This simply returns focus to the parent surface if there's one available.
        // This seems to handle JetBrains issues.
        if (xsurface->parent && xsurface->parent->surface
            && wlr_xwayland_or_surface_wants_focus(xsurface->parent)) {
            seat_set_focus_surface(seat, xsurface->parent->surface, false);
            return;
        }

        // Restore focus
        struct wsm_node *previous = seat_get_focus_inactive(seat, &global_server.wsm_scene->node);
        if (previous) {
            // Hack to get seat to re-focus the return value of get_focus
            seat_set_focus(seat, NULL);
            seat_set_focus(seat, previous);
        }
    }
}

static void unmanaged_handle_request_activate(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, request_activate);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    struct wsm_seat *seat = input_manager_current_seat();
    struct wsm_container *focus = seat_get_focused_container(seat);
    if (focus && focus->view && focus->view->pid != xsurface->pid) {
        return;
    }

    seat_set_focus_surface(seat, xsurface->surface, false);
}

static void unmanaged_handle_associate(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, associate);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;
    wl_signal_add(&xsurface->surface->events.map, &surface->map);
    surface->map.notify = unmanaged_handle_map;
    wl_signal_add(&xsurface->surface->events.unmap, &surface->unmap);
    surface->unmap.notify = unmanaged_handle_unmap;
}

static void unmanaged_handle_dissociate(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, dissociate);
    wl_list_remove(&surface->map.link);
    wl_list_remove(&surface->unmap.link);
}

static void unmanaged_handle_destroy(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, destroy);
    wl_list_remove(&surface->request_configure.link);
    wl_list_remove(&surface->associate.link);
    wl_list_remove(&surface->dissociate.link);
    wl_list_remove(&surface->destroy.link);
    wl_list_remove(&surface->override_redirect.link);
    wl_list_remove(&surface->request_activate.link);
    free(surface);
}

static void handle_map(struct wl_listener *listener, void *data);
static void handle_associate(struct wl_listener *listener, void *data);

struct wsm_xwayland_view *create_xwayland_view(struct wlr_xwayland_surface *xsurface);

static void unmanaged_handle_override_redirect(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_unmanaged *surface =
        wl_container_of(listener, surface, override_redirect);
    struct wlr_xwayland_surface *xsurface = surface->wlr_xwayland_surface;

    bool associated = xsurface->surface != NULL;
    bool mapped = associated && xsurface->surface->mapped;
    if (mapped) {
        unmanaged_handle_unmap(&surface->unmap, NULL);
    }
    if (associated) {
        unmanaged_handle_dissociate(&surface->dissociate, NULL);
    }

    unmanaged_handle_destroy(&surface->destroy, NULL);
    xsurface->data = NULL;

    struct wsm_xwayland_view *xwayland_view = create_xwayland_view(xsurface);
    if (associated) {
        handle_associate(&xwayland_view->associate, NULL);
    }
    if (mapped) {
        handle_map(&xwayland_view->map, xsurface);
    }
}

static struct wsm_xwayland_unmanaged *create_unmanaged(
    struct wlr_xwayland_surface *xsurface) {
    struct wsm_xwayland_unmanaged *surface =
        calloc(1, sizeof(struct wsm_xwayland_unmanaged));
    if (surface == NULL) {
        wsm_log(WSM_ERROR, "Allocation failed");
        return NULL;
    }

    surface->wlr_xwayland_surface = xsurface;

    wl_signal_add(&xsurface->events.request_configure,
                  &surface->request_configure);
    surface->request_configure.notify = unmanaged_handle_request_configure;
    wl_signal_add(&xsurface->events.associate, &surface->associate);
    surface->associate.notify = unmanaged_handle_associate;
    wl_signal_add(&xsurface->events.dissociate, &surface->dissociate);
    surface->dissociate.notify = unmanaged_handle_dissociate;
    wl_signal_add(&xsurface->events.destroy, &surface->destroy);
    surface->destroy.notify = unmanaged_handle_destroy;
    wl_signal_add(&xsurface->events.set_override_redirect, &surface->override_redirect);
    surface->override_redirect.notify = unmanaged_handle_override_redirect;
    wl_signal_add(&xsurface->events.request_activate, &surface->request_activate);
    surface->request_activate.notify = unmanaged_handle_request_activate;

    return surface;
}

static struct wsm_xwayland_view *xwayland_view_from_view(
    struct wsm_view *view) {
    if (!wsm_assert(view->type == WSM_VIEW_XWAYLAND,
                     "Expected xwayland view")) {
        return NULL;
    }
    return (struct wsm_xwayland_view *)view;
}

static const char *get_string_prop(struct wsm_view *view, enum wsm_view_prop prop) {
    if (xwayland_view_from_view(view) == NULL) {
        return NULL;
    }
    switch (prop) {
    case VIEW_PROP_TITLE:
        return view->wlr_xwayland_surface->title;
    case VIEW_PROP_CLASS:
        return view->wlr_xwayland_surface->class;
    case VIEW_PROP_INSTANCE:
        return view->wlr_xwayland_surface->instance;
    case VIEW_PROP_WINDOW_ROLE:
        return view->wlr_xwayland_surface->role;
    default:
        return NULL;
    }
}

static uint32_t get_int_prop(struct wsm_view *view, enum wsm_view_prop prop) {
    if (xwayland_view_from_view(view) == NULL) {
        return 0;
    }
    switch (prop) {
    case VIEW_PROP_X11_WINDOW_ID:
        return view->wlr_xwayland_surface->window_id;
    case VIEW_PROP_X11_PARENT_ID:
        if (view->wlr_xwayland_surface->parent) {
            return view->wlr_xwayland_surface->parent->window_id;
        }
        return 0;
    case VIEW_PROP_WINDOW_TYPE:
        if (view->wlr_xwayland_surface->window_type_len == 0) {
            return 0;
        }
        return view->wlr_xwayland_surface->window_type[0];
    default:
        return 0;
    }
}

static uint32_t configure(struct wsm_view *view, double lx, double ly, int width,
                          int height) {
    struct wsm_xwayland_view *xwayland_view = xwayland_view_from_view(view);
    if (xwayland_view == NULL) {
        return 0;
    }
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;

    wlr_xwayland_surface_configure(xsurface, lx, ly, width, height);

    // xwayland doesn't give us a serial for the configure
    return 0;
}

static void set_activated(struct wsm_view *view, bool activated) {
    if (xwayland_view_from_view(view) == NULL) {
        return;
    }
    struct wlr_xwayland_surface *surface = view->wlr_xwayland_surface;

    if (activated && surface->minimized) {
        wlr_xwayland_surface_set_minimized(surface, false);
    }

    wlr_xwayland_surface_activate(surface, activated);
    wlr_xwayland_surface_restack(surface, NULL, XCB_STACK_MODE_ABOVE);
}

static void set_tiled(struct wsm_view *view, bool tiled) {
    if (xwayland_view_from_view(view) == NULL) {
        return;
    }
    struct wlr_xwayland_surface *surface = view->wlr_xwayland_surface;
    wlr_xwayland_surface_set_maximized(surface, tiled);
}

static void set_fullscreen(struct wsm_view *view, bool fullscreen) {
    if (xwayland_view_from_view(view) == NULL) {
        return;
    }
    struct wlr_xwayland_surface *surface = view->wlr_xwayland_surface;
    wlr_xwayland_surface_set_fullscreen(surface, fullscreen);
}

static bool wants_floating(struct wsm_view *view) {
    if (xwayland_view_from_view(view) == NULL) {
        return false;
    }
    struct wlr_xwayland_surface *surface = view->wlr_xwayland_surface;
    struct wsm_xwayland *xwayland = &global_server.xwayland;

    if (surface->modal) {
        return true;
    }

    for (size_t i = 0; i < surface->window_type_len; ++i) {
        xcb_atom_t type = surface->window_type[i];
        if (type == xwayland->atoms[NET_WM_WINDOW_TYPE_DIALOG] ||
            type == xwayland->atoms[NET_WM_WINDOW_TYPE_UTILITY] ||
            type == xwayland->atoms[NET_WM_WINDOW_TYPE_TOOLBAR] ||
            type == xwayland->atoms[NET_WM_WINDOW_TYPE_SPLASH]) {
            return true;
        }
    }

    xcb_size_hints_t *size_hints = surface->size_hints;
    if (size_hints != NULL &&
        size_hints->min_width > 0 && size_hints->min_height > 0 &&
        (size_hints->max_width == size_hints->min_width ||
         size_hints->max_height == size_hints->min_height)) {
        return true;
    }

    return false;
}

static void handle_set_decorations(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_decorations);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;

    bool csd = xsurface->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    view_update_csd_from_client(view, csd);
}

static bool is_transient_for(struct wsm_view *child,
                             struct wsm_view *ancestor) {
    if (xwayland_view_from_view(child) == NULL) {
        return false;
    }
    struct wlr_xwayland_surface *surface = child->wlr_xwayland_surface;
    while (surface) {
        if (surface->parent == ancestor->wlr_xwayland_surface) {
            return true;
        }
        surface = surface->parent;
    }
    return false;
}

static void _close(struct wsm_view *view) {
    if (xwayland_view_from_view(view) == NULL) {
        return;
    }
    wlr_xwayland_surface_close(view->wlr_xwayland_surface);
}

static void destroy(struct wsm_view *view) {
    struct wsm_xwayland_view *xwayland_view = xwayland_view_from_view(view);
    if (xwayland_view == NULL) {
        return;
    }
    free(xwayland_view);
}

static void get_constraints(struct wsm_view *view, double *min_width,
                            double *max_width, double *min_height, double *max_height) {
    struct wlr_xwayland_surface *surface = view->wlr_xwayland_surface;
    xcb_size_hints_t *size_hints = surface->size_hints;

    if (size_hints == NULL) {
        *min_width = DBL_MIN;
        *max_width = DBL_MAX;
        *min_height = DBL_MIN;
        *max_height = DBL_MAX;
        return;
    }

    *min_width = size_hints->min_width > 0 ? size_hints->min_width : DBL_MIN;
    *max_width = size_hints->max_width > 0 ? size_hints->max_width : DBL_MAX;
    *min_height = size_hints->min_height > 0 ? size_hints->min_height : DBL_MIN;
    *max_height = size_hints->max_height > 0 ? size_hints->max_height : DBL_MAX;
}

static const struct wsm_view_impl view_impl = {
    .get_constraints = get_constraints,
    .get_string_prop = get_string_prop,
    .get_int_prop = get_int_prop,
    .configure = configure,
    .set_activated = set_activated,
    .set_tiled = set_tiled,
    .set_fullscreen = set_fullscreen,
    .wants_floating = wants_floating,
    .is_transient_for = is_transient_for,
    .close = _close,
    .destroy = destroy,
};

static void handle_commit(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, commit);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    struct wlr_surface_state *state = &xsurface->surface->current;

    struct wlr_box new_geo = {0};
    new_geo.width = state->width;
    new_geo.height = state->height;

    bool new_size = new_geo.width != view->geometry.width ||
                    new_geo.height != view->geometry.height;

    if (new_size) {
        // The client changed its surface size in this commit. For floating
        // containers, we resize the container to match. For tiling containers,
        // we only recenter the surface.
        memcpy(&view->geometry, &new_geo, sizeof(struct wlr_box));
        if (container_is_floating(view->container)) {
            view_update_size(view);
            transaction_commit_dirty_client();
        }

        view_center_and_clip_surface(view);
    }

    if (view->container->node.instruction) {
        bool successful = transaction_notify_view_ready_by_geometry(view,
                                                                    xsurface->x, xsurface->y, state->width, state->height);

        // If we saved the view and this commit isn't what we're looking for
        // that means the user will never actually see the buffers submitted to
        // us here. Just send frame done events to these surfaces so they can
        // commit another time for us.
        if (view->saved_surface_tree && !successful) {
            view_send_frame_done(view);
        }
    }
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, destroy);
    struct wsm_view *view = &xwayland_view->view;

    if (view->surface) {
        view_unmap(view);
        wl_list_remove(&xwayland_view->commit.link);
    }

    xwayland_view->view.wlr_xwayland_surface = NULL;

    wl_list_remove(&xwayland_view->destroy.link);
    wl_list_remove(&xwayland_view->request_configure.link);
    wl_list_remove(&xwayland_view->request_fullscreen.link);
    wl_list_remove(&xwayland_view->request_minimize.link);
    wl_list_remove(&xwayland_view->request_move.link);
    wl_list_remove(&xwayland_view->request_resize.link);
    wl_list_remove(&xwayland_view->request_activate.link);
    wl_list_remove(&xwayland_view->set_title.link);
    wl_list_remove(&xwayland_view->set_class.link);
    wl_list_remove(&xwayland_view->set_role.link);
    wl_list_remove(&xwayland_view->set_startup_id.link);
    wl_list_remove(&xwayland_view->set_window_type.link);
    wl_list_remove(&xwayland_view->set_hints.link);
    wl_list_remove(&xwayland_view->set_decorations.link);
    wl_list_remove(&xwayland_view->associate.link);
    wl_list_remove(&xwayland_view->dissociate.link);
    wl_list_remove(&xwayland_view->override_redirect.link);
    view_begin_destroy(&xwayland_view->view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, unmap);
    struct wsm_view *view = &xwayland_view->view;

    if (!wsm_assert(view->surface, "Cannot unmap unmapped view")) {
        return;
    }

    wl_list_remove(&xwayland_view->commit.link);
    wl_list_remove(&xwayland_view->surface_tree_destroy.link);

    if (xwayland_view->surface_tree) {
        wlr_scene_node_destroy(&xwayland_view->surface_tree->node);
        xwayland_view->surface_tree = NULL;
    }

    view_unmap(view);
}

static void handle_surface_tree_destroy(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view,
                                                               surface_tree_destroy);
    xwayland_view->surface_tree = NULL;
}

static void handle_map(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, map);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;

    view->natural_width = xsurface->width;
    view->natural_height = xsurface->height;

    // Wire up the commit listener here, because xwayland map/unmap can change
    // the underlying wlr_surface
    wl_signal_add(&xsurface->surface->events.commit, &xwayland_view->commit);
    xwayland_view->commit.notify = handle_commit;

    // Put it back into the tree
    view_map(view, xsurface->surface, xsurface->fullscreen, NULL, false);

    xwayland_view->surface_tree = wlr_scene_subsurface_tree_create(
        xwayland_view->view.content_tree, xsurface->surface);

    if (xwayland_view->surface_tree) {
        xwayland_view->surface_tree_destroy.notify = handle_surface_tree_destroy;
        wl_signal_add(&xwayland_view->surface_tree->node.events.destroy,
                      &xwayland_view->surface_tree_destroy);
    }

    transaction_commit_dirty();
}

static void handle_dissociate(struct wl_listener *listener, void *data);

static void handle_override_redirect(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, override_redirect);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;

    bool associated = xsurface->surface != NULL;
    bool mapped = associated && xsurface->surface->mapped;
    if (mapped) {
        handle_unmap(&xwayland_view->unmap, NULL);
    }
    if (associated) {
        handle_dissociate(&xwayland_view->dissociate, NULL);
    }

    handle_destroy(&xwayland_view->destroy, view);
    xsurface->data = NULL;

    struct wsm_xwayland_unmanaged *unmanaged = create_unmanaged(xsurface);
    if (associated) {
        unmanaged_handle_associate(&unmanaged->associate, NULL);
    }
    if (mapped) {
        unmanaged_handle_map(&unmanaged->map, xsurface);
    }
}

static void handle_request_configure(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        wlr_xwayland_surface_configure(xsurface, ev->x, ev->y,
                                       ev->width, ev->height);
        return;
    }
    if (container_is_floating(view->container)) {
        // Respect minimum and maximum sizes
        view->natural_width = ev->width;
        view->natural_height = ev->height;
        container_floating_resize_and_center(view->container);

        configure(view, view->container->pending.content_x,
                  view->container->pending.content_y,
                  view->container->pending.content_width,
                  view->container->pending.content_height);
        node_set_dirty(&view->container->node);
    } else {
        configure(view, view->container->current.content_x,
                  view->container->current.content_y,
                  view->container->current.content_width,
                  view->container->current.content_height);
    }
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_fullscreen);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    container_set_fullscreen(view->container, xsurface->fullscreen);

    arrange_root_auto();
    transaction_commit_dirty();
}

static void handle_request_minimize(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_minimize);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }

    struct wlr_xwayland_minimize_event *e = data;
    struct wsm_seat *seat = input_manager_current_seat();
    bool focused = seat_get_focus(seat) == &view->container->node;
    wlr_xwayland_surface_set_minimized(xsurface, !focused && e->minimize);
}

static void handle_request_move(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_move);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    if (!container_is_floating(view->container) ||
        view->container->pending.fullscreen_mode) {
        return;
    }
    struct wsm_seat *seat = input_manager_current_seat();
    seatop_begin_move_floating(seat, view->container);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_resize);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    if (!container_is_floating(view->container)) {
        return;
    }
    struct wlr_xwayland_resize_event *e = data;
    struct wsm_seat *seat = input_manager_current_seat();
    seatop_begin_resize_floating(seat, view->container, e->edges);
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, request_activate);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    view_request_activate(view, NULL);

    transaction_commit_dirty();
}

static void handle_set_title(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_title);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    view_update_title(view, false);
}

static void handle_set_class(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_class);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
}

static void handle_set_role(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_role);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
}

static void handle_set_startup_id(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_startup_id);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->startup_id == NULL) {
        return;
    }

    struct wlr_xdg_activation_token_v1 *token =
        wlr_xdg_activation_v1_find_token(
            global_server.xdg_activation_v1, xsurface->startup_id);
    if (token == NULL) {
        // Tried to activate with an unknown or expired token
        return;
    }
}

static void handle_set_window_type(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_window_type);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
}

static void handle_set_hints(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, set_hints);
    struct wsm_view *view = &xwayland_view->view;
    struct wlr_xwayland_surface *xsurface = view->wlr_xwayland_surface;
    if (xsurface->surface == NULL || !xsurface->surface->mapped) {
        return;
    }
    const bool hints_urgency = xcb_icccm_wm_hints_get_urgency(xsurface->hints);
    if (!hints_urgency && view->urgent_timer) {
        // The view is in the timeout period. We'll ignore the request to
        // unset urgency so that the view remains urgent until the timer clears
        // it.
        return;
    }
    if (view->allow_request_urgent) {
        view_set_urgent(view, hints_urgency);
    }
}

static void handle_associate(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, associate);
    struct wlr_xwayland_surface *xsurface =
        xwayland_view->view.wlr_xwayland_surface;
    wl_signal_add(&xsurface->surface->events.unmap, &xwayland_view->unmap);
    xwayland_view->unmap.notify = handle_unmap;
    wl_signal_add(&xsurface->surface->events.map, &xwayland_view->map);
    xwayland_view->map.notify = handle_map;
}

static void handle_dissociate(struct wl_listener *listener, void *data) {
    struct wsm_xwayland_view *xwayland_view =
        wl_container_of(listener, xwayland_view, dissociate);
    wl_list_remove(&xwayland_view->map.link);
    wl_list_remove(&xwayland_view->unmap.link);
}

struct wsm_xwayland_view *create_xwayland_view(struct wlr_xwayland_surface *xsurface) {
    wsm_log(WSM_DEBUG, "New xwayland surface title='%s' class='%s'",
             xsurface->title, xsurface->class);

    struct wsm_xwayland_view *xwayland_view =
        calloc(1, sizeof(struct wsm_xwayland_view));
    if (!wsm_assert(xwayland_view, "Failed to allocate view")) {
        return NULL;
    }

    if (!view_init(&xwayland_view->view, WSM_VIEW_XWAYLAND, &view_impl)) {
        free(xwayland_view);
        return NULL;
    }
    xwayland_view->view.wlr_xwayland_surface = xsurface;

    wl_signal_add(&xsurface->events.destroy, &xwayland_view->destroy);
    xwayland_view->destroy.notify = handle_destroy;

    wl_signal_add(&xsurface->events.request_configure,
                  &xwayland_view->request_configure);
    xwayland_view->request_configure.notify = handle_request_configure;

    wl_signal_add(&xsurface->events.request_fullscreen,
                  &xwayland_view->request_fullscreen);
    xwayland_view->request_fullscreen.notify = handle_request_fullscreen;

    wl_signal_add(&xsurface->events.request_minimize,
                  &xwayland_view->request_minimize);
    xwayland_view->request_minimize.notify = handle_request_minimize;

    wl_signal_add(&xsurface->events.request_activate,
                  &xwayland_view->request_activate);
    xwayland_view->request_activate.notify = handle_request_activate;

    wl_signal_add(&xsurface->events.request_move,
                  &xwayland_view->request_move);
    xwayland_view->request_move.notify = handle_request_move;

    wl_signal_add(&xsurface->events.request_resize,
                  &xwayland_view->request_resize);
    xwayland_view->request_resize.notify = handle_request_resize;

    wl_signal_add(&xsurface->events.set_title, &xwayland_view->set_title);
    xwayland_view->set_title.notify = handle_set_title;

    wl_signal_add(&xsurface->events.set_class, &xwayland_view->set_class);
    xwayland_view->set_class.notify = handle_set_class;

    wl_signal_add(&xsurface->events.set_role, &xwayland_view->set_role);
    xwayland_view->set_role.notify = handle_set_role;

    wl_signal_add(&xsurface->events.set_startup_id,
                  &xwayland_view->set_startup_id);
    xwayland_view->set_startup_id.notify = handle_set_startup_id;

    wl_signal_add(&xsurface->events.set_window_type,
                  &xwayland_view->set_window_type);
    xwayland_view->set_window_type.notify = handle_set_window_type;

    wl_signal_add(&xsurface->events.set_hints, &xwayland_view->set_hints);
    xwayland_view->set_hints.notify = handle_set_hints;

    wl_signal_add(&xsurface->events.set_decorations,
                  &xwayland_view->set_decorations);
    xwayland_view->set_decorations.notify = handle_set_decorations;

    wl_signal_add(&xsurface->events.associate, &xwayland_view->associate);
    xwayland_view->associate.notify = handle_associate;

    wl_signal_add(&xsurface->events.dissociate, &xwayland_view->dissociate);
    xwayland_view->dissociate.notify = handle_dissociate;

    wl_signal_add(&xsurface->events.set_override_redirect,
                  &xwayland_view->override_redirect);
    xwayland_view->override_redirect.notify = handle_override_redirect;

    xsurface->data = xwayland_view;

    return xwayland_view;
}

void handle_xwayland_surface(struct wl_listener *listener, void *data) {
    struct wlr_xwayland_surface *xsurface = data;

    if (xsurface->override_redirect) {
        wsm_log(WSM_DEBUG, "New xwayland unmanaged surface");
        create_unmanaged(xsurface);
        return;
    }

    create_xwayland_view(xsurface);
}

void handle_xwayland_ready(struct wl_listener *listener, void *data) {
    struct wsm_server *server =
        wl_container_of(listener, server, xwayland_ready);
    struct wsm_xwayland *xwayland = &server->xwayland;

    xcb_connection_t *xcb_conn = xcb_connect(NULL, NULL);
    int err = xcb_connection_has_error(xcb_conn);
    if (err) {
        wsm_log(WSM_ERROR, "XCB connect failed: %d", err);
        return;
    }

    xcb_intern_atom_cookie_t cookies[ATOM_LAST];
    for (size_t i = 0; i < ATOM_LAST; i++) {
        cookies[i] =
            xcb_intern_atom(xcb_conn, 0, strlen(atom_map[i]), atom_map[i]);
    }
    for (size_t i = 0; i < ATOM_LAST; i++) {
        xcb_generic_error_t *error = NULL;
        xcb_intern_atom_reply_t *reply =
            xcb_intern_atom_reply(xcb_conn, cookies[i], &error);
        if (reply != NULL && error == NULL) {
            xwayland->atoms[i] = reply->atom;
        }
        free(reply);

        if (error != NULL) {
            wsm_log(WSM_ERROR, "could not resolve atom %s, X11 error code %d",
                     atom_map[i], error->error_code);
            free(error);
            break;
        }
    }

    xcb_disconnect(xcb_conn);
}

/**
 * @brief xwayland_start start a xwayland server
 * @param server
 * @return true represents successed; false maybe failed to start Xwayland or wsm disabled Xwayland
 */
bool xwayland_start(struct wsm_server *server) {
    if (global_server.xwayland_enabled) {
        wsm_log(WSM_DEBUG, "Initializing Xwayland (lazy=%d)",
                global_config.xwayland == XWAYLAND_MODE_LAZY);
        server->xwayland.wlr_xwayland = wlr_xwayland_create(server->wl_display, server->wlr_compositor,
                                                            global_config.xwayland  == XWAYLAND_MODE_LAZY);
        if (!server->xwayland.wlr_xwayland) {
            wsm_log(WSM_ERROR, "Failed to start Xwayland");
            unsetenv("DISPLAY");
            return false;
        } else {
            server->xwayland_surface.notify = handle_xwayland_surface;
            wl_signal_add(&server->xwayland.wlr_xwayland->events.new_surface,
                          &server->xwayland_surface);
            server->xwayland_ready.notify = handle_xwayland_ready;
            wl_signal_add(&server->xwayland.wlr_xwayland->events.ready,
                          &server->xwayland_ready);
            setenv("DISPLAY", server->xwayland.wlr_xwayland->display_name, true);
        }
    }

    return true;
}

struct wsm_view *view_from_wlr_xwayland_surface(
    struct wlr_xwayland_surface *xsurface) {
    return xsurface->data;
}

#endif
