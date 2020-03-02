#pragma once
#include <stdbool.h>

#include "terminal.h"
#include "fdm.h"
#include "wayland.h"

struct renderer;
struct renderer *render_init(struct fdm *fdm, struct wayland *wayl);
void render_destroy(struct renderer *renderer);

bool render_resize(struct terminal *term, int width, int height);
bool render_resize_force(struct terminal *term, int width, int height);

void render_set_title(struct terminal *term, const char *title);
void render_refresh(struct terminal *term);
bool render_xcursor_set(struct terminal *term);

void render_search_box(struct terminal *term);
void render_csd(struct terminal *term);
void render_csd_title(struct terminal *term);
void render_csd_button(struct terminal *term, enum csd_surface surf_idx);

struct render_worker_context {
    int my_id;
    struct terminal *term;
};
int render_worker_thread(void *_ctx);
