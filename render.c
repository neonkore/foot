#include "render.h"

#include <string.h>

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/prctl.h>

#include <wayland-cursor.h>
#include <xdg-shell.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "shm.h"
#include "grid.h"
#include "font.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

struct font *
attrs_to_font(struct terminal *term, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return &term->fonts[idx];
}

static inline struct rgb
color_hex_to_rgb(uint32_t color)
{
    return (struct rgb){
        ((color >> 16) & 0xff) / 255.,
        ((color >>  8) & 0xff) / 255.,
        ((color >>  0) & 0xff) / 255.,
    };
}

static inline pixman_color_t
color_hex_to_pixman_with_alpha(uint32_t color, uint16_t alpha)
{
    int alpha_div = 0xffff / alpha;
    return (pixman_color_t){
        .red =   ((color >> 16 & 0xff) | (color >> 8 & 0xff00)) / alpha_div,
        .green = ((color >>  8 & 0xff) | (color >> 0 & 0xff00)) / alpha_div,
        .blue =  ((color >>  0 & 0xff) | (color << 8 & 0xff00)) / alpha_div,
        .alpha = alpha,
    };
}

static inline pixman_color_t
color_hex_to_pixman(uint32_t color)
{
    /* Count on the compiler optimizing this */
    return color_hex_to_pixman_with_alpha(color, 0xffff);
}

static inline void
color_dim(struct rgb *rgb)
{
    rgb->r /= 2.;
    rgb->g /= 2.;
    rgb->b /= 2.;
}

static inline void
pixman_color_dim(pixman_color_t *color)
{
    color->red /= 2;
    color->green /= 2;
    color->blue /= 2;
}

static void
draw_bar(const struct terminal *term, pixman_image_t *pix,
         const pixman_color_t *color, int x, int y)
{
    /* TODO: investigate if this is the best way */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){x, y, 1, term->cell_height});
}

static void
draw_underline(const struct terminal *term, pixman_image_t *pix,
               const struct font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    int baseline = y + term->fextents.height - term->fextents.descent;
    int width = font->underline.thickness;
    int y_under = baseline - font->underline.position - width / 2;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){x, y_under, cols * term->cell_width, width});
}

static void
draw_strikeout(const struct terminal *term, pixman_image_t *pix,
               const struct font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    int baseline = y + term->fextents.height - term->fextents.descent;
    int width = font->strikeout.thickness;
    int y_strike = baseline - font->strikeout.position - width / 2;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){x, y_strike, cols * term->cell_width, width});
}

static bool
coord_is_selected(const struct terminal *term, int col, int row)
{
    if (term->selection.start.col == -1 || term->selection.end.col == -1)
        return false;

    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;

    if (start->row > end->row || (start->row == end->row && start->col > end->col)) {
        const struct coord *tmp = start;
        start = end;
        end = tmp;
    }

    assert(start->row <= end->row);

    row += term->grid->view;

    if (start->row == end->row) {
        return row == start->row && col >= start->col && col <= end->col;
    } else {
        if (row == start->row)
            return col >= start->col;
        else if (row == end->row)
            return col <= end->col;
        else
            return row >= start->row && row <= end->row;
    }
}

static void
arm_blink_timer(struct terminal *term)
{
    LOG_DBG("arming blink timer");
    struct itimerspec alarm = {
        .it_value = {.tv_sec = 0, .tv_nsec = 500 * 1000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 500 * 1000000},
    };

    if (timerfd_settime(term->blink.fd, 0, &alarm, NULL) < 0)
        LOG_ERRNO("failed to arm blink timer");
    else
        term->blink.active = true;
}

static int
render_cell(struct terminal *term, pixman_image_t *pix,
            struct cell *cell, int col, int row, bool has_cursor)
{
    if (cell->attrs.clean)
        return 0;

    cell->attrs.clean = 1;

    int width = term->cell_width;
    int height = term->cell_height;
    int x = col * width;
    int y = row * height;

    bool block_cursor = has_cursor && term->cursor_style == CURSOR_BLOCK;
    bool is_selected = coord_is_selected(term, col, row);

    uint32_t _fg = cell->attrs.have_fg
        ? cell->attrs.fg
        : !term->reverse ? term->colors.fg : term->colors.bg;
    uint32_t _bg = cell->attrs.have_bg
        ? cell->attrs.bg
        : !term->reverse ? term->colors.bg : term->colors.fg;

    /* If *one* is set, we reverse */
    if (block_cursor ^ cell->attrs.reverse ^ is_selected) {
        uint32_t swap = _fg;
        _fg = _bg;
        _bg = swap;
    }

    if (cell->attrs.blink && term->blink.state == BLINK_OFF)
        _fg = _bg;

    pixman_color_t fg = color_hex_to_pixman(_fg);
    pixman_color_t bg = color_hex_to_pixman_with_alpha(
        _bg, block_cursor ? 0xffff : term->colors.alpha);

    if (cell->attrs.dim)
        pixman_color_dim(&fg);

    if (block_cursor && term->cursor_color.text >> 31) {
        /* User configured cursor color overrides all attributes */
        assert(term->cursor_color.cursor >> 31);
        fg = color_hex_to_pixman(term->cursor_color.text);
        bg = color_hex_to_pixman(term->cursor_color.cursor);
    }

    struct font *font = attrs_to_font(term, &cell->attrs);
    const struct glyph *glyph = font_glyph_for_wc(font, cell->wc);

    int cell_cols = glyph != NULL ? max(1, glyph->cols) : 1;

    /* Background */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, &bg, 1,
        &(pixman_rectangle16_t){x, y, cell_cols * width, height});

    /* Non-block cursors */
    if (has_cursor) {
        pixman_color_t cursor_color = term->cursor_color.text >> 31
            ? color_hex_to_pixman(term->cursor_color.cursor)
            : color_hex_to_pixman(_fg);

        if (term->cursor_style == CURSOR_BAR)
            draw_bar(term, pix, &cursor_color, x, y);
        else if (term->cursor_style == CURSOR_UNDERLINE)
            draw_underline(
                term, pix, attrs_to_font(term, &cell->attrs), &cursor_color,
                x, y, cell_cols);
    }

    if (cell->attrs.blink && !term->blink.active) {
        /* First cell we see that has blink set - arm blink timer */
        arm_blink_timer(term);
    }

    if (cell->wc == 0 || cell->attrs.conceal)
        return cell_cols;

    if (glyph != NULL) {
        if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
            /* Glyph surface is a pre-rendered image (typically a color emoji...) */
            if (!(cell->attrs.blink && term->blink.state == BLINK_OFF)) {
                pixman_image_composite(
                    PIXMAN_OP_OVER, glyph->pix, NULL, pix, 0, 0, 0, 0,
                    x + glyph->x, y + term->fextents.ascent - glyph->y,
                    glyph->width, glyph->height);
            }
        } else {
            /* Glyph surface is an alpha mask */
            /* TODO: cache */
            pixman_image_t *src = pixman_image_create_solid_fill(&fg);
            pixman_image_composite(
                PIXMAN_OP_OVER, src, glyph->pix, pix, 0, 0, 0, 0,
                x + glyph->x, y + term->fextents.ascent - glyph->y,
                glyph->width, glyph->height);
            pixman_image_unref(src);
        }
    }

    /* Underline */
    if (cell->attrs.underline) {
        pixman_color_t color = color_hex_to_pixman(_fg);
        draw_underline(term, pix, attrs_to_font(term, &cell->attrs),
                       &color, x, y, cell_cols);
    }

    if (cell->attrs.strikethrough) {
        pixman_color_t color = color_hex_to_pixman(_fg);
        draw_strikeout(term, pix, attrs_to_font(term, &cell->attrs),
                       &color, x, y, cell_cols);
    }

    return cell_cols;
}

static void
grid_render_scroll(struct terminal *term, struct buffer *buf,
                   const struct damage *dmg)
{
    int dst_y = (dmg->scroll.region.start + 0) * term->cell_height;
    int src_y = (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
    int width = buf->width;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * term->cell_height;

    LOG_DBG("damage: SCROLL: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, buf->stride,
            buf->size);

    if (height > 0) {
        uint8_t *raw = buf->mmapped;
        memmove(raw + dst_y * buf->stride,
                raw + src_y * buf->stride,
                height * buf->stride);

        wl_surface_damage_buffer(term->wl.surface, 0, dst_y, width, height);
    }
}

static void
grid_render_scroll_reverse(struct terminal *term, struct buffer *buf,
                           const struct damage *dmg)
{
    int src_y = (dmg->scroll.region.start + 0) * term->cell_height;
    int dst_y = (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
    int width = buf->width;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * term->cell_height;

    LOG_DBG("damage: SCROLL REVERSE: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, buf->stride,
            buf->size);

    if (height > 0) {
        uint8_t *raw = buf->mmapped;
        memmove(raw + dst_y * buf->stride,
                raw + src_y * buf->stride,
                height * buf->stride);

        wl_surface_damage_buffer(term->wl.surface, 0, dst_y, width, height);
    }
}

static void
render_row(struct terminal *term, pixman_image_t *pix, struct row *row, int row_no)
{
    for (int col = term->cols - 1; col >= 0; col--)
        render_cell(term, pix, &row->cells[col], col, row_no, false);
}

int
render_worker_thread(void *_ctx)
{
    struct render_worker_context *ctx = _ctx;
    struct terminal *term = ctx->term;
    const int my_id = ctx->my_id;

    char proc_title[16];
    snprintf(proc_title, sizeof(proc_title), "foot:render:%d", my_id);

    if (prctl(PR_SET_NAME, proc_title, 0, 0, 0) < 0)
        LOG_ERRNO("render worker %d: failed to set process title", my_id);

    sem_t *start = &term->render.workers.start;
    sem_t *done = &term->render.workers.done;
    mtx_t *lock = &term->render.workers.lock;
    cnd_t *cond = &term->render.workers.cond;

    while (true) {
        sem_wait(start);

        struct buffer *buf = term->render.workers.buf;
        bool frame_done = false;

        while (!frame_done) {
            mtx_lock(lock);
            while (tll_length(term->render.workers.queue) == 0)
                cnd_wait(cond, lock);

            int row_no = tll_pop_front(term->render.workers.queue);
            mtx_unlock(lock);

            switch (row_no) {
            default:
                assert(buf != NULL);
                render_row(term, buf->pix, grid_row_in_view(term->grid, row_no), row_no);
                break;

            case -1:
                frame_done = true;
                sem_post(done);
                break;

            case -2:
                return 0;
            }
        }
    };

    return -1;
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

void
grid_render(struct terminal *term)
{
#define TIME_FRAME_RENDERING 0

#if TIME_FRAME_RENDERING
    struct timeval start_time;
    gettimeofday(&start_time, NULL);
#endif

    assert(term->width > 0);
    assert(term->height > 0);

    struct buffer *buf = shm_get_buffer(term->wl.shm, term->width, term->height, 1 + term->render.workers.count);
    pixman_image_t *pix = buf->pix;

    bool all_clean = tll_length(term->grid->scroll_damage) == 0;

    /* Erase old cursor (if we rendered a cursor last time) */
    if (term->render.last_cursor.cell != NULL) {
        struct cell *cell = term->render.last_cursor.cell;
        struct coord at = term->render.last_cursor.in_view;

        if (cell->attrs.clean) {
            cell->attrs.clean = 0;
            render_cell(term, pix, cell, at.col, at.row, false);

            wl_surface_damage_buffer(
                term->wl.surface,
                at.col * term->cell_width, at.row * term->cell_height,
                term->cell_width, term->cell_height);
        }
        term->render.last_cursor.cell = NULL;

        if (term->render.last_cursor.actual.col != term->cursor.col ||
            term->render.last_cursor.actual.row != term->cursor.row)
        {
            /* Detect cursor movement - we don't dirty cells touched
             * by the cursor, since only the final cell matters. */
            all_clean = false;
        }
    }

    if (term->flash.active)
        term_damage_view(term);

    /* If we resized the window, or is flashing, or just stopped flashing */
    if (term->render.last_buf != buf || term->flash.active || term->render.was_flashing) {
        LOG_DBG("new buffer");

        /* Fill area outside the cell grid with the default background color */
        int rmargin = term->cols * term->cell_width;
        int bmargin = term->rows * term->cell_height;
        int rmargin_width = term->width - rmargin;
        int bmargin_height = term->height - bmargin;

        uint32_t _bg = !term->reverse ? term->colors.bg : term->colors.fg;

        pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, term->colors.alpha);
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, pix, &bg, 1,
            &(pixman_rectangle16_t){rmargin, 0, rmargin_width, term->height});

        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, pix, &bg, 1,
            &(pixman_rectangle16_t){0, bmargin, term->width, bmargin_height});

        wl_surface_damage_buffer(
            term->wl.surface, rmargin, 0, rmargin_width, term->height);
        wl_surface_damage_buffer(
            term->wl.surface, 0, bmargin, term->width, bmargin_height);

        /* Force a full grid refresh */
        term_damage_view(term);

        term->render.last_buf = buf;
        term->render.was_flashing = term->flash.active;
    }

    tll_foreach(term->grid->scroll_damage, it) {
        switch (it->item.type) {
        case DAMAGE_SCROLL:
            grid_render_scroll(term, buf, &it->item);
            break;

        case DAMAGE_SCROLL_REVERSE:
            grid_render_scroll_reverse(term, buf, &it->item);
            break;
        }

        tll_remove(term->grid->scroll_damage, it);
    }

    if (term->render.workers.count > 0) {

        term->render.workers.buf = buf;
        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_post(&term->render.workers.start);

        assert(tll_length(term->render.workers.queue) == 0);

        for (int r = 0; r < term->rows; r++) {
            struct row *row = grid_row_in_view(term->grid, r);

            if (!row->dirty)
                continue;

            mtx_lock(&term->render.workers.lock);
            tll_push_back(term->render.workers.queue, r);
            cnd_signal(&term->render.workers.cond);
            mtx_unlock(&term->render.workers.lock);

            row->dirty = false;
            all_clean = false;

            wl_surface_damage_buffer(
                term->wl.surface,
                0, r * term->cell_height,
                term->width, term->cell_height);
        }

        mtx_lock(&term->render.workers.lock);
        for (size_t i = 0; i < term->render.workers.count; i++)
            tll_push_back(term->render.workers.queue, -1);
        cnd_broadcast(&term->render.workers.cond);
        mtx_unlock(&term->render.workers.lock);
    } else {
        for (int r = 0; r < term->rows; r++) {
            struct row *row = grid_row_in_view(term->grid, r);

            if (!row->dirty)
                continue;

            render_row(term, pix, row, r);

            row->dirty = false;
            all_clean = false;

            wl_surface_damage_buffer(
                term->wl.surface,
                0, r * term->cell_height,
                term->width, term->cell_height);
        }
    }

    if (term->blink.active) {
        /* Check if there are still any visible blinking cells */
        bool none_is_blinking = true;
        for (int r = 0; r < term->rows; r++) {
            struct row *row = grid_row_in_view(term->grid, r);
            for (int col = 0; col < term->cols; col++) {
                if (row->cells[col].attrs.blink) {
                    none_is_blinking = false;
                    break;
                }
            }
        }

        /* No, disarm the blink timer */
        if (none_is_blinking) {
            LOG_DBG("disarming blink timer");

            term->blink.active = false;
            term->blink.state = BLINK_ON;

            if (timerfd_settime(
                    term->blink.fd, 0,
                    &(struct itimerspec){{0}}, NULL)  < 0)
            {
                LOG_ERRNO("failed to disarm blink timer");
            }
        }
    }

    /*
     * Determine if we need to render a cursor or not. The cursor
     * could be hidden. Or it could have been scrolled out of view.
     */
    bool cursor_is_visible = false;
    int view_end = (term->grid->view + term->rows - 1) % term->grid->num_rows;
    int cursor_row = (term->grid->offset + term->cursor.row) % term->grid->num_rows;
    if (view_end >= term->grid->view) {
        /* Not wrapped */
        if (cursor_row >= term->grid->view && cursor_row <= view_end)
            cursor_is_visible = true;
    } else {
        /* Wrapped */
        if (cursor_row >= term->grid->view || cursor_row <= view_end)
            cursor_is_visible = true;
    }

    /*
     * Wait for workers to finish before we render the cursor. This is
     * because the cursor cell might be dirty, in which case a worker
     * will render it (but without the cursor).
     */
    if (term->render.workers.count > 0) {
        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_wait(&term->render.workers.done);
        term->render.workers.buf = NULL;
    }

    if (cursor_is_visible && !term->hide_cursor) {
        /* Remember cursor coordinates so that we can erase it next
         * time. Note that we need to re-align it against the view. */
        int view_aligned_row
            = (cursor_row - term->grid->view + term->grid->num_rows)
            % term->grid->num_rows;

        term->render.last_cursor.actual = term->cursor;
        term->render.last_cursor.in_view = (struct coord) {
            term->cursor.col, view_aligned_row};

        struct row *row = grid_row_in_view(term->grid, view_aligned_row);
        struct cell *cell = &row->cells[term->cursor.col];

        cell->attrs.clean = 0;
        term->render.last_cursor.cell = cell;
        int cols_updated = render_cell(
            term, pix, cell, term->cursor.col, view_aligned_row, true);

        wl_surface_damage_buffer(
            term->wl.surface,
            term->cursor.col * term->cell_width,
            view_aligned_row * term->cell_height,
            cols_updated * term->cell_width, term->cell_height);
    }

    if (all_clean) {
        buf->busy = false;
        return;
    }

    if (term->flash.active) {
        /* Note: alpha is pre-computed in each color component */
        pixman_image_fill_rectangles(
            PIXMAN_OP_OVER, pix,
            &(pixman_color_t){.red=0x7fff, .green=0x7fff, .blue=0, .alpha=0x7fff},
            1, &(pixman_rectangle16_t){0, 0, term->width, term->height});

        wl_surface_damage_buffer(
            term->wl.surface, 0, 0, term->width, term->height);
    }

    assert(term->grid->offset >= 0 && term->grid->offset < term->grid->num_rows);
    assert(term->grid->view >= 0 && term->grid->view < term->grid->num_rows);

    wl_surface_attach(term->wl.surface, buf->wl_buf, 0, 0);

    assert(term->render.frame_callback == NULL);
    term->render.frame_callback = wl_surface_frame(term->wl.surface);
    wl_callback_add_listener(term->render.frame_callback, &frame_listener, term);

    wl_surface_set_buffer_scale(term->wl.surface, term->scale);
    wl_surface_commit(term->wl.surface);

#if TIME_FRAME_RENDERING
    struct timeval end_time;
    gettimeofday(&end_time, NULL);

    struct timeval render_time;
    timersub(&end_time, &start_time, &render_time);
    LOG_INFO("frame rendered in %lds %ldms",
             render_time.tv_sec, render_time.tv_usec / 1000);
#endif
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct terminal *term = data;

    assert(term->render.frame_callback == wl_callback);
    wl_callback_destroy(wl_callback);
    term->render.frame_callback = NULL;
    grid_render(term);
}

static void
reflow(struct row **new_grid, int new_cols, int new_rows,
       struct row *const *old_grid, int old_cols, int old_rows)
{
    /* TODO: actually reflow */
    for (int r = 0; r < min(new_rows, old_rows); r++) {
        size_t copy_cols = min(new_cols, old_cols);
        size_t clear_cols = new_cols - copy_cols;

        if (old_grid[r] == NULL)
            continue;

        if (new_grid[r] == NULL)
            new_grid[r] = grid_row_alloc(new_cols);

        struct cell *new_cells = new_grid[r]->cells;
        const struct cell *old_cells = old_grid[r]->cells;

        new_grid[r]->dirty = old_grid[r]->dirty;
        memcpy(new_cells, old_cells, copy_cols * sizeof(new_cells[0]));
        memset(&new_cells[copy_cols], 0, clear_cols * sizeof(new_cells[0]));
    }
}

/* Move to terminal.c? */
void
render_resize(struct terminal *term, int width, int height)
{
    int scale = -1;
    tll_foreach(term->wl.on_outputs, it) {
        if (it->item->scale > scale)
            scale = it->item->scale;
    }

    if (scale == -1) {
        /* Haven't 'entered' an output yet? */
        scale = 1;
    }

    width *= term->scale;
    height *= term->scale;

    if (width == 0 && height == 0) {
        /* Assume we're not fully up and running yet */
        return;
    }

    if (width == term->width && height == term->height && scale == term->scale)
        return;

    term->width = width;
    term->height = height;
    term->scale = scale;

    const int scrollback_lines = term->render.scrollback_lines;

    const int old_cols = term->cols;
    const int old_rows = term->rows;
    const int old_normal_grid_rows = term->normal.num_rows;
    const int old_alt_grid_rows = term->alt.num_rows;

    const int new_cols = term->width / term->cell_width;
    const int new_rows = term->height / term->cell_height;
    const int new_normal_grid_rows = new_rows + scrollback_lines;
    const int new_alt_grid_rows = new_rows;

    term->normal.offset %= new_normal_grid_rows;
    term->normal.view %= new_normal_grid_rows;

    term->alt.offset %= new_alt_grid_rows;
    term->alt.view %= new_alt_grid_rows;

    /* Allocate new 'normal' grid */
    struct row **normal = calloc(new_normal_grid_rows, sizeof(normal[0]));
    for (int r = 0; r < new_rows; r++)
        normal[(term->normal.view + r) % new_normal_grid_rows] = grid_row_alloc(new_cols);

    /* Allocate new 'alt' grid */
    struct row **alt = calloc(new_alt_grid_rows, sizeof(alt[0]));
    for (int r = 0; r < new_rows; r++)
        alt[(term->alt.view + r) % new_alt_grid_rows] = grid_row_alloc(new_cols);

    /* Reflow content */
    reflow(normal, new_cols, new_normal_grid_rows,
           term->normal.rows, old_cols, old_normal_grid_rows);
    reflow(alt, new_cols, new_alt_grid_rows,
           term->alt.rows, old_cols, old_alt_grid_rows);

    /* Free old 'normal' grid */
    for (int r = 0; r < term->normal.num_rows; r++)
        grid_row_free(term->normal.rows[r]);
    free(term->normal.rows);

    /* Free old 'alt' grid */
    for (int r = 0; r < term->alt.num_rows; r++)
        grid_row_free(term->alt.rows[r]);
    free(term->alt.rows);

    term->cols = new_cols;
    term->rows = new_rows;

    term->normal.rows = normal;
    term->normal.num_rows = new_normal_grid_rows;
    term->normal.num_cols = new_cols;
    term->alt.rows = alt;
    term->alt.num_rows = new_alt_grid_rows;
    term->alt.num_cols = new_cols;

    LOG_DBG("resize: %dx%d, grid: cols=%d, rows=%d",
             term->width, term->height, term->cols, term->rows);

    /* Signal TIOCSWINSZ */
    if (ioctl(term->ptmx, TIOCSWINSZ,
              &(struct winsize){
                  .ws_row = term->rows,
                  .ws_col = term->cols,
                  .ws_xpixel = term->width,
                  .ws_ypixel = term->height}) == -1)
    {
        LOG_ERRNO("TIOCSWINSZ");
    }

    if (term->scroll_region.start >= term->rows)
        term->scroll_region.start = 0;

    if (term->scroll_region.end >= old_rows)
        term->scroll_region.end = term->rows;

    term_cursor_to(
        term,
        min(term->cursor.row, term->rows - 1),
        min(term->cursor.col, term->cols - 1));

    term->render.last_cursor.cell = NULL;

    term_damage_view(term);
    render_refresh(term);
}

void
render_set_title(struct terminal *term, const char *title)
{
    xdg_toplevel_set_title(term->wl.xdg_toplevel, title);
}

void
render_update_cursor_surface(struct terminal *term)
{
    if (term->wl.pointer.cursor == NULL)
        return;

    const int scale = term->scale;
    wl_surface_set_buffer_scale(term->wl.pointer.surface, scale);

    struct wl_cursor_image *image = term->wl.pointer.cursor->images[0];

    wl_surface_attach(
        term->wl.pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        term->wl.pointer.pointer, term->wl.pointer.serial,
        term->wl.pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        term->wl.pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_commit(term->wl.pointer.surface);
}

void
render_refresh(struct terminal *term)
{
    if (term->render.frame_callback == NULL)
        grid_render(term);
}
