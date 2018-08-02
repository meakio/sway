#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include "config.h"
#include "sway/debug.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/layout.h"
#include "sway/output.h"
#include "sway/tree/workspace.h"
#include "sway/tree/view.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "list.h"
#include "log.h"

struct sway_container root_container;

static void output_layout_handle_change(struct wl_listener *listener,
		void *data) {
	arrange_windows(&root_container);
	transaction_commit_dirty();
}

void layout_init(void) {
	root_container.id = 0; // normally assigned in new_swayc()
	root_container.type = C_ROOT;
	root_container.layout = L_NONE;
	root_container.name = strdup("root");
	root_container.instructions = create_list();
	root_container.children = create_list();
	root_container.current.children = create_list();
	wl_signal_init(&root_container.events.destroy);

	root_container.sway_root = calloc(1, sizeof(*root_container.sway_root));
	root_container.sway_root->output_layout = wlr_output_layout_create();
	wl_list_init(&root_container.sway_root->outputs);
#ifdef HAVE_XWAYLAND
	wl_list_init(&root_container.sway_root->xwayland_unmanaged);
#endif
	wl_list_init(&root_container.sway_root->drag_icons);
	wl_signal_init(&root_container.sway_root->events.new_container);
	root_container.sway_root->scratchpad = create_list();

	root_container.sway_root->output_layout_change.notify =
		output_layout_handle_change;
	wl_signal_add(&root_container.sway_root->output_layout->events.change,
		&root_container.sway_root->output_layout_change);
}

static int index_child(const struct sway_container *child) {
	struct sway_container *parent = child->parent;
	for (int i = 0; i < parent->children->length; ++i) {
		if (parent->children->items[i] == child) {
			return i;
		}
	}
	// This happens if the child is a floating container
	return -1;
}

static void container_handle_fullscreen_reparent(struct sway_container *con,
		struct sway_container *old_parent) {
	if (!con->is_fullscreen) {
		return;
	}
	struct sway_container *old_workspace = old_parent;
	if (old_workspace && old_workspace->type != C_WORKSPACE) {
		old_workspace = container_parent(old_workspace, C_WORKSPACE);
	}
	struct sway_container *new_workspace = container_parent(con, C_WORKSPACE);
	if (old_workspace == new_workspace) {
		return;
	}
	// Unmark the old workspace as fullscreen
	if (old_workspace) {
		old_workspace->sway_workspace->fullscreen = NULL;
	}

	// Mark the new workspace as fullscreen
	if (new_workspace->sway_workspace->fullscreen) {
		container_set_fullscreen(
				new_workspace->sway_workspace->fullscreen, false);
	}
	new_workspace->sway_workspace->fullscreen = con;

	// Resize container to new output dimensions
	struct sway_container *output = new_workspace->parent;
	con->x = output->x;
	con->y = output->y;
	con->width = output->width;
	con->height = output->height;

	if (con->type == C_VIEW) {
		struct sway_view *view = con->sway_view;
		view->x = output->x;
		view->y = output->y;
		view->width = output->width;
		view->height = output->height;
	} else {
		arrange_windows(new_workspace);
	}
}

void container_insert_child(struct sway_container *parent,
		struct sway_container *child, int i) {
	struct sway_container *old_parent = child->parent;
	if (old_parent) {
		container_remove_child(child);
	}
	wlr_log(WLR_DEBUG, "Inserting id:%zd at index %d", child->id, i);
	list_insert(parent->children, i, child);
	child->parent = parent;
	container_handle_fullscreen_reparent(child, old_parent);
	wl_signal_emit(&child->events.reparent, old_parent);
}

struct sway_container *container_add_sibling(struct sway_container *fixed,
		struct sway_container *active) {
	// TODO handle floating
	struct sway_container *old_parent = NULL;
	if (active->parent) {
		old_parent = active->parent;
		container_remove_child(active);
	}
	struct sway_container *parent = fixed->parent;
	int i = index_child(fixed);
	list_insert(parent->children, i + 1, active);
	active->parent = parent;
	container_handle_fullscreen_reparent(active, old_parent);
	wl_signal_emit(&active->events.reparent, old_parent);
	return active->parent;
}

void container_add_child(struct sway_container *parent,
		struct sway_container *child) {
	wlr_log(WLR_DEBUG, "Adding %p (%d, %fx%f) to %p (%d, %fx%f)",
			child, child->type, child->width, child->height,
			parent, parent->type, parent->width, parent->height);
	struct sway_container *old_parent = child->parent;
	list_add(parent->children, child);
	child->parent = parent;
	container_handle_fullscreen_reparent(child, old_parent);
	if (old_parent) {
		container_set_dirty(old_parent);
	}
	container_set_dirty(child);
}

struct sway_container *container_remove_child(struct sway_container *child) {
	if (child->is_fullscreen) {
		struct sway_container *workspace = container_parent(child, C_WORKSPACE);
		workspace->sway_workspace->fullscreen = NULL;
	}

	struct sway_container *parent = child->parent;
	for (int i = 0; i < parent->children->length; ++i) {
		if (parent->children->items[i] == child) {
			list_del(parent->children, i);
			break;
		}
	}
	child->parent = NULL;
	container_notify_subtree_changed(parent);

	container_set_dirty(parent);
	container_set_dirty(child);

	return parent;
}

void container_move_to(struct sway_container *container,
		struct sway_container *destination) {
	if (container == destination
			|| container_has_ancestor(container, destination)) {
		return;
	}
	if (container_is_floating(container)) {
		// TODO
		return;
	}
	struct sway_container *old_parent = container_remove_child(container);
	container->width = container->height = 0;
	container->saved_width = container->saved_height = 0;

	struct sway_container *new_parent, *new_parent_focus;
	struct sway_seat *seat = input_manager_get_default_seat(input_manager);

	// Get the focus of the destination before we change it.
	new_parent_focus = seat_get_focus_inactive(seat, destination);
	if (destination->type == C_VIEW) {
		new_parent = container_add_sibling(destination, container);
	} else {
		new_parent = destination;
		container_add_child(destination, container);
	}
	wl_signal_emit(&container->events.reparent, old_parent);

	if (container->type == C_WORKSPACE) {
		// If moving a workspace to a new output, maybe create a new workspace
		// on the previous output
		if (old_parent->children->length == 0) {
			char *ws_name = workspace_next_name(old_parent->name);
			struct sway_container *ws = workspace_create(old_parent, ws_name);
			free(ws_name);
			seat_set_focus(seat, ws);
		}

		// Try to remove an empty workspace from the destination output.
		container_reap_empty_recursive(new_parent_focus);

		container_sort_workspaces(new_parent);
		seat_set_focus(seat, new_parent);
		workspace_output_raise_priority(container, old_parent, new_parent);
		ipc_event_workspace(NULL, container, "move");
	} else if (container->type == C_VIEW) {
		ipc_event_window(container, "move");
	}
	container_notify_subtree_changed(old_parent);
	container_notify_subtree_changed(new_parent);

	// If view was moved to a fullscreen workspace, refocus the fullscreen view
	struct sway_container *new_workspace = container;
	if (new_workspace->type != C_WORKSPACE) {
		new_workspace = container_parent(new_workspace, C_WORKSPACE);
	}
	if (new_workspace->sway_workspace->fullscreen) {
		struct sway_seat *seat;
		struct sway_container *focus, *focus_ws;
		wl_list_for_each(seat, &input_manager->seats, link) {
			focus = seat_get_focus(seat);
			focus_ws = focus;
			if (focus_ws->type != C_WORKSPACE) {
				focus_ws = container_parent(focus_ws, C_WORKSPACE);
			}
			if (focus_ws == new_workspace) {
				struct sway_container *new_focus = seat_get_focus_inactive(seat,
						new_workspace->sway_workspace->fullscreen);
				seat_set_focus(seat, new_focus);
			}
		}
	}
	// Update workspace urgent state
	struct sway_container *old_workspace = old_parent;
	if (old_workspace->type != C_WORKSPACE) {
		old_workspace = container_parent(old_workspace, C_WORKSPACE);
	}
	if (new_workspace != old_workspace) {
		workspace_detect_urgent(new_workspace);
		if (old_workspace) {
			workspace_detect_urgent(old_workspace);
		}
	}
}

static bool sway_dir_to_wlr(enum movement_direction dir,
		enum wlr_direction *out) {
	switch (dir) {
	case MOVE_UP:
		*out = WLR_DIRECTION_UP;
		break;
	case MOVE_DOWN:
		*out = WLR_DIRECTION_DOWN;
		break;
	case MOVE_LEFT:
		*out = WLR_DIRECTION_LEFT;
		break;
	case MOVE_RIGHT:
		*out = WLR_DIRECTION_RIGHT;
		break;
	default:
		return false;
	}

	return true;
}

static bool is_parallel(enum sway_container_layout layout,
		enum movement_direction dir) {
	switch (layout) {
	case L_TABBED:
	case L_HORIZ:
		return dir == MOVE_LEFT || dir == MOVE_RIGHT;
	case L_STACKED:
	case L_VERT:
		return dir == MOVE_UP || dir == MOVE_DOWN;
	default:
		return false;
	}
}

static enum movement_direction invert_movement(enum movement_direction dir) {
	switch (dir) {
	case MOVE_LEFT:
		return MOVE_RIGHT;
	case MOVE_RIGHT:
		return MOVE_LEFT;
	case MOVE_UP:
		return MOVE_DOWN;
	case MOVE_DOWN:
		return MOVE_UP;
	default:
		sway_assert(0, "This function expects left|right|up|down");
		return MOVE_LEFT;
	}
}

static int move_offs(enum movement_direction move_dir) {
	return move_dir == MOVE_LEFT || move_dir == MOVE_UP ? -1 : 1;
}

/* Gets the index of the most extreme member based on the movement offset */
static int container_limit(struct sway_container *container,
		enum movement_direction move_dir) {
	return move_offs(move_dir) < 0 ? 0 : container->children->length;
}

/* Takes one child, sets it aside, wraps the rest of the children in a new
 * container, switches the layout of the workspace, and drops the child back in.
 * In other words, rejigger it. */
static void workspace_rejigger(struct sway_container *ws,
		struct sway_container *child, enum movement_direction move_dir) {
	struct sway_container *original_parent = child->parent;
	struct sway_container *new_parent =
		container_split(ws, ws->layout);

	container_remove_child(child);
	for (int i = 0; i < ws->children->length; ++i) {
		struct sway_container *_child = ws->children->items[i];
		container_move_to(new_parent, _child);
	}

	int index = move_offs(move_dir);
	container_insert_child(ws, child, index < 0 ? 0 : 1);
	ws->layout =
		move_dir == MOVE_LEFT || move_dir == MOVE_RIGHT ? L_HORIZ : L_VERT;

	container_flatten(ws);
	container_reap_empty_recursive(original_parent);
	wl_signal_emit(&child->events.reparent, original_parent);
	container_create_notify(new_parent);
}

static void move_out_of_tabs_stacks(struct sway_container *container,
		struct sway_container *current, enum movement_direction move_dir,
		int offs) {
	if (container->parent == current->parent
			&& current->parent->children->length == 1) {
		wlr_log(WLR_DEBUG, "Changing layout of %zd", current->parent->id);
		current->parent->layout = move_dir ==
			MOVE_LEFT || move_dir == MOVE_RIGHT ? L_HORIZ : L_VERT;
		return;
	}

	wlr_log(WLR_DEBUG, "Moving out of tab/stack into a split");
	bool is_workspace = current->parent->type == C_WORKSPACE;
	struct sway_container *new_parent = container_split(current->parent,
		move_dir == MOVE_LEFT || move_dir == MOVE_RIGHT ? L_HORIZ : L_VERT);
	if (is_workspace) {
		container_insert_child(new_parent->parent, container, offs < 0 ? 0 : 1);
	} else {
		container_insert_child(new_parent, container, offs < 0 ? 0 : 1);
		container_reap_empty_recursive(new_parent->parent);
		container_flatten(new_parent->parent);
	}
	container_create_notify(new_parent);
	container_notify_subtree_changed(new_parent);
}

void container_move(struct sway_container *container,
		enum movement_direction move_dir, int move_amt) {
	if (!sway_assert(
				container->type != C_CONTAINER || container->type != C_VIEW,
				"Can only move containers and views")) {
		return;
	}
	int offs = move_offs(move_dir);

	struct sway_container *sibling = NULL;
	struct sway_container *current = container;
	struct sway_container *parent = current->parent;
	struct sway_container *top = &root_container;

	// If moving a fullscreen view, only consider outputs
	if (container->is_fullscreen) {
		current = container_parent(container, C_OUTPUT);
	} else if (container_is_fullscreen_or_child(container) ||
			container_is_floating_or_child(container)) {
		// If we've fullscreened a split container, only allow the child to move
		// around within the fullscreen parent.
		// Same with floating a split container.
		struct sway_container *ws = container_parent(container, C_WORKSPACE);
		top = ws->sway_workspace->fullscreen;
	}

	struct sway_container *new_parent = container_flatten(parent);
	if (new_parent != parent) {
		// Special case: we were the last one in this container, so leave
		return;
	}

	while (!sibling) {
		if (current == top) {
			return;
		}

		parent = current->parent;
		wlr_log(WLR_DEBUG, "Visiting %p %s '%s'", current,
				container_type_to_str(current->type), current->name);

		int index = index_child(current);

		switch (current->type) {
		case C_OUTPUT: {
			enum wlr_direction wlr_dir = 0;
			if (!sway_assert(sway_dir_to_wlr(move_dir, &wlr_dir),
						"got invalid direction: %d", move_dir)) {
				return;
			}
			double ref_lx = current->x + current->width / 2;
			double ref_ly = current->y + current->height / 2;
			struct wlr_output *next = wlr_output_layout_adjacent_output(
				root_container.sway_root->output_layout, wlr_dir,
				current->sway_output->wlr_output, ref_lx, ref_ly);
			if (!next) {
				wlr_log(WLR_DEBUG, "Hit edge of output, nowhere else to go");
				return;
			}
			struct sway_output *next_output = next->data;
			current = next_output->swayc;
			wlr_log(WLR_DEBUG, "Selected next output (%s)", current->name);
			// Select workspace and get outta here
			current = seat_get_focus_inactive(
					config->handler_context.seat, current);
			if (current->type != C_WORKSPACE) {
				current = container_parent(current, C_WORKSPACE);
			}
			sibling = current;
			break;
		}
		case C_WORKSPACE:
			if (!is_parallel(current->layout, move_dir)) {
				if (current->children->length >= 2) {
					wlr_log(WLR_DEBUG, "Rejiggering the workspace (%d kiddos)",
							current->children->length);
					workspace_rejigger(current, container, move_dir);
					return;
				} else {
					wlr_log(WLR_DEBUG, "Selecting output");
					current = current->parent;
				}
			} else if (current->layout == L_TABBED
					|| current->layout == L_STACKED) {
				wlr_log(WLR_DEBUG, "Rejiggering out of tabs/stacks");
				workspace_rejigger(current, container, move_dir);
			} else {
				wlr_log(WLR_DEBUG, "Selecting output");
				current = current->parent;
			}
			break;
		case C_CONTAINER:
		case C_VIEW:
			if (is_parallel(parent->layout, move_dir)) {
				if ((index == parent->children->length - 1 && offs > 0)
						|| (index == 0 && offs < 0)) {
					if (current->parent == container->parent) {
						if (parent->parent->layout == L_FLOATING) {
							return;
						}
						if (!parent->is_fullscreen &&
								(parent->layout == L_TABBED ||
								 parent->layout == L_STACKED)) {
							move_out_of_tabs_stacks(container, current,
									move_dir, offs);
							return;
						} else {
							wlr_log(WLR_DEBUG, "Hit limit, selecting parent");
							current = current->parent;
						}
					} else {
						wlr_log(WLR_DEBUG, "Hit limit, "
								"promoting descendant to sibling");
						// Special case
						container_insert_child(current->parent, container,
								index + (offs < 0 ? 0 : 1));
						container->width = container->height = 0;
						return;
					}
				} else {
					sibling = parent->children->items[index + offs];
					wlr_log(WLR_DEBUG, "Selecting sibling id:%zd", sibling->id);
				}
			} else if (!parent->is_fullscreen &&
					parent->parent->layout != L_FLOATING &&
					(parent->layout == L_TABBED ||
						parent->layout == L_STACKED)) {
				move_out_of_tabs_stacks(container, current, move_dir, offs);
				return;
			} else if (parent->parent->layout == L_FLOATING) {
				return;
			} else {
				wlr_log(WLR_DEBUG, "Moving up to find a parallel container");
				current = current->parent;
			}
			break;
		default:
			sway_assert(0, "Not expecting to see container of type %s here",
					container_type_to_str(current->type));
			return;
		}
	}

	// Part two: move stuff around
	int index = index_child(container);
	struct sway_container *old_parent = container->parent;

	while (sibling) {
		switch (sibling->type) {
		case C_VIEW:
			if (sibling->parent == container->parent) {
				wlr_log(WLR_DEBUG, "Swapping siblings");
				sibling->parent->children->items[index + offs] = container;
				sibling->parent->children->items[index] = sibling;
			} else {
				wlr_log(WLR_DEBUG, "Promoting to sibling of cousin");
				container_insert_child(sibling->parent, container,
						index_child(sibling) + (offs > 0 ? 0 : 1));
				container->width = container->height = 0;
			}
			sibling = NULL;
			break;
		case C_WORKSPACE: // Note: only in the case of moving between outputs
		case C_CONTAINER:
			if (is_parallel(sibling->layout, move_dir)) {
				int limit = container_limit(sibling, invert_movement(move_dir));
				wlr_log(WLR_DEBUG, "limit: %d", limit);
				wlr_log(WLR_DEBUG,
						"Reparenting container (parallel) to index %d "
						"(move dir: %d)", limit, move_dir);
				container_insert_child(sibling, container, limit);
				container->width = container->height = 0;
				sibling = NULL;
			} else {
				wlr_log(WLR_DEBUG, "Reparenting container (perpendicular)");
				struct sway_container *focus_inactive = seat_get_focus_inactive(
						config->handler_context.seat, sibling);
				if (focus_inactive && focus_inactive != sibling) {
					while (focus_inactive->parent != sibling) {
						focus_inactive = focus_inactive->parent;
					}
					wlr_log(WLR_DEBUG, "Focus inactive: id:%zd",
							focus_inactive->id);
					sibling = focus_inactive;
					continue;
				} else if (sibling->children->length) {
					wlr_log(WLR_DEBUG, "No focus-inactive, adding arbitrarily");
					container_remove_child(container);
					container_add_sibling(sibling->children->items[0], container);
				} else {
					wlr_log(WLR_DEBUG, "No kiddos, adding container alone");
					container_remove_child(container);
					container_add_child(sibling, container);
				}
				container->width = container->height = 0;
				sibling = NULL;
			}
			break;
		default:
			sway_assert(0, "Not expecting to see container of type %s here",
					container_type_to_str(sibling->type));
			return;
		}
	}

	container_notify_subtree_changed(old_parent);
	container_notify_subtree_changed(container->parent);

	if (container->type == C_VIEW) {
		ipc_event_window(container, "move");
	}

	if (old_parent) {
		seat_set_focus(config->handler_context.seat, old_parent);
		seat_set_focus(config->handler_context.seat, container);
	}

	struct sway_container *last_ws = old_parent;
	struct sway_container *next_ws = container->parent;
	if (last_ws && last_ws->type != C_WORKSPACE) {
		last_ws = container_parent(last_ws, C_WORKSPACE);
	}
	if (next_ws && next_ws->type != C_WORKSPACE) {
		next_ws = container_parent(next_ws, C_WORKSPACE);
	}
	if (last_ws && next_ws && last_ws != next_ws) {
		ipc_event_workspace(last_ws, next_ws, "focus");
		workspace_detect_urgent(last_ws);
		workspace_detect_urgent(next_ws);
	}
	container_end_mouse_operation(container);
}

enum sway_container_layout container_get_default_layout(
		struct sway_container *con) {
	if (con->type != C_OUTPUT) {
		con = container_parent(con, C_OUTPUT);
	}

	if (!sway_assert(con != NULL,
			"container_get_default_layout must be called on an attached"
			" container below the root container")) {
		return 0;
	}

	if (config->default_layout != L_NONE) {
		return config->default_layout;
	} else if (config->default_orientation != L_NONE) {
		return config->default_orientation;
	} else if (con->width >= con->height) {
		return L_HORIZ;
	} else {
		return L_VERT;
	}
}

static int sort_workspace_cmp_qsort(const void *_a, const void *_b) {
	struct sway_container *a = *(void **)_a;
	struct sway_container *b = *(void **)_b;
	int retval = 0;

	if (isdigit(a->name[0]) && isdigit(b->name[0])) {
		int a_num = strtol(a->name, NULL, 10);
		int b_num = strtol(b->name, NULL, 10);
		retval = (a_num < b_num) ? -1 : (a_num > b_num);
	} else if (isdigit(a->name[0])) {
		retval = -1;
	} else if (isdigit(b->name[0])) {
		retval = 1;
	}

	return retval;
}

void container_sort_workspaces(struct sway_container *output) {
	list_stable_sort(output->children, sort_workspace_cmp_qsort);
}

/**
 * Get swayc in the direction of newly entered output.
 */
static struct sway_container *get_swayc_in_output_direction(
		struct sway_container *output, enum movement_direction dir,
		struct sway_seat *seat) {
	if (!output) {
		return NULL;
	}

	struct sway_container *ws = seat_get_focus_inactive(seat, output);
	if (ws->type != C_WORKSPACE) {
		ws = container_parent(ws, C_WORKSPACE);
	}

	if (ws == NULL) {
		wlr_log(WLR_ERROR, "got an output without a workspace");
		return NULL;
	}

	if (ws->children->length > 0) {
		switch (dir) {
		case MOVE_LEFT:
			if (ws->layout == L_HORIZ || ws->layout == L_TABBED) {
				// get most right child of new output
				return ws->children->items[ws->children->length-1];
			} else {
				return seat_get_focus_inactive(seat, ws);
			}
		case MOVE_RIGHT:
			if (ws->layout == L_HORIZ || ws->layout == L_TABBED) {
				// get most left child of new output
				return ws->children->items[0];
			} else {
				return seat_get_focus_inactive(seat, ws);
			}
		case MOVE_UP:
		case MOVE_DOWN: {
			struct sway_container *focused =
				seat_get_focus_inactive(seat, ws);
			if (focused && focused->parent) {
				struct sway_container *parent = focused->parent;
				if (parent->layout == L_VERT) {
					if (dir == MOVE_UP) {
						// get child furthest down on new output
						int idx = parent->children->length - 1;
						return parent->children->items[idx];
					} else if (dir == MOVE_DOWN) {
						// get child furthest up on new output
						return parent->children->items[0];
					}
				}
				return focused;
			}
			break;
		}
		default:
			break;
		}
	}

	return ws;
}

static struct sway_container *sway_output_from_wlr(struct wlr_output *output) {
	if (output == NULL) {
		return NULL;
	}
	for (int i = 0; i < root_container.children->length; ++i) {
		struct sway_container *o = root_container.children->items[i];
		if (o->type == C_OUTPUT && o->sway_output->wlr_output == output) {
			return o;
		}
	}
	return NULL;
}

struct sway_container *container_get_in_direction(
		struct sway_container *container, struct sway_seat *seat,
		enum movement_direction dir) {
	struct sway_container *parent = container->parent;

	if (dir == MOVE_CHILD) {
		return seat_get_focus_inactive(seat, container);
	}
	if (container->is_fullscreen) {
		if (dir == MOVE_PARENT) {
			return NULL;
		}
		container = container_parent(container, C_OUTPUT);
		parent = container->parent;
	} else {
		if (dir == MOVE_PARENT) {
			if (parent->type == C_OUTPUT || container_is_floating(container)) {
				return NULL;
			} else {
				return parent;
			}
		}
	}

	struct sway_container *wrap_candidate = NULL;
	while (true) {
		bool can_move = false;
		int desired;
		int idx = index_child(container);
		if (idx == -1) {
			return NULL;
		}
		if (parent->type == C_ROOT) {
			enum wlr_direction wlr_dir = 0;
			if (!sway_assert(sway_dir_to_wlr(dir, &wlr_dir),
						"got invalid direction: %d", dir)) {
				return NULL;
			}
			int lx = container->x + container->width / 2;
			int ly = container->y + container->height / 2;
			struct wlr_output_layout *layout =
				root_container.sway_root->output_layout;
			struct wlr_output *wlr_adjacent =
				wlr_output_layout_adjacent_output(layout, wlr_dir,
					container->sway_output->wlr_output, lx, ly);
			struct sway_container *adjacent =
				sway_output_from_wlr(wlr_adjacent);

			if (!adjacent || adjacent == container) {
				if (!wrap_candidate) {
					return NULL;
				}
				return seat_get_focus_inactive_view(seat, wrap_candidate);
			}
			struct sway_container *next =
				get_swayc_in_output_direction(adjacent, dir, seat);
			if (next == NULL) {
				return NULL;
			}
			struct sway_container *next_workspace = next;
			if (next_workspace->type != C_WORKSPACE) {
				next_workspace = container_parent(next_workspace, C_WORKSPACE);
			}
			sway_assert(next_workspace, "Next container has no workspace");
			if (next_workspace->sway_workspace->fullscreen) {
				return seat_get_focus_inactive(seat,
						next_workspace->sway_workspace->fullscreen);
			}
			if (next->children && next->children->length) {
				// TODO consider floating children as well
				return seat_get_focus_inactive_view(seat, next);
			} else {
				return next;
			}
		} else {
			if (dir == MOVE_LEFT || dir == MOVE_RIGHT) {
				if (parent->layout == L_HORIZ || parent->layout == L_TABBED) {
					can_move = true;
					desired = idx + (dir == MOVE_LEFT ? -1 : 1);
				}
			} else {
				if (parent->layout == L_VERT || parent->layout == L_STACKED) {
					can_move = true;
					desired = idx + (dir == MOVE_UP ? -1 : 1);
				}
			}
		}

		if (can_move) {
			// TODO handle floating
			if (desired < 0 || desired >= parent->children->length) {
				can_move = false;
				int len = parent->children->length;
				if (config->focus_wrapping != WRAP_NO && !wrap_candidate
						&& len > 1) {
					if (desired < 0) {
						wrap_candidate = parent->children->items[len-1];
					} else {
						wrap_candidate = parent->children->items[0];
					}
					if (config->focus_wrapping == WRAP_FORCE) {
						return seat_get_focus_inactive_view(seat,
								wrap_candidate);
					}
				}
			} else {
				struct sway_container *desired_con =
					parent->children->items[desired];
				wlr_log(WLR_DEBUG,
					"cont %d-%p dir %i sibling %d: %p", idx,
					container, dir, desired, desired_con);
				return seat_get_focus_inactive_view(seat, desired_con);
			}
		}

		if (!can_move) {
			container = parent;
			parent = parent->parent;
			if (!parent) {
				// wrapping is the last chance
				if (!wrap_candidate) {
					return NULL;
				}
				return seat_get_focus_inactive_view(seat, wrap_candidate);
			}
		}
	}
}

struct sway_container *container_replace_child(struct sway_container *child,
		struct sway_container *new_child) {
	struct sway_container *parent = child->parent;
	if (parent == NULL) {
		return NULL;
	}
	int i = index_child(child);

	// TODO floating
	if (new_child->parent) {
		container_remove_child(new_child);
	}
	parent->children->items[i] = new_child;
	new_child->parent = parent;
	child->parent = NULL;

	// Set geometry for new child
	new_child->x = child->x;
	new_child->y = child->y;
	new_child->width = child->width;
	new_child->height = child->height;

	// reset geometry for child
	child->width = 0;
	child->height = 0;

	return parent;
}

struct sway_container *container_split(struct sway_container *child,
		enum sway_container_layout layout) {
	// TODO floating: cannot split a floating container
	if (!sway_assert(child, "child cannot be null")) {
		return NULL;
	}
	if (child->type == C_WORKSPACE && child->children->length == 0) {
		// Special case: this just behaves like splitt
		child->prev_layout = child->layout;
		child->layout = layout;
		return child;
	}

	struct sway_container *cont = container_create(C_CONTAINER);

	wlr_log(WLR_DEBUG, "creating container %p around %p", cont, child);

	remove_gaps(child);

	cont->prev_layout = L_NONE;
	cont->width = child->width;
	cont->height = child->height;
	cont->x = child->x;
	cont->y = child->y;

	struct sway_seat *seat = input_manager_get_default_seat(input_manager);
	bool set_focus = (seat_get_focus(seat) == child);

	add_gaps(cont);

	if (child->type == C_WORKSPACE) {
		struct sway_container *workspace = child;
		while (workspace->children->length) {
			struct sway_container *ws_child = workspace->children->items[0];
			container_remove_child(ws_child);
			container_add_child(cont, ws_child);
			wl_signal_emit(&ws_child->events.reparent, workspace);
		}

		container_add_child(workspace, cont);
		enum sway_container_layout old_layout = workspace->layout;
		workspace->layout = layout;
		cont->layout = old_layout;
	} else {
		struct sway_container *old_parent = child->parent;
		cont->layout = layout;
		container_replace_child(child, cont);
		container_add_child(cont, child);
		wl_signal_emit(&child->events.reparent, old_parent);
	}

	if (set_focus) {
		seat_set_focus(seat, cont);
		seat_set_focus(seat, child);
	}

	container_notify_subtree_changed(cont);
	return cont;
}

void container_recursive_resize(struct sway_container *container,
		double amount, enum resize_edge edge) {
	bool layout_match = true;
	wlr_log(WLR_DEBUG, "Resizing %p with amount: %f", container, amount);
	if (edge == RESIZE_EDGE_LEFT || edge == RESIZE_EDGE_RIGHT) {
		container->width += amount;
		layout_match = container->layout == L_HORIZ;
	} else if (edge == RESIZE_EDGE_TOP || edge == RESIZE_EDGE_BOTTOM) {
		container->height += amount;
		layout_match = container->layout == L_VERT;
	}
	if (container->children) {
		for (int i = 0; i < container->children->length; i++) {
			struct sway_container *child = container->children->items[i];
			double amt = layout_match ?
				amount / container->children->length : amount;
			container_recursive_resize(child, amt, edge);
		}
	}
}

static void swap_places(struct sway_container *con1,
		struct sway_container *con2) {
	struct sway_container *temp = malloc(sizeof(struct sway_container));
	temp->x = con1->x;
	temp->y = con1->y;
	temp->width = con1->width;
	temp->height = con1->height;
	temp->parent = con1->parent;

	con1->x = con2->x;
	con1->y = con2->y;
	con1->width = con2->width;
	con1->height = con2->height;

	con2->x = temp->x;
	con2->y = temp->y;
	con2->width = temp->width;
	con2->height = temp->height;

	int temp_index = index_child(con1);
	container_insert_child(con2->parent, con1, index_child(con2));
	container_insert_child(temp->parent, con2, temp_index);

	free(temp);
}

static void swap_focus(struct sway_container *con1,
		struct sway_container *con2, struct sway_seat *seat,
		struct sway_container *focus) {
	if (focus == con1 || focus == con2) {
		struct sway_container *ws1 = container_parent(con1, C_WORKSPACE);
		struct sway_container *ws2 = container_parent(con2, C_WORKSPACE);
		if (focus == con1 && (con2->parent->layout == L_TABBED
					|| con2->parent->layout == L_STACKED)) {
			if (workspace_is_visible(ws2)) {
				seat_set_focus_warp(seat, con2, false, true);
			}
			seat_set_focus(seat, ws1 != ws2 ? con2 : con1);
		} else if (focus == con2 && (con1->parent->layout == L_TABBED
					|| con1->parent->layout == L_STACKED)) {
			if (workspace_is_visible(ws1)) {
				seat_set_focus_warp(seat, con1, false, true);
			}
			seat_set_focus(seat, ws1 != ws2 ? con1 : con2);
		} else if (ws1 != ws2) {
			seat_set_focus(seat, focus == con1 ? con2 : con1);
		} else {
			seat_set_focus(seat, focus);
		}
	} else {
		seat_set_focus(seat, focus);
	}
}

void container_swap(struct sway_container *con1, struct sway_container *con2) {
	if (!sway_assert(con1 && con2, "Cannot swap with nothing")) {
		return;
	}
	if (!sway_assert(con1->type >= C_CONTAINER && con2->type >= C_CONTAINER,
				"Can only swap containers and views")) {
		return;
	}
	if (!sway_assert(!container_has_ancestor(con1, con2)
				&& !container_has_ancestor(con2, con1),
				"Cannot swap ancestor and descendant")) {
		return;
	}
	if (!sway_assert(con1->layout != L_FLOATING && con2->layout != L_FLOATING,
				"Swapping with floating containers is not supported")) {
		return;
	}

	wlr_log(WLR_DEBUG, "Swapping containers %zu and %zu", con1->id, con2->id);

	int fs1 = con1->is_fullscreen;
	int fs2 = con2->is_fullscreen;
	if (fs1) {
		container_set_fullscreen(con1, false);
	}
	if (fs2) {
		container_set_fullscreen(con2, false);
	}

	struct sway_seat *seat = input_manager_get_default_seat(input_manager);
	struct sway_container *focus = seat_get_focus(seat);
	struct sway_container *vis1 = container_parent(
			seat_get_focus_inactive(seat, container_parent(con1, C_OUTPUT)),
			C_WORKSPACE);
	struct sway_container *vis2 = container_parent(
			seat_get_focus_inactive(seat, container_parent(con2, C_OUTPUT)),
			C_WORKSPACE);

	char *stored_prev_name = NULL;
	if (prev_workspace_name) {
		stored_prev_name = strdup(prev_workspace_name);
	}

	swap_places(con1, con2);

	if (!workspace_is_visible(vis1)) {
		seat_set_focus(seat, seat_get_focus_inactive(seat, vis1));
	}
	if (!workspace_is_visible(vis2)) {
		seat_set_focus(seat, seat_get_focus_inactive(seat, vis2));
	}

	swap_focus(con1, con2, seat, focus);

	if (stored_prev_name) {
		free(prev_workspace_name);
		prev_workspace_name = stored_prev_name;
	}

	if (fs1) {
		container_set_fullscreen(con2, true);
	}
	if (fs2) {
		container_set_fullscreen(con1, true);
	}
}
