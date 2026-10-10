/* $OpenBSD$ */

/*
 * Copyright (c) 2026 calii23 <https://github.com/calii23>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <resolv.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include "tmux.h"

/*
 * Kitty graphics protocol, described at:
 *
 *     https://sw.kovidgoyal.net/kitty/graphics-protocol/
 *
 * Every command has the form:
 *
 *     APC G key=value,key=value ; payload ST
 *
 * tmux is the terminal for the programs in its panes and a program for the
 * client terminals:
 *
 * - Images are stored by tmux, per screen (the main and alternate screens each
 *   have their own, as in kitty, and both are kept). Pixel data is kept as the
 *   base64 payload and never decoded. Data sent as a file, temporary file or
 *   shared memory is read once and stored like direct data.
 *
 * - Each image has a serial which is its id on every client terminal. Images
 *   are uploaded to a client the first time that client needs them, so
 *   nothing is lost when a pane is hidden, a redraw is pending or a client
 *   attaches later.
 *
 * - Placements are anchored to the grid line holding their top row (struct
 *   grid_line anchor), so they move with the text when it scrolls, lines are
 *   inserted or deleted, the screen is reflowed or copied for copy mode.
 *
 * - Virtual placements (U=1) are drawn by the unicode placeholder cells in the
 *   grid. When drawing a placeholder, the pane image and placement ids in its
 *   colours are swapped for the client ones and the row and column
 *   diacritics are filled in.
 *
 * - Other placements are placed by tmux on each client after redraws and
 *   output. Each is split into the parts not hidden by other panes, menus or
 *   the edge of the pane, and each part is cropped using the source
 *   rectangle. The placements on each client are remembered so only changes
 *   are sent.
 */

/* Biggest payload chunk sent to a terminal. */
#define GRAPHICS_CHUNK 4096

/* Most base64 data kept for all images before the oldest are freed. */
#define GRAPHICS_MAX_TOTAL (320 * 1024 * 1024)

/* Biggest single image or file. */
#define GRAPHICS_MAX_SIZE (128 * 1024 * 1024)

/* Biggest image or source rectangle in pixels and placement in cells. */
#define GRAPHICS_MAX_PIXELS 100000
#define GRAPHICS_MAX_CELLS 10000

/* Most parents a relative placement can have. */
#define GRAPHICS_MAX_DEPTH 8

/* Most frame and animation commands kept for one image. */
#define GRAPHICS_MAX_OPS 4096

/* Most visible parts of one placement. */
#define GRAPHICS_MAX_PARTS 8

/* Image serials fit in 24 bits, the last is used for unknown images. */
#define GRAPHICS_SERIAL_MAX 0xfffffe
#define GRAPHICS_SERIAL_UNKNOWN 0xffffff

/* Placement serials, times the parts must fit in 24 bits. */
#define GRAPHICS_PLACEMENT_MAX 0x1fffff

/* Unicode placeholder character. */
#define GRAPHICS_PLACEHOLDER 0x10eeee

/* A command to replay when uploading an image (frames and animation). */
struct graphics_op {
	char				*keys;
	char				*data;
	size_t				 size;
	size_t				 cost;

	TAILQ_ENTRY(graphics_op)	 entry;
};
TAILQ_HEAD(graphics_ops, graphics_op);

/* An image. */
struct graphics_image {
	struct graphics			*gr;

	u_int				 id;
	u_int				 number;
	u_int				 serial;

	u_int				 format;
	char				 compression;
	u_int				 width;
	u_int				 height;

	char				*data;
	size_t				 size;
	struct graphics_ops		 ops;
	u_int				 nops;
	size_t				 total;

	TAILQ_ENTRY(graphics_image)	 entry;
	TAILQ_ENTRY(graphics_image)	 all_entry;
	RB_ENTRY(graphics_image)	 serial_entry;
};
TAILQ_HEAD(graphics_images, graphics_image);
RB_HEAD(graphics_serials, graphics_image);

/* A placement of an image. */
struct graphics_placement {
	struct graphics_image		*im;

	u_int				 id;
	u_int				 serial;

	int				 flags;
#define GRAPHICS_VIRTUAL 0x1
#define GRAPHICS_COLS 0x2
#define GRAPHICS_ROWS 0x4
#define GRAPHICS_WRAPPED 0x8

	u_int				 anchor;
	u_int				 hint;
	u_int				 x;
	u_int				 cols;
	u_int				 rows;

	u_int				 src_x;
	u_int				 src_y;
	u_int				 src_w;
	u_int				 src_h;
	u_int				 x_off;
	u_int				 y_off;
	int				 z;

	u_int				 req_cols; /* as given, 0 if not */
	u_int				 req_rows;

	u_int				 parent;
	int				 h_off; /* from parent */
	int				 v_off;
	u_int				 wrap_x; /* position while reflowing */
	u_int				 wrap_y;

	TAILQ_ENTRY(graphics_placement)	 entry;
};
TAILQ_HEAD(graphics_placements, graphics_placement);

/* A parsed command. */
struct graphics_cmd {
	uint64_t			 set;

	char				 action;
	char				 medium;
	char				 compression;
	char				 delete;

	u_int				 quiet;
	u_int				 format;
	u_int				 id;
	u_int				 number;
	u_int				 placement;
	u_int				 width;
	u_int				 height;
	u_int				 size;
	u_int				 offset;
	u_int				 more;

	u_int				 x;
	u_int				 y;
	u_int				 w;
	u_int				 h;
	u_int				 x_off;
	u_int				 y_off;
	u_int				 cols;
	u_int				 rows;
	u_int				 cursor;
	u_int				 virtual;
	int				 z;
	u_int				 parent_id;
	u_int				 parent_placement;
	int				 h_off;
	int				 v_off;

	char				*keys;
	char				*data;
	size_t				 datalen;
	int				 toobig;
};

/* Images and placements for a screen. */
struct graphics {
	struct graphics_images		 images;
	struct graphics_placements	 placements;
	struct graphics_cmd		*loading;
	struct graphics_image		*last;

	int				 serials; /* cells use client ids */
};

/* An image uploaded to a client terminal. */
struct graphics_upload {
	u_int				 serial;
	RB_ENTRY(graphics_upload)	 entry;
};
RB_HEAD(graphics_uploads, graphics_upload);

/* A placement (or part of one) on a client terminal. */
struct graphics_part {
	struct graphics_image		*im; /* only while collecting */

	u_int				 serial;
	u_int				 id;

	u_int				 x;
	u_int				 y;
	u_int				 src_x;
	u_int				 src_y;
	u_int				 src_w;
	u_int				 src_h;
	u_int				 x_off;
	u_int				 y_off;
	u_int				 cols;
	u_int				 rows;
	int				 z;
	int				 flags;

	TAILQ_ENTRY(graphics_part)	 entry;
};
TAILQ_HEAD(graphics_parts, graphics_part);

/* Client terminal state. */
struct graphics_client {
	struct graphics_uploads		 uploads;
	struct graphics_parts		 shown;
	uint64_t			 generation;
	int				 flags;
#define GRAPHICS_CLIENT_CHECK 0x1
#define GRAPHICS_CLIENT_FORCE 0x2
};

static int	graphics_serial_cmp(struct graphics_image *,
		    struct graphics_image *);
RB_GENERATE_STATIC(graphics_serials, graphics_image, serial_entry,
    graphics_serial_cmp);

static int	graphics_upload_cmp(struct graphics_upload *,
		    struct graphics_upload *);
RB_GENERATE_STATIC(graphics_uploads, graphics_upload, entry,
    graphics_upload_cmp);

static struct graphics_images	graphics_all =
    TAILQ_HEAD_INITIALIZER(graphics_all);
static struct graphics_serials	graphics_serials =
    RB_INITIALIZER(&graphics_serials);
static size_t			graphics_total;
static u_int			graphics_next_serial;
static u_int			graphics_next_placement;
static u_int			graphics_next_anchor;
static uint64_t			graphics_generation;

/* Placeholder cells not drawn yet because their diacritics may be to come. */
#define GRAPHICS_MAX_HELD 256
static struct {
	struct graphics	*gr;
	u_int		 px;
	u_int		 py;
}				graphics_held[GRAPHICS_MAX_HELD];
static u_int			graphics_nheld;

/* Row and column diacritics, from kitty's rowcolumn-diacritics.txt. */
static const u_int graphics_diacritics[] = {
	0x305, 0x30d, 0x30e, 0x310, 0x312, 0x33d, 0x33e, 0x33f,
	0x346, 0x34a, 0x34b, 0x34c, 0x350, 0x351, 0x352, 0x357,
	0x35b, 0x363, 0x364, 0x365, 0x366, 0x367, 0x368, 0x369,
	0x36a, 0x36b, 0x36c, 0x36d, 0x36e, 0x36f, 0x483, 0x484,
	0x485, 0x486, 0x487, 0x592, 0x593, 0x594, 0x595, 0x597,
	0x598, 0x599, 0x59c, 0x59d, 0x59e, 0x59f, 0x5a0, 0x5a1,
	0x5a8, 0x5a9, 0x5ab, 0x5ac, 0x5af, 0x5c4, 0x610, 0x611,
	0x612, 0x613, 0x614, 0x615, 0x616, 0x617, 0x657, 0x658,
	0x659, 0x65a, 0x65b, 0x65d, 0x65e, 0x6d6, 0x6d7, 0x6d8,
	0x6d9, 0x6da, 0x6db, 0x6dc, 0x6df, 0x6e0, 0x6e1, 0x6e2,
	0x6e4, 0x6e7, 0x6e8, 0x6eb, 0x6ec, 0x730, 0x732, 0x733,
	0x735, 0x736, 0x73a, 0x73d, 0x73f, 0x740, 0x741, 0x743,
	0x745, 0x747, 0x749, 0x74a, 0x7eb, 0x7ec, 0x7ed, 0x7ee,
	0x7ef, 0x7f0, 0x7f1, 0x7f3, 0x816, 0x817, 0x818, 0x819,
	0x81b, 0x81c, 0x81d, 0x81e, 0x81f, 0x820, 0x821, 0x822,
	0x823, 0x825, 0x826, 0x827, 0x829, 0x82a, 0x82b, 0x82c,
	0x82d, 0x951, 0x953, 0x954, 0xf82, 0xf83, 0xf86, 0xf87,
	0x135d, 0x135e, 0x135f, 0x17dd, 0x193a, 0x1a17, 0x1a75, 0x1a76,
	0x1a77, 0x1a78, 0x1a79, 0x1a7a, 0x1a7b, 0x1a7c, 0x1b6b, 0x1b6d,
	0x1b6e, 0x1b6f, 0x1b70, 0x1b71, 0x1b72, 0x1b73, 0x1cd0, 0x1cd1,
	0x1cd2, 0x1cda, 0x1cdb, 0x1ce0, 0x1dc0, 0x1dc1, 0x1dc3, 0x1dc4,
	0x1dc5, 0x1dc6, 0x1dc7, 0x1dc8, 0x1dc9, 0x1dcb, 0x1dcc, 0x1dd1,
	0x1dd2, 0x1dd3, 0x1dd4, 0x1dd5, 0x1dd6, 0x1dd7, 0x1dd8, 0x1dd9,
	0x1dda, 0x1ddb, 0x1ddc, 0x1ddd, 0x1dde, 0x1ddf, 0x1de0, 0x1de1,
	0x1de2, 0x1de3, 0x1de4, 0x1de5, 0x1de6, 0x1dfe, 0x20d0, 0x20d1,
	0x20d4, 0x20d5, 0x20d6, 0x20d7, 0x20db, 0x20dc, 0x20e1, 0x20e7,
	0x20e9, 0x20f0, 0x2cef, 0x2cf0, 0x2cf1, 0x2de0, 0x2de1, 0x2de2,
	0x2de3, 0x2de4, 0x2de5, 0x2de6, 0x2de7, 0x2de8, 0x2de9, 0x2dea,
	0x2deb, 0x2dec, 0x2ded, 0x2dee, 0x2def, 0x2df0, 0x2df1, 0x2df2,
	0x2df3, 0x2df4, 0x2df5, 0x2df6, 0x2df7, 0x2df8, 0x2df9, 0x2dfa,
	0x2dfb, 0x2dfc, 0x2dfd, 0x2dfe, 0x2dff, 0xa66f, 0xa67c, 0xa67d,
	0xa6f0, 0xa6f1, 0xa8e0, 0xa8e1, 0xa8e2, 0xa8e3, 0xa8e4, 0xa8e5,
	0xa8e6, 0xa8e7, 0xa8e8, 0xa8e9, 0xa8ea, 0xa8eb, 0xa8ec, 0xa8ed,
	0xa8ee, 0xa8ef, 0xa8f0, 0xa8f1, 0xaab0, 0xaab2, 0xaab3, 0xaab7,
	0xaab8, 0xaabe, 0xaabf, 0xaac1, 0xfe20, 0xfe21, 0xfe22, 0xfe23,
	0xfe24, 0xfe25, 0xfe26, 0x10a0f, 0x10a38, 0x1d185, 0x1d186, 0x1d187,
	0x1d188, 0x1d189, 0x1d1aa, 0x1d1ab, 0x1d1ac, 0x1d1ad, 0x1d242, 0x1d243,
	0x1d244
};
#define GRAPHICS_NDIACRITICS (nitems(graphics_diacritics))

static void	graphics_image_free(struct graphics_image *);
static void	graphics_placement_free(struct graphics_placement *);
static void	graphics_upload(struct client *, struct graphics_image *);
static void	graphics_free_parts(struct graphics_parts *);
static int	graphics_placement_hits(struct graphics_placement *,
		    struct window_pane *, int, int, int, int, int);

static int
graphics_serial_cmp(struct graphics_image *im1, struct graphics_image *im2)
{
	if (im1->serial < im2->serial)
		return (-1);
	return (im1->serial > im2->serial);
}

static int
graphics_upload_cmp(struct graphics_upload *gu1, struct graphics_upload *gu2)
{
	if (gu1->serial < gu2->serial)
		return (-1);
	return (gu1->serial > gu2->serial);
}

/* Does this client terminal support the protocol? */
static int
graphics_client_supported(struct client *c)
{
	if (c->session == NULL || c->tty.term == NULL)
		return (0);
	if (c->flags & (CLIENT_CONTROL|CLIENT_SUSPENDED))
		return (0);
	return ((c->tty.term->flags & TERM_KITTYGRAPHICS) != 0);
}

/* Is the pane shown on a client terminal which supports the protocol? */
static int
graphics_pane_supported(struct window_pane *wp)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (graphics_client_supported(c) &&
		    session_has(c->session, wp->window))
			return (1);
	}
	return (0);
}

/* Get client state. */
static struct graphics_client *
graphics_client_get(struct client *c)
{
	struct graphics_client	*gcl = c->graphics;

	if (gcl == NULL) {
		gcl = c->graphics = xcalloc(1, sizeof *gcl);
		RB_INIT(&gcl->uploads);
		TAILQ_INIT(&gcl->shown);
		gcl->flags = GRAPHICS_CLIENT_CHECK;
	}
	return (gcl);
}

/* Has this client got an image? */
static struct graphics_upload *
graphics_client_has(struct client *c, u_int serial)
{
	struct graphics_upload	find;

	if (c->graphics == NULL)
		return (NULL);
	find.serial = serial;
	return (RB_FIND(graphics_uploads, &c->graphics->uploads, &find));
}

/* Write to a client terminal. This must not be discarded. */
static void
graphics_write(struct tty *tty, const char *keys, const char *data,
    size_t size)
{
	char	*s;
	size_t	 n;
	int	 first = 1;

	if (size == 0) {
		xasprintf(&s, "\033_G%s,q=2\033\\", keys);
		tty_write_noblock(tty, s, strlen(s));
		free(s);
		return;
	}
	while (size != 0) {
		n = size > GRAPHICS_CHUNK ? GRAPHICS_CHUNK : size;
		if (first)
			xasprintf(&s, "\033_G%s,q=2,m=%d;", keys, n != size);
		else
			xasprintf(&s, "\033_Gm=%d,q=2;", n != size);
		tty_write_noblock(tty, s, strlen(s));
		tty_write_noblock(tty, data, n);
		tty_write_noblock(tty, "\033\\", 2);
		free(s);
		first = 0;
		data += n;
		size -= n;
	}
}

/* Write to every client terminal which has an image. */
static void
graphics_write_all(struct graphics_image *im, const char *keys,
    const char *data, size_t size)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (graphics_client_has(c, im->serial) != NULL)
			graphics_write(&c->tty, keys, data, size);
	}
}

/* Get the client placement id of a part of a placement. */
static u_int
graphics_placement_part(struct graphics_placement *pl, u_int part)
{
	return (pl->serial * GRAPHICS_MAX_PARTS + part);
}

/* Write a virtual placement to a client terminal. */
static void
graphics_write_virtual(struct tty *tty, struct graphics_placement *pl)
{
	struct graphics_image	*im = pl->im;
	char			*keys, size[128];
	size_t			 n = 0;

	/* Only what was given, so the terminal sizes the rest itself. */
	*size = '\0';
	if (pl->src_x != 0 || pl->src_y != 0) {
		n += xsnprintf(size + n, sizeof size - n, ",x=%u,y=%u",
		    pl->src_x, pl->src_y);
	}
	if (pl->src_w != (im->width > pl->src_x ? im->width - pl->src_x : 0))
		n += xsnprintf(size + n, sizeof size - n, ",w=%u", pl->src_w);
	if (pl->src_h != (im->height > pl->src_y ? im->height - pl->src_y : 0))
		n += xsnprintf(size + n, sizeof size - n, ",h=%u", pl->src_h);
	if (pl->req_cols != 0)
		n += xsnprintf(size + n, sizeof size - n, ",c=%u", pl->req_cols);
	if (pl->req_rows != 0)
		n += xsnprintf(size + n, sizeof size - n, ",r=%u", pl->req_rows);

	xasprintf(&keys, "a=p,U=1,i=%u,p=%u%s", im->serial,
	    graphics_placement_part(pl, 0), size);
	graphics_write(tty, keys, NULL, 0);
	free(keys);
}

/* Remove a placement (or part) from a client terminal. */
static void
graphics_write_delete(struct tty *tty, u_int serial, u_int id)
{
	char	*keys;

	xasprintf(&keys, "a=d,d=i,i=%u,p=%u", serial, id);
	graphics_write(tty, keys, NULL, 0);
	free(keys);
}

/* Mark an image as used. */
static void
graphics_image_touch(struct graphics_image *im)
{
	TAILQ_REMOVE(&graphics_all, im, all_entry);
	TAILQ_INSERT_TAIL(&graphics_all, im, all_entry);
}

/* Find an image by id. */
static struct graphics_image *
graphics_find_id(struct graphics *gr, u_int id)
{
	struct graphics_image	*im;

	if (gr == NULL || id == 0)
		return (NULL);
	if (gr->last != NULL && gr->last->id == id)
		return (gr->last);
	TAILQ_FOREACH(im, &gr->images, entry) {
		if (im->id == id)
			return (gr->last = im);
	}
	return (NULL);
}

/* Find the newest image with a number. */
static struct graphics_image *
graphics_find_number(struct graphics *gr, u_int number)
{
	struct graphics_image	*im;

	if (gr == NULL || number == 0)
		return (NULL);
	TAILQ_FOREACH_REVERSE(im, &gr->images, graphics_images, entry) {
		if (im->number == number)
			return (im);
	}
	return (NULL);
}

/* Find the image for a command. */
static struct graphics_image *
graphics_find(struct graphics *gr, struct graphics_cmd *cmd)
{
	if (cmd->id != 0)
		return (graphics_find_id(gr, cmd->id));
	return (graphics_find_number(gr, cmd->number));
}

/* Find a placement. */
static struct graphics_placement *
graphics_find_placement(struct graphics *gr, struct graphics_image *im,
    u_int id)
{
	struct graphics_placement	*pl;

	TAILQ_FOREACH(pl, &gr->placements, entry) {
		if (pl->im == im && pl->id == id)
			return (pl);
	}
	return (NULL);
}

/* Find any placement of an image. */
static struct graphics_placement *
graphics_find_placement_any(struct graphics *gr, struct graphics_image *im)
{
	struct graphics_placement	*pl;

	TAILQ_FOREACH(pl, &gr->placements, entry) {
		if (pl->im == im)
			return (pl);
	}
	return (NULL);
}

/* Get the anchor for a grid line, adding one if needed. */
static u_int
graphics_anchor(struct grid *gd, u_int py)
{
	struct grid_line	*gl = grid_get_line(gd, py);

	if (gl->anchor == 0) {
		if (++graphics_next_anchor == 0)
			graphics_next_anchor = 1;
		gl->anchor = graphics_next_anchor;
	}
	return (gl->anchor);
}

/* Find the grid line with an anchor, starting at the hint. */
static int
graphics_find_anchor(struct grid *gd, u_int anchor, u_int *hint)
{
	u_int	n = gd->hsize + gd->sy, i;

	if (n == 0)
		return (0);
	if (*hint >= n)
		*hint = n - 1;
	for (i = *hint + 1; i > 0; i--) {
		if (gd->linedata[i - 1].anchor == anchor) {
			*hint = i - 1;
			return (1);
		}
	}
	for (i = *hint + 1; i < n; i++) {
		if (gd->linedata[i].anchor == anchor) {
			*hint = i;
			return (1);
		}
	}
	return (0);
}

/* Find a placement by serial. */
static struct graphics_placement *
graphics_find_placement_serial(struct graphics *gr, u_int serial)
{
	struct graphics_placement	*pl;

	TAILQ_FOREACH(pl, &gr->placements, entry) {
		if (pl->serial == serial)
			return (pl);
	}
	return (NULL);
}

/*
 * Get the grid line and column of a placement, adding the offsets of relative
 * placements to their parent. Returns 0 if it is gone.
 */
static int
graphics_placement_position(struct grid *gd, struct graphics_placement *pl,
    int *py, int *px)
{
	struct graphics	*gr = pl->im->gr;
	int		 x = 0, y = 0;
	u_int		 depth = 0;

	while (pl->parent != 0) {
		if (++depth > GRAPHICS_MAX_DEPTH)
			return (0);
		x += pl->h_off;
		y += pl->v_off;
		if ((pl = graphics_find_placement_serial(gr, pl->parent)) == NULL)
			return (0);
	}
	if (pl->anchor == 0 || !graphics_find_anchor(gd, pl->anchor, &pl->hint))
		return (0);
	*py = (int)pl->hint + y;
	*px = (int)pl->x + x;
	return (1);
}

/* Get placement row and column on screen, or 0 if it is gone. */
static int
graphics_placement_row(struct screen *s, struct graphics_placement *pl,
    int *row, int *col)
{
	struct grid	*gd = s->grid;
	int		 py;

	if (!graphics_placement_position(gd, pl, &py, col))
		return (0);
	*row = py - (int)gd->hsize;
	return (1);
}

/* Free a command. */
static void
graphics_cmd_free(struct graphics_cmd *cmd)
{
	free(cmd->keys);
	free(cmd->data);
	free(cmd);
}

/* Map a key to a bit. */
static uint64_t
graphics_key_bit(char key)
{
	if (key >= 'a' && key <= 'z')
		return (1ULL << (key - 'a'));
	return (1ULL << (26 + key - 'A'));
}

/* Is a key set? */
static int
graphics_has(struct graphics_cmd *cmd, char key)
{
	return ((cmd->set & graphics_key_bit(key)) != 0);
}

/* Parse a command. Returns NULL on error. */
static struct graphics_cmd *
graphics_parse(const char *buf, size_t len, struct graphics_cmd *bad)
{
	struct graphics_cmd	*cmd;
	const char		*end = buf + len, *next, *payload, *errstr;
	char			 key, value[32], *old;
	size_t			 vlen, i;
	long long		 n;

	memset(bad, 0, sizeof *bad);
	cmd = xcalloc(1, sizeof *cmd);

	payload = memchr(buf, ';', len);
	if (payload == NULL)
		payload = end;

	while (buf < payload) {
		next = memchr(buf, ',', payload - buf);
		if (next == NULL)
			next = payload;
		if (next - buf < 3 || buf[1] != '=' || !isalpha((u_char)*buf))
			goto skip;
		key = buf[0];
		vlen = next - buf - 2;
		if (vlen >= sizeof value)
			goto fail;
		memcpy(value, buf + 2, vlen);
		value[vlen] = '\0';
		for (i = 0; i < vlen; i++) {
			if (!isalnum((u_char)value[i]) && value[i] != '-')
				goto fail;
		}

		cmd->set |= graphics_key_bit(key);
		switch (key) {
		case 'a':
		case 't':
		case 'o':
		case 'd':
			if (vlen != 1)
				goto fail;
			if (key == 'a')
				cmd->action = value[0];
			else if (key == 't')
				cmd->medium = value[0];
			else if (key == 'o')
				cmd->compression = value[0];
			else
				cmd->delete = value[0];
			goto keep;
		}

		n = strtonum(value, INT_MIN, UINT_MAX, &errstr);
		if (errstr != NULL)
			goto fail;
		switch (key) {
		case 'z':
			cmd->z = n;
			goto keep;
		case 'H':
			cmd->h_off = n;
			goto keep;
		case 'V':
			cmd->v_off = n;
			goto keep;
		}
		if (n < 0)
			goto fail;
		switch (key) {
		case 'q':
			cmd->quiet = n;
			break;
		case 'f':
			cmd->format = n;
			break;
		case 'i':
			cmd->id = bad->id = n;
			break;
		case 'I':
			cmd->number = bad->number = n;
			break;
		case 'p':
			cmd->placement = bad->placement = n;
			break;
		case 's':
			cmd->width = n;
			break;
		case 'v':
			cmd->height = n;
			break;
		case 'S':
			cmd->size = n;
			break;
		case 'O':
			cmd->offset = n;
			break;
		case 'm':
			cmd->more = n;
			break;
		case 'x':
			cmd->x = n;
			break;
		case 'y':
			cmd->y = n;
			break;
		case 'w':
			cmd->w = n;
			break;
		case 'h':
			cmd->h = n;
			break;
		case 'X':
			cmd->x_off = n;
			break;
		case 'Y':
			cmd->y_off = n;
			break;
		case 'c':
			cmd->cols = n;
			break;
		case 'r':
			cmd->rows = n;
			break;
		case 'C':
			cmd->cursor = n;
			break;
		case 'U':
			cmd->virtual = n;
			break;
		case 'P':
			cmd->parent_id = n;
			break;
		case 'Q':
			cmd->parent_placement = n;
			break;
		}

	keep:
		/* Keep the keys which are replayed with frame commands. */
		if (strchr("iIpqmtSO", key) == NULL) {
			if (cmd->keys == NULL)
				xasprintf(&cmd->keys, "%c=%s", key, value);
			else {
				old = cmd->keys;
				xasprintf(&cmd->keys, "%s,%c=%s", old, key,
				    value);
				free(old);
			}
		}
	skip:
		buf = next + 1;
	}

	bad->quiet = cmd->quiet;
	if (payload != end) {
		payload++;
		for (i = 0; i < (size_t)(end - payload); i++) {
			if (!isalnum((u_char)payload[i]) &&
			    strchr("+/=", payload[i]) == NULL)
				goto fail;
		}
		cmd->datalen = end - payload;
		cmd->data = xmalloc(cmd->datalen + 1);
		memcpy(cmd->data, payload, cmd->datalen);
		cmd->data[cmd->datalen] = '\0';
	}
	return (cmd);

fail:
	bad->quiet = cmd->quiet;
	graphics_cmd_free(cmd);
	return (NULL);
}

/* Make a reply. */
static char * printflike(3, 4)
graphics_reply(struct graphics_cmd *cmd, u_int id, const char *fmt, ...)
{
	va_list	 ap;
	char	*msg, *reply, number[32], placement[32];
	int	 ok;

	if (id == 0 && cmd->number == 0)
		return (NULL);
	ok = (strcmp(fmt, "OK") == 0);
	if (cmd->quiet >= 2 || (ok && cmd->quiet == 1))
		return (NULL);

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	*number = *placement = '\0';
	if (cmd->number != 0)
		xsnprintf(number, sizeof number, ",I=%u", cmd->number);
	if (cmd->placement != 0)
		xsnprintf(placement, sizeof placement, ",p=%u", cmd->placement);
	xasprintf(&reply, "\033_Gi=%u%s%s;%s\033\\", id, number, placement,
	    msg);
	free(msg);
	return (reply);
}

/* Decode base64. */
static u_char *
graphics_decode(const char *data, size_t size, size_t *outlen)
{
	u_char	*out;
	int	 n;

	out = xmalloc(size * 3 / 4 + 4);
	n = b64_pton(data, out, size * 3 / 4 + 4);
	if (n < 0) {
		free(out);
		return (NULL);
	}
	*outlen = n;
	return (out);
}

/* Encode base64. */
static char *
graphics_encode(const u_char *in, size_t size, size_t *outlen)
{
	char	*out;
	size_t	 n = 4 * ((size + 2) / 3) + 1;
	int	 len;

	out = xmalloc(n);
	len = b64_ntop(in, size, out, n);
	if (len < 0) {
		free(out);
		return (NULL);
	}
	*outlen = len;
	return (out);
}

/*
 * Is this a temporary file which may be deleted? Like kitty, it must be in a
 * temporary directory and have tty-graphics-protocol in its name.
 */
static int
graphics_temporary_file(const char *path)
{
	const char	*dirs[] = { "/tmp", "/var/tmp", "/dev/shm", NULL, NULL };
	char		 resolved[PATH_MAX];
	const char	*tmpdir;
	size_t		 len;
	u_int		 i;

	if (strstr(path, "tty-graphics-protocol") == NULL)
		return (0);
	tmpdir = getenv("TMPDIR");
	if (tmpdir != NULL && *tmpdir != '\0')
		dirs[3] = tmpdir;
	for (i = 0; dirs[i] != NULL; i++) {
		if (realpath(dirs[i], resolved) == NULL)
			continue;
		len = strlen(resolved);
		while (len > 1 && resolved[len - 1] == '/')
			resolved[--len] = '\0';
		if (strncmp(path, resolved, len) == 0 && path[len] == '/')
			return (1);
	}
	return (0);
}

/* Read a file for a command. */
static const char *
graphics_read_file(struct graphics_cmd *cmd, const char *path, u_char **out,
    size_t *outlen)
{
	char		 resolved[PATH_MAX];
	struct stat	 sb;
	int		 fd;
	size_t		 size;
	ssize_t		 n;

	if (realpath(path, resolved) == NULL)
		return ("EBADF:Failed to open file");
	if (strncmp(resolved, "/proc/", 6) == 0 ||
	    strncmp(resolved, "/sys/", 5) == 0 ||
	    strncmp(resolved, "/dev/", 5) == 0)
		return ("EPERM:Not allowed to read file");
	if (cmd->medium == 't' && !graphics_temporary_file(resolved))
		return ("EPERM:Not a temporary file");

	/* Do not block on a FIFO or device before it can be checked. */
	fd = open(resolved, O_RDONLY|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);
	if (fd == -1)
		return ("EBADF:Failed to open file");
	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
		close(fd);
		return ("EBADF:Not a regular file");
	}
	if (cmd->offset > sb.st_size) {
		close(fd);
		return ("EINVAL:Offset is past the end of the file");
	}
	size = sb.st_size - cmd->offset;
	if (cmd->size != 0 && cmd->size < size)
		size = cmd->size;
	if (size > GRAPHICS_MAX_SIZE) {
		close(fd);
		return ("EFBIG:File is too big");
	}

	*out = xmalloc(size + 1);
	n = pread(fd, *out, size, cmd->offset);
	close(fd);
	if (n < 0 || (size_t)n != size) {
		free(*out);
		return ("EBADF:Failed to read file");
	}
	*outlen = size;

	if (cmd->medium == 't')
		unlink(resolved);
	return (NULL);
}

/* Read shared memory for a command. */
static const char *
graphics_read_shm(struct graphics_cmd *cmd, const char *name, u_char **out,
    size_t *outlen)
{
	struct stat	 sb;
	int		 fd;
	size_t		 size;
	void		*p;
	char		*path;

	if (*name != '/')
		xasprintf(&path, "/%s", name);
	else
		path = xstrdup(name);
	fd = shm_open(path, O_RDONLY, 0);
	if (fd != -1)
		shm_unlink(path);
	free(path);
	if (fd == -1)
		return ("EBADF:Failed to open shared memory");
	if (fstat(fd, &sb) != 0) {
		close(fd);
		return ("EBADF:Failed to open shared memory");
	}
	size = sb.st_size;
	if (cmd->size != 0 && (size_t)cmd->offset + cmd->size <= size)
		size = cmd->size;
	else if (cmd->offset < size)
		size -= cmd->offset;
	else {
		close(fd);
		return ("EINVAL:Offset is past the end of shared memory");
	}
	if (size == 0 || size > GRAPHICS_MAX_SIZE) {
		close(fd);
		return ("EFBIG:Shared memory is too big");
	}

	p = mmap(NULL, cmd->offset + size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return ("EBADF:Failed to map shared memory");
	*out = xmalloc(size);
	memcpy(*out, (u_char *)p + cmd->offset, size);
	munmap(p, cmd->offset + size);
	*outlen = size;
	return (NULL);
}

/* Load data from a file or shared memory for a command. */
static const char *
graphics_load(struct graphics_cmd *cmd)
{
	u_char		*name, *data;
	size_t		 namelen, size, len;
	const char	*error;

	if (cmd->toobig)
		return ("EFBIG:Image is too big");
	if (cmd->medium == '\0' || cmd->medium == 'd')
		return (NULL);
	if (cmd->medium != 'f' && cmd->medium != 't' && cmd->medium != 's')
		return ("EINVAL:Unknown transmission medium");
	if (cmd->data == NULL)
		return ("EINVAL:Missing file name");

	name = graphics_decode(cmd->data, cmd->datalen, &namelen);
	if (name == NULL || namelen == 0 || memchr(name, '\0', namelen)) {
		free(name);
		return ("EINVAL:Invalid file name");
	}
	name[namelen] = '\0';

	if (cmd->medium == 's')
		error = graphics_read_shm(cmd, name, &data, &size);
	else
		error = graphics_read_file(cmd, name, &data, &size);
	free(name);
	if (error != NULL)
		return (error);

	free(cmd->data);
	cmd->data = graphics_encode(data, size, &len);
	free(data);
	if (cmd->data == NULL)
		return ("EINVAL:Failed to encode data");
	cmd->datalen = len;
	cmd->medium = 'd';
	return (NULL);
}

/*
 * Get the size of a PNG image from its header, inflating the start of it if
 * compressed. Returns 0 if it cannot be found.
 */
static int
graphics_png_size(const char *data, size_t size, char compression,
    u_int *width, u_int *height)
{
	u_char		*decoded, header[24];
	char		*start;
	size_t		 len, n, got = 0;
	const u_char	 magic[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a,
			     '\n' };
#ifdef HAVE_ZLIB
	z_stream	 z;
#endif

	n = (compression == '\0') ? 32 : 65536;
	if (n > size)
		n = size;
	n &= ~(size_t)3;
	start = xmalloc(n + 1);
	memcpy(start, data, n);
	start[n] = '\0';
	decoded = graphics_decode(start, n, &len);
	free(start);
	if (decoded == NULL)
		return (0);

	if (compression == '\0') {
		got = (len < sizeof header) ? len : sizeof header;
		memcpy(header, decoded, got);
	}
#ifdef HAVE_ZLIB
	else {
		memset(&z, 0, sizeof z);
		if (inflateInit(&z) == Z_OK) {
			z.next_in = decoded;
			z.avail_in = len;
			z.next_out = header;
			z.avail_out = sizeof header;
			inflate(&z, Z_SYNC_FLUSH);
			got = sizeof header - z.avail_out;
			inflateEnd(&z);
		}
	}
#endif
	free(decoded);

	if (got < sizeof header || memcmp(header, magic, sizeof magic) != 0)
		return (0);
	*width = (u_int)header[16] << 24 | (u_int)header[17] << 16 |
	    (u_int)header[18] << 8 | header[19];
	*height = (u_int)header[20] << 24 | (u_int)header[21] << 16 |
	    (u_int)header[22] << 8 | header[23];
	return (*width != 0 && *height != 0);
}

/* Get a free image serial. */
static u_int
graphics_new_serial(void)
{
	struct graphics_image	find;

	for (;;) {
		if (++graphics_next_serial > GRAPHICS_SERIAL_MAX)
			graphics_next_serial = 1;
		find.serial = graphics_next_serial;
		if (RB_FIND(graphics_serials, &graphics_serials, &find) == NULL)
			return (graphics_next_serial);
	}
}

/* Get a free image id for an image with only a number. */
static u_int
graphics_new_id(struct graphics *gr)
{
	u_int	id;

	do
		id = arc4random() & 0x7fffffff;
	while (id == 0 || graphics_find_id(gr, id) != NULL);
	return (id);
}

/* Free images until under the limit. */
static void
graphics_evict(struct graphics_image *keep)
{
	struct graphics_image	*im, *im1;

	TAILQ_FOREACH_SAFE(im, &graphics_all, all_entry, im1) {
		if (graphics_total <= GRAPHICS_MAX_TOTAL)
			break;
		if (im != keep) {
			log_debug("%s: evicting image %u", __func__, im->serial);
			graphics_image_free(im);
		}
	}
}

/* Free an image and its placements. */
static void
graphics_image_free(struct graphics_image *im)
{
	struct graphics			*gr = im->gr;
	struct graphics_placement	*pl;
	struct graphics_op		*op, *op1;
	struct graphics_upload		*gu;
	struct client			*c;
	char				*keys;

	while ((pl = graphics_find_placement_any(gr, im)) != NULL)
		graphics_placement_free(pl);

	xasprintf(&keys, "a=d,d=I,i=%u", im->serial);
	TAILQ_FOREACH(c, &clients, entry) {
		gu = graphics_client_has(c, im->serial);
		if (gu == NULL)
			continue;
		graphics_write(&c->tty, keys, NULL, 0);
		RB_REMOVE(graphics_uploads, &c->graphics->uploads, gu);
		free(gu);
	}
	free(keys);

	TAILQ_FOREACH_SAFE(op, &im->ops, entry, op1) {
		TAILQ_REMOVE(&im->ops, op, entry);
		free(op->keys);
		free(op->data);
		free(op);
	}

	if (gr->last == im)
		gr->last = NULL;
	TAILQ_REMOVE(&gr->images, im, entry);
	TAILQ_REMOVE(&graphics_all, im, all_entry);
	RB_REMOVE(graphics_serials, &graphics_serials, im);
	graphics_total -= im->total;

	free(im->data);
	free(im);
}

/* Free a placement. */
static void
graphics_placement_free(struct graphics_placement *pl)
{
	struct graphics			*gr = pl->im->gr;
	struct graphics_placement	*loop;
	struct client			*c;
	u_int				 serial;

	if (pl->flags & GRAPHICS_VIRTUAL) {
		TAILQ_FOREACH(c, &clients, entry) {
			if (graphics_client_has(c, pl->im->serial) != NULL) {
				graphics_write_delete(&c->tty, pl->im->serial,
				    graphics_placement_part(pl, 0));
			}
		}
	} else
		graphics_generation++;
	TAILQ_REMOVE(&gr->placements, pl, entry);
	serial = pl->serial;
	free(pl);

	/* Children go with their parent. */
	do {
		TAILQ_FOREACH(loop, &gr->placements, entry) {
			if (loop->parent == serial)
				break;
		}
		if (loop != NULL)
			graphics_placement_free(loop);
	} while (loop != NULL);
}

/* Free all images in a store. */
void
graphics_free(struct graphics *gr)
{
	struct graphics_image	*im, *im1;
	u_int			 i;

	if (gr == NULL)
		return;
	for (i = 0; i < graphics_nheld; /* nothing */) {
		if (graphics_held[i].gr == gr)
			graphics_held[i] = graphics_held[--graphics_nheld];
		else
			i++;
	}
	TAILQ_FOREACH_SAFE(im, &gr->images, entry, im1)
		graphics_image_free(im);
	if (gr->loading != NULL)
		graphics_cmd_free(gr->loading);
	free(gr);
}

/* Reset a screen (RIS). */
void
graphics_reset(struct screen *s)
{
	graphics_free(s->graphics);
	s->graphics = NULL;
	graphics_free(s->saved_graphics);
	s->saved_graphics = NULL;
}

/*
 * Enter the alternate screen. Its images are kept while the main screen is
 * used (saved_graphics holds the other screen's store), as in kitty, so a
 * program which leaves it to run another can draw them again. Only the
 * placements are removed.
 */
void
graphics_alternate_on(struct screen *s)
{
	struct graphics			*gr = s->saved_graphics;
	struct graphics_placement	*pl;

	s->saved_graphics = s->graphics;
	s->graphics = gr;
	graphics_generation++;
	if (gr == NULL)
		return;
restart:
	TAILQ_FOREACH(pl, &gr->placements, entry) {
		if (~pl->flags & GRAPHICS_VIRTUAL) {
			graphics_placement_free(pl);
			goto restart;
		}
	}
}

/* Leave the alternate screen. */
void
graphics_alternate_off(struct screen *s)
{
	struct graphics	*gr = s->graphics;

	s->graphics = s->saved_graphics;
	s->saved_graphics = gr;
	graphics_generation++;
}

/* Save placement positions in a store as offsets into unwrapped lines. */
static void
graphics_reflow_save(struct graphics *gr, struct grid *gd, u_int *ly,
    u_int *lx)
{
	struct graphics_placement	*pl;

	if (gr == NULL)
		return;
	TAILQ_FOREACH(pl, &gr->placements, entry) {
		pl->flags &= ~GRAPHICS_WRAPPED;
		if ((pl->flags & GRAPHICS_VIRTUAL) || pl->parent != 0)
			continue;
		if (!graphics_find_anchor(gd, pl->anchor, &pl->hint))
			continue;
		pl->wrap_x = lx[pl->hint] + pl->x;
		pl->wrap_y = ly[pl->hint];
		pl->flags |= GRAPHICS_WRAPPED;
	}
}

/* Before a screen is reflowed, save where its placements are. */
void
graphics_reflow_start(struct screen *s)
{
	struct grid	*gd = s->grid;
	u_int		 n = gd->hsize + gd->sy, yy, ax = 0, ay = 0, *ly, *lx;

	if (s->graphics == NULL && s->saved_graphics == NULL)
		return;

	ly = xreallocarray(NULL, n, sizeof *ly);
	lx = xreallocarray(NULL, n, sizeof *lx);
	for (yy = 0; yy < n; yy++) {
		ly[yy] = ay;
		lx[yy] = ax;
		if (gd->linedata[yy].flags & GRID_LINE_WRAPPED)
			ax += gd->linedata[yy].cellused;
		else {
			ax = 0;
			ay++;
		}
	}
	graphics_reflow_save(s->graphics, gd, ly, lx);
	graphics_reflow_save(s->saved_graphics, gd, ly, lx);
	free(ly);
	free(lx);
}

/* Move placements in a store to their new lines. */
static void
graphics_reflow_restore(struct graphics *gr, struct grid *gd, u_int *starts,
    u_int nstarts)
{
	struct graphics_placement	*pl;
	struct grid_line		*gl;
	u_int				 n = gd->hsize + gd->sy, yy, wx;

	if (gr == NULL)
		return;
	TAILQ_FOREACH(pl, &gr->placements, entry) {
		if (~pl->flags & GRAPHICS_WRAPPED)
			continue;
		pl->flags &= ~GRAPHICS_WRAPPED;
		if (pl->wrap_y >= nstarts)
			continue;
		yy = starts[pl->wrap_y];
		wx = pl->wrap_x;
		for (; yy < n - 1; yy++) {
			gl = &gd->linedata[yy];
			if ((~gl->flags & GRID_LINE_WRAPPED) ||
			    wx < gl->cellused)
				break;
			wx -= gl->cellused;
		}
		pl->x = wx;
		pl->anchor = graphics_anchor(gd, yy);
		pl->hint = yy;
	}
}

/* After a screen is reflowed, put placements back on the same text. */
void
graphics_reflow_end(struct screen *s)
{
	struct grid	*gd = s->grid;
	u_int		 n = gd->hsize + gd->sy, yy, nstarts = 0, *starts;
	int		 start = 1;

	if (s->graphics == NULL && s->saved_graphics == NULL)
		return;

	starts = xreallocarray(NULL, n, sizeof *starts);
	for (yy = 0; yy < n; yy++) {
		if (start)
			starts[nstarts++] = yy;
		start = (~gd->linedata[yy].flags & GRID_LINE_WRAPPED);
	}
	graphics_reflow_restore(s->graphics, gd, starts, nstarts);
	graphics_reflow_restore(s->saved_graphics, gd, starts, nstarts);
	free(starts);
	graphics_generation++;
}

/* Remove placements on the screen or in the history (ED 2 and 3). */
void
graphics_clear(struct screen *s, struct window_pane *wp, int history)
{
	struct graphics			*gr = s->graphics;
	struct graphics_placement	*pl;
	int				 row, col, match;

	if (gr == NULL)
		return;
restart:
	TAILQ_FOREACH(pl, &gr->placements, entry) {
		if (pl->flags & GRAPHICS_VIRTUAL)
			continue;
		if (!graphics_placement_row(s, pl, &row, &col))
			match = 1;
		else if (history)
			match = (row < 0);
		else {
			/* Anything on the screen, even if it starts above. */
			match = graphics_placement_hits(pl, wp, row, col, -1, 0,
			    screen_size_y(s));
		}
		if (match) {
			graphics_placement_free(pl);
			goto restart;
		}
	}
}

/* Transmit an image. */
static char *
graphics_transmit(struct graphics *gr, struct graphics_cmd *cmd,
    struct graphics_image **imp)
{
	struct graphics_image	*im;
	const char		*error;
	u_int			 id = cmd->id;

	*imp = NULL;
	if (cmd->id != 0 && cmd->number != 0)
		return (graphics_reply(cmd, id, "EINVAL:Both i and I set"));
	if ((error = graphics_load(cmd)) != NULL)
		return (graphics_reply(cmd, id, "%s", error));
	if (cmd->data == NULL || cmd->datalen == 0)
		return (graphics_reply(cmd, id, "ENODATA:No image data"));
	if (cmd->format == 0)
		cmd->format = 32;
	if (cmd->format != 24 && cmd->format != 32 && cmd->format != 100)
		return (graphics_reply(cmd, id, "EINVAL:Unknown format"));
	if (cmd->format != 100 && (cmd->width == 0 || cmd->height == 0))
		return (graphics_reply(cmd, id, "EINVAL:Missing image size"));
	if (cmd->compression != '\0' && cmd->compression != 'z')
		return (graphics_reply(cmd, id, "EINVAL:Unknown compression"));
	/* The size is needed to crop placements. */
	if (cmd->format == 100 && !graphics_png_size(cmd->data, cmd->datalen,
	    cmd->compression, &cmd->width, &cmd->height))
		return (graphics_reply(cmd, id, "EBADPNG:Failed to read size"));
	if (cmd->width > GRAPHICS_MAX_PIXELS ||
	    cmd->height > GRAPHICS_MAX_PIXELS)
		return (graphics_reply(cmd, id, "EINVAL:Image too big"));
	if (cmd->action == 'q')
		return (graphics_reply(cmd, id, "OK"));

	if (id != 0) {
		if ((im = graphics_find_id(gr, id)) != NULL)
			graphics_image_free(im);
	} else if (cmd->number != 0)
		id = graphics_new_id(gr);

	im = xcalloc(1, sizeof *im);
	im->gr = gr;
	im->id = id;
	im->number = cmd->number;
	im->serial = graphics_new_serial();
	im->format = cmd->format;
	im->compression = cmd->compression;
	im->width = cmd->width;
	im->height = cmd->height;
	im->data = cmd->data;
	im->size = im->total = cmd->datalen;
	cmd->data = NULL;
	TAILQ_INIT(&im->ops);

	TAILQ_INSERT_TAIL(&gr->images, im, entry);
	TAILQ_INSERT_TAIL(&graphics_all, im, all_entry);
	RB_INSERT(graphics_serials, &graphics_serials, im);
	graphics_total += im->total;
	log_debug("%s: image %u (serial %u) %ux%u, %zu bytes", __func__,
	    im->id, im->serial, im->width, im->height, im->size);

	graphics_evict(im);
	*imp = im;
	return (graphics_reply(cmd, id, "OK"));
}

/*
 * Work out the size of a placement in cells for a cell size. Without c or r
 * this is the size of the image; with only one, the other keeps the aspect
 * ratio and the image is stretched over both, as in kitty.
 */
static void
graphics_placement_size(struct graphics_placement *pl, u_int cw, u_int ch,
    u_int *cols, u_int *rows)
{
	uint64_t	t, d;

	/* Round up only at the end so a partly covered cell is counted. */
	*cols = pl->req_cols;
	*rows = pl->req_rows;
	if (*cols != 0 && *rows == 0 && pl->src_w != 0) {
		t = ((uint64_t)*cols * cw + pl->x_off) * pl->src_h;
		d = (uint64_t)pl->src_w * ch;
		*rows = (t + d - 1) / d;
	} else if (*rows != 0 && *cols == 0 && pl->src_h != 0) {
		t = ((uint64_t)*rows * ch + pl->y_off) * pl->src_w;
		d = (uint64_t)pl->src_h * cw;
		*cols = (t + d - 1) / d;
	} else if (*cols == 0 && *rows == 0) {
		*cols = (pl->src_w + pl->x_off + cw - 1) / cw;
		*rows = (pl->src_h + pl->y_off + ch - 1) / ch;
	}
	if (*cols == 0)
		*cols = 1;
	else if (*cols > GRAPHICS_MAX_CELLS)
		*cols = GRAPHICS_MAX_CELLS;
	if (*rows == 0)
		*rows = 1;
	else if (*rows > GRAPHICS_MAX_CELLS)
		*rows = GRAPHICS_MAX_CELLS;
}

/* Does a rectangle of cells touch a column and range of rows? */
static int
graphics_rect_hits(int row, int col, u_int cols, u_int rows, int x, int y0,
    int y1)
{
	if (y0 != -1 && (y1 <= row || y0 >= row + (int)rows))
		return (0);
	return (x == -1 || (x >= col && x < col + (int)cols));
}

/*
 * Does a placement at row and col touch column x (-1 for any) and rows y0 to
 * y1 - 1 (y0 -1 for any)? Its size is worked out for each client showing its
 * pane, or the size when it was put if none.
 */
static int
graphics_placement_hits(struct graphics_placement *pl,
    struct window_pane *wp, int row, int col, int x, int y0, int y1)
{
	struct client	*c;
	u_int		 cw, ch, cols, rows;
	int		 found = 0;

	TAILQ_FOREACH(c, &clients, entry) {
		if (wp == NULL || !window_pane_is_visible(wp))
			break;
		if (c->session == NULL || c->session->curw == NULL ||
		    c->session->curw->window != wp->window ||
		    !graphics_client_supported(c))
			continue;
		cw = c->tty.xpixel == 0 ? DEFAULT_XPIXEL : c->tty.xpixel;
		ch = c->tty.ypixel == 0 ? DEFAULT_YPIXEL : c->tty.ypixel;
		graphics_placement_size(pl, cw, ch, &cols, &rows);
		found = 1;
		if (graphics_rect_hits(row, col, cols, rows, x, y0, y1))
			return (1);
	}
	if (found)
		return (0);
	return (graphics_rect_hits(row, col, pl->cols, pl->rows, x, y0, y1));
}

/* Forget a part shown on a client without deleting it. */
static void
graphics_forget_part(struct graphics_client *gcl, u_int serial, u_int id)
{
	struct graphics_part	*gp;

	TAILQ_FOREACH(gp, &gcl->shown, entry) {
		if (gp->serial == serial && gp->id == id) {
			TAILQ_REMOVE(&gcl->shown, gp, entry);
			free(gp);
			return;
		}
	}
}

/* Place an image. */
static char *
graphics_put(struct screen_write_ctx *ctx, struct graphics *gr,
    struct graphics_image *im, struct graphics_cmd *cmd)
{
	struct screen			*s = ctx->s;
	struct grid			*gd = s->grid;
	struct window			*w = ctx->wp->window;
	struct graphics_placement	*pl, *parent = NULL, *loop;
	struct graphics_image		*pim;
	struct client			*c;
	u_int				 cw, ch, py, i, depth;
	int				 new = 0, row = 0, col = 0;

	if (cmd->virtual && cmd->parent_id != 0)
		return (graphics_reply(cmd, im->id, "EINVAL:Virtual "
		    "placements can't have a parent"));
	if (cmd->cols > GRAPHICS_MAX_CELLS || cmd->rows > GRAPHICS_MAX_CELLS ||
	    cmd->h_off > GRAPHICS_MAX_CELLS ||
	    cmd->h_off < -GRAPHICS_MAX_CELLS ||
	    cmd->v_off > GRAPHICS_MAX_CELLS ||
	    cmd->v_off < -GRAPHICS_MAX_CELLS)
		return (graphics_reply(cmd, im->id, "EINVAL:Too many cells"));
	if (cmd->x > GRAPHICS_MAX_PIXELS || cmd->y > GRAPHICS_MAX_PIXELS ||
	    cmd->w > GRAPHICS_MAX_PIXELS || cmd->h > GRAPHICS_MAX_PIXELS ||
	    cmd->x_off > GRAPHICS_MAX_PIXELS ||
	    cmd->y_off > GRAPHICS_MAX_PIXELS)
		return (graphics_reply(cmd, im->id, "EINVAL:Too many pixels"));
	if (cmd->parent_id != 0) {
		pim = graphics_find_id(gr, cmd->parent_id);
		if (pim != NULL) {
			parent = graphics_find_placement(gr, pim,
			    cmd->parent_placement);
		}
		if (parent == NULL)
			return (graphics_reply(cmd, im->id,
			    "ENOPARENT:Parent placement not found"));
		if (parent->flags & GRAPHICS_VIRTUAL)
			return (graphics_reply(cmd, im->id,
			    "ENOTSUPPORTED:Parent is a virtual placement"));
		if (parent->im == im && parent->id == cmd->placement)
			return (graphics_reply(cmd, im->id,
			    "EINVAL:Placement is its own parent"));
		if (!graphics_placement_row(s, parent, &row, &col))
			return (graphics_reply(cmd, im->id,
			    "ENOPARENT:Parent placement not found"));
	}

	pl = NULL;
	if (cmd->placement != 0)
		pl = graphics_find_placement(gr, im, cmd->placement);
	for (loop = parent, depth = 0; loop != NULL; depth++) {
		if (loop == pl)
			return (graphics_reply(cmd, im->id,
			    "ECYCLE:Placement would be its own parent"));
		if (depth == GRAPHICS_MAX_DEPTH)
			return (graphics_reply(cmd, im->id,
			    "ETOODEEP:Too many parents"));
		if (loop->parent == 0)
			break;
		loop = graphics_find_placement_serial(gr, loop->parent);
	}
	if (pl == NULL) {
		pl = xcalloc(1, sizeof *pl);
		pl->im = im;
		pl->id = cmd->placement;
		if (++graphics_next_placement > GRAPHICS_PLACEMENT_MAX)
			graphics_next_placement = 1;
		pl->serial = graphics_next_placement;
		new = 1;
	} else if ((pl->flags & GRAPHICS_VIRTUAL) && !cmd->virtual) {
		TAILQ_FOREACH(c, &clients, entry) {
			if (graphics_client_has(c, im->serial) != NULL) {
				graphics_write_delete(&c->tty, im->serial,
				    graphics_placement_part(pl, 0));
			}
		}
	} else if ((~pl->flags & GRAPHICS_VIRTUAL) && cmd->virtual) {
		/*
		 * The virtual placement replaces the first part on the
		 * terminal, so it must not be deleted when the parts change.
		 */
		TAILQ_FOREACH(c, &clients, entry) {
			if (c->graphics != NULL) {
				graphics_forget_part(c->graphics, im->serial,
				    graphics_placement_part(pl, 0));
			}
		}
	}

	cw = w->xpixel == 0 ? DEFAULT_XPIXEL : w->xpixel;
	ch = w->ypixel == 0 ? DEFAULT_YPIXEL : w->ypixel;

	/* Everything is set again from the command when put again. */
	pl->flags = cmd->virtual ? GRAPHICS_VIRTUAL : 0;
	pl->parent = pl->h_off = pl->v_off = 0;
	pl->anchor = pl->x = 0;
	pl->src_x = cmd->x;
	pl->src_y = cmd->y;
	if (cmd->w != 0)
		pl->src_w = cmd->w;
	else
		pl->src_w = im->width > cmd->x ? im->width - cmd->x : 0;
	if (cmd->h != 0)
		pl->src_h = cmd->h;
	else
		pl->src_h = im->height > cmd->y ? im->height - cmd->y : 0;
	if (pl->src_w > GRAPHICS_MAX_PIXELS)
		pl->src_w = GRAPHICS_MAX_PIXELS;
	if (pl->src_h > GRAPHICS_MAX_PIXELS)
		pl->src_h = GRAPHICS_MAX_PIXELS;
	pl->x_off = cmd->x_off;
	pl->y_off = cmd->y_off;
	pl->z = cmd->z;
	pl->req_cols = cmd->cols;
	pl->req_rows = cmd->rows;
	if (cmd->cols != 0 || cmd->rows != 0)
		pl->flags |= (GRAPHICS_COLS|GRAPHICS_ROWS);
	graphics_placement_size(pl, cw, ch, &pl->cols, &pl->rows);
	if (new)
		TAILQ_INSERT_TAIL(&gr->placements, pl, entry);
	graphics_image_touch(im);

	if (pl->flags & GRAPHICS_VIRTUAL) {
		TAILQ_FOREACH(c, &clients, entry) {
			if (graphics_client_has(c, im->serial) != NULL)
				graphics_write_virtual(&c->tty, pl);
		}
		return (graphics_reply(cmd, im->id, "OK"));
	}

	if (parent != NULL) {
		/* Relative placements follow their parent when drawn. */
		pl->parent = parent->serial;
		pl->h_off = cmd->h_off;
		pl->v_off = cmd->v_off;
		py = gd->hsize + row + cmd->v_off;
	} else {
		py = gd->hsize + s->cy;
		pl->x = s->cx;
		pl->anchor = graphics_anchor(gd, py);
		pl->hint = py;
	}
	graphics_generation++;
	log_debug("%s: placement %u of image %u at %u,%u (%ux%u)", __func__,
	    pl->id, im->id, pl->x, py, pl->cols, pl->rows);

	/* Move the cursor to after the image. */
	if (parent == NULL && cmd->cursor == 0) {
		for (i = 1; i < pl->rows; i++)
			screen_write_linefeed(ctx, 0, 8);
		if (pl->x + pl->cols >= screen_size_x(s))
			screen_write_cursormove(ctx, screen_size_x(s) - 1, -1, 0);
		else
			screen_write_cursormove(ctx, pl->x + pl->cols, -1, 0);
	}
	return (graphics_reply(cmd, im->id, "OK"));
}

/* Does a placement cover a cell? -1 is any row or column. */
static int
graphics_covers(struct screen *s, struct window_pane *wp,
    struct graphics_placement *pl, int x, int y, int *gone)
{
	int	row, col;

	*gone = 0;
	if (!graphics_placement_row(s, pl, &row, &col)) {
		*gone = 1;
		return (0);
	}
	return (graphics_placement_hits(pl, wp, row, col, x, y, y + 1));
}

/* Delete placements and images. */
static char *
graphics_delete(struct screen_write_ctx *ctx, struct graphics *gr,
    struct graphics_cmd *cmd)
{
	struct screen			*s = ctx->s;
	struct window_pane		*wp = ctx->wp;
	struct graphics_placement	*pl;
	struct graphics_image		*im, *im1, *target = NULL;
	char				 d = cmd->delete == '\0' ? 'a' : cmd->delete;
	int				 data = isupper((u_char)d), match, gone;
	int				 x, y, virtual, row, col;

	x = (int)MIN(cmd->x, INT_MAX) - (cmd->x != 0);
	y = (int)MIN(cmd->y, INT_MAX) - (cmd->y != 0);

	switch (tolower((u_char)d)) {
	case 'i':
	case 'n':
		target = graphics_find(gr, cmd);
		if (target == NULL)
			return (NULL);
		break;
	case 'a':
	case 'c':
	case 'p':
	case 'q':
	case 'x':
	case 'y':
	case 'z':
	case 'r':
		break;
	default:
		return (graphics_reply(cmd, cmd->id, "EINVAL:Unknown delete"));
	}

restart:
	TAILQ_FOREACH(pl, &gr->placements, entry) {
		virtual = (pl->flags & GRAPHICS_VIRTUAL);
		gone = 0;
		switch (tolower((u_char)d)) {
		case 'a':
			/* Only placements on the visible screen. */
			match = 0;
			if (!virtual &&
			    graphics_placement_row(s, pl, &row, &col)) {
				match = graphics_placement_hits(pl, wp, row,
				    col, -1, 0, screen_size_y(s));
			}
			break;
		case 'i':
		case 'n':
			match = (pl->im == target &&
			    (cmd->placement == 0 || pl->id == cmd->placement));
			break;
		case 'r':
			match = (pl->im->id >= cmd->x && pl->im->id <= cmd->y);
			break;
		case 'c':
			match = !virtual &&
			    graphics_covers(s, wp, pl, s->cx, s->cy, &gone);
			break;
		case 'p':
			match = !virtual &&
			    graphics_covers(s, wp, pl, x, y, &gone);
			break;
		case 'q':
			match = !virtual && pl->z == cmd->z &&
			    graphics_covers(s, wp, pl, x, y, &gone);
			break;
		case 'x':
			match = !virtual &&
			    graphics_covers(s, wp, pl, x, -1, &gone);
			break;
		case 'y':
			match = !virtual &&
			    graphics_covers(s, wp, pl, -1, y, &gone);
			break;
		case 'z':
			match = !virtual && pl->z == cmd->z;
			break;
		default:
			match = 0;
			break;
		}
		if (match || gone) {
			im = pl->im;
			graphics_placement_free(pl);
			if (match && data && target == NULL &&
			    graphics_find_placement_any(gr, im) == NULL)
				graphics_image_free(im);
			goto restart;
		}
	}

	if (data) {
		switch (tolower((u_char)d)) {
		case 'i':
		case 'n':
			if (cmd->placement == 0 ||
			    graphics_find_placement_any(gr, target) == NULL)
				graphics_image_free(target);
			break;
		case 'r':
			TAILQ_FOREACH_SAFE(im, &gr->images, entry, im1) {
				if (im->id >= cmd->x && im->id <= cmd->y)
					graphics_image_free(im);
			}
			break;
		}
	}
	return (NULL);
}

/* Add a frame or animation command to an image. */
static char *
graphics_frame(struct graphics *gr, struct graphics_cmd *cmd)
{
	struct graphics_image	*im;
	struct graphics_op	*op, *op1;
	const char		*error;
	char			*keys;
	size_t			 cost;

	im = graphics_find(gr, cmd);
	if (im == NULL) {
		return (graphics_reply(cmd, cmd->id, "ENOENT:No image with id "
		    "%u", cmd->id));
	}
	if ((error = graphics_load(cmd)) != NULL)
		return (graphics_reply(cmd, im->id, "%s", error));
	if (cmd->keys == NULL)
		cmd->keys = xstrdup("");

	/* A repeated animation control replaces the earlier one. */
	if (cmd->action == 'a' && cmd->datalen == 0) {
		TAILQ_FOREACH_SAFE(op, &im->ops, entry, op1) {
			if (op->size != 0 || strcmp(op->keys, cmd->keys) != 0)
				continue;
			TAILQ_REMOVE(&im->ops, op, entry);
			im->nops--;
			im->total -= op->cost;
			graphics_total -= op->cost;
			free(op->keys);
			free(op->data);
			free(op);
		}
	}

	cost = sizeof *op + strlen(cmd->keys) + cmd->datalen;
	if (im->nops >= GRAPHICS_MAX_OPS ||
	    im->total + cost > GRAPHICS_MAX_SIZE)
		return (graphics_reply(cmd, im->id, "ENOSPC:Too many frames"));

	op = xcalloc(1, sizeof *op);
	op->keys = cmd->keys;
	cmd->keys = NULL;
	op->data = cmd->data;
	op->size = cmd->datalen;
	op->cost = cost;
	cmd->data = NULL;
	TAILQ_INSERT_TAIL(&im->ops, op, entry);
	im->nops++;
	im->total += cost;
	graphics_total += cost;
	graphics_image_touch(im);

	xasprintf(&keys, "%s%si=%u%s", op->keys, *op->keys == '\0' ? "" : ",",
	    im->serial, op->size != 0 ? ",t=d" : "");
	graphics_write_all(im, keys, op->data, op->size);
	free(keys);

	graphics_evict(im);
	return (graphics_reply(cmd, im->id, "OK"));
}

/* Run a complete command. */
static char *
graphics_run(struct screen_write_ctx *ctx, struct graphics *gr,
    struct graphics_cmd *cmd)
{
	struct graphics_image	*im;
	char			*reply;

	switch (cmd->action) {
	case '\0':
	case 't':
	case 'T':
	case 'q':
		reply = graphics_transmit(gr, cmd, &im);
		if (cmd->action != 'T' || im == NULL)
			return (reply);
		free(reply);
		return (graphics_put(ctx, gr, im, cmd));
	case 'p':
		im = graphics_find(gr, cmd);
		if (im == NULL) {
			return (graphics_reply(cmd, cmd->id, "ENOENT:No image "
			    "with id %u", cmd->id));
		}
		return (graphics_put(ctx, gr, im, cmd));
	case 'd':
		if (tolower((u_char)cmd->delete) == 'f')
			return (graphics_frame(gr, cmd));
		return (graphics_delete(ctx, gr, cmd));
	case 'f':
	case 'a':
	case 'c':
		return (graphics_frame(gr, cmd));
	}
	return (graphics_reply(cmd, cmd->id, "EINVAL:Unknown action"));
}

/*
 * Handle a graphics command from a pane (the APC string without the
 * introducer and terminator). Returns the reply if any.
 */
char *
graphics_command(struct screen_write_ctx *ctx, const char *buf, size_t len)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = ctx->s;
	struct graphics		*gr;
	struct graphics_cmd	*cmd, *loading, bad;
	char			*reply;
	size_t			 size;

	if (wp == NULL || len == 0 || *buf != 'G')
		return (NULL);
	buf++;
	len--;

	cmd = graphics_parse(buf, len, &bad);
	if (cmd == NULL)
		return (graphics_reply(&bad, bad.id, "EINVAL:Invalid command"));
	if (cmd->action == 'q' && !graphics_pane_supported(wp)) {
		graphics_cmd_free(cmd);
		return (NULL);
	}

	gr = s->graphics;
	if (gr == NULL) {
		gr = s->graphics = xcalloc(1, sizeof *gr);
		TAILQ_INIT(&gr->images);
		TAILQ_INIT(&gr->placements);
	}

	/* Another command cancels an unfinished upload. */
	if (gr->loading != NULL && graphics_has(cmd, 'a') &&
	    strchr("tTfq", cmd->action) == NULL) {
		graphics_cmd_free(gr->loading);
		gr->loading = NULL;
	}

	/* Add chunks to the command being loaded. */
	if ((loading = gr->loading) != NULL) {
		size = loading->datalen + cmd->datalen;
		if (size > GRAPHICS_MAX_SIZE * 4 / 3 || loading->toobig) {
			loading->toobig = 1;
			free(loading->data);
			loading->data = NULL;
			loading->datalen = 0;
		} else if (cmd->datalen != 0) {
			loading->data = xrealloc(loading->data, size + 1);
			memcpy(loading->data + loading->datalen, cmd->data,
			    cmd->datalen);
			loading->data[size] = '\0';
			loading->datalen = size;
		}
		if (graphics_has(cmd, 'q'))
			loading->quiet = cmd->quiet;
		if (cmd->more) {
			graphics_cmd_free(cmd);
			return (NULL);
		}
		graphics_cmd_free(cmd);
		cmd = loading;
		gr->loading = NULL;
	} else if (cmd->more) {
		gr->loading = cmd;
		return (NULL);
	}

	reply = graphics_run(ctx, gr, cmd);
	graphics_cmd_free(cmd);
	return (reply);
}

/* Get the id in a placeholder colour. */
static u_int
graphics_colour_id(int colour)
{
	u_char	r, g, b;

	if (colour & COLOUR_FLAG_RGB) {
		colour_split_rgb(colour, &r, &g, &b);
		return ((u_int)r << 16 | (u_int)g << 8 | b);
	}
	if (colour & COLOUR_FLAG_256)
		return (colour & 0xff);
	if (colour >= 0 && colour <= 7)
		return (colour);
	if (colour >= 90 && colour <= 97)
		return (colour - 90 + 8);
	return (0);
}

/* Get the index of a row or column diacritic. */
static int
graphics_diacritic(wchar_t wc)
{
	u_int	lo = 0, hi = GRAPHICS_NDIACRITICS, mid;

	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (graphics_diacritics[mid] == (u_int)wc)
			return (mid);
		if (graphics_diacritics[mid] < (u_int)wc)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (-1);
}

/*
 * Parse a placeholder cell. Returns 0 if not a placeholder. Missing
 * diacritics are -1.
 */
static int
graphics_parse_placeholder(const struct grid_cell *gc, u_int *id, u_int *pid,
    int *row, int *col, int *msb)
{
	const u_char	*p = gc->data.data, *end = p + gc->data.size;
	struct utf8_data ud;
	wchar_t		 wc;
	int		 values[3] = { -1, -1, -1 };
	enum utf8_state	 more;
	u_int		 i, n = 0;

	if (gc->data.size < 4 || p[0] != 0xf4 || p[1] != 0x8e ||
	    p[2] != 0xbb || p[3] != 0xae)
		return (0);
	for (p += 4; p < end && n < 3; p += ud.size) {
		if (utf8_open(&ud, *p) != UTF8_MORE)
			break;
		more = UTF8_MORE;
		for (i = 1; i < ud.size && p + i < end; i++)
			more = utf8_append(&ud, p[i]);
		if (more != UTF8_DONE || utf8_towc(&ud, &wc) != UTF8_DONE)
			break;
		values[n++] = graphics_diacritic(wc);
	}
	*row = values[0];
	*col = values[1];
	*msb = values[2];
	*id = graphics_colour_id(gc->fg);
	*pid = graphics_colour_id(gc->us);
	return (1);
}

/* Write a character as UTF-8. */
static size_t
graphics_put_utf8(u_char *cp, u_int wc)
{
	if (wc < 0x800) {
		cp[0] = 0xc0 | (wc >> 6);
		cp[1] = 0x80 | (wc & 0x3f);
		return (2);
	}
	if (wc < 0x10000) {
		cp[0] = 0xe0 | (wc >> 12);
		cp[1] = 0x80 | ((wc >> 6) & 0x3f);
		cp[2] = 0x80 | (wc & 0x3f);
		return (3);
	}
	cp[0] = 0xf0 | (wc >> 18);
	cp[1] = 0x80 | ((wc >> 12) & 0x3f);
	cp[2] = 0x80 | ((wc >> 6) & 0x3f);
	cp[3] = 0x80 | (wc & 0x3f);
	return (4);
}

/*
 * Fill in missing row, column and high byte of a placeholder from the cell to
 * its left, as kitty does. Each prev is the resolved previous cell.
 */
static void
graphics_infer_placeholder(int have, int prow, int pcol, int pmsb, int pknown,
    int *row, int *col, int *msb, int *known)
{
	*known = 1;
	if (have) {
		if (*row < 0) {
			*row = prow;
			*col = pcol + 1;
			*msb = pmsb;
			*known = pknown;
		} else if (*col < 0) {
			if (*row == prow) {
				*col = pcol + 1;
				if (*msb < 0)
					*msb = pmsb;
				*known = pknown;
			}
		} else if (*msb < 0) {
			if (*row == prow && *col == pcol + 1)
				*msb = pmsb;
		}
	}
	if (*row < 0 || *col < 0)
		*known = 0;
	if (*row < 0)
		*row = 0;
	if (*col < 0)
		*col = 0;
	if (*msb < 0)
		*msb = 0;
}

/*
 * Resolve the row, column and high byte of a placeholder. Returns 0 if the
 * row or column are not known (diacritics may still be to come).
 */
static int
graphics_resolve_placeholder(struct grid *gd, u_int px, u_int py, u_int id,
    u_int pid, int *row, int *col, int *msb)
{
	static struct {
		struct grid	*gd;
		u_int		 px, py, id, pid;
		int		 row, col, msb, known;
	}			 last;
	struct grid_cell	 gc;
	u_int			 x = px, lid, lpid;
	int			 lrow, lcol, lmsb, prow = 0, pcol = 0, pmsb = 0;
	int			 pknown = 0, have = 0, known;

	if (*row >= 0 && *col >= 0 && *msb >= 0) {
		known = 1;
		goto out;
	}
	if (last.gd == gd && last.py == py && last.px + 1 == px &&
	    last.id == id && last.pid == pid) {
		prow = last.row;
		pcol = last.col;
		pmsb = last.msb;
		pknown = 1; /* a later cell has started, so it is complete */
		have = 1;
		goto infer;
	}

	/* Find where the run of placeholders starts, then resolve forwards. */
	while (x > 0) {
		grid_get_cell(gd, x - 1, py, &gc);
		if (!graphics_parse_placeholder(&gc, &lid, &lpid, &lrow, &lcol,
		    &lmsb) || lid != id || lpid != pid)
			break;
		x--;
		if (lrow >= 0 && lcol >= 0 && lmsb >= 0)
			break;
	}
	for (; x < px; x++) {
		grid_get_cell(gd, x, py, &gc);
		graphics_parse_placeholder(&gc, &lid, &lpid, &lrow, &lcol,
		    &lmsb);
		graphics_infer_placeholder(have, prow, pcol, pmsb, pknown,
		    &lrow, &lcol, &lmsb, &pknown);
		pknown = 1;
		prow = lrow;
		pcol = lcol;
		pmsb = lmsb;
		have = 1;
	}

infer:
	graphics_infer_placeholder(have, prow, pcol, pmsb, pknown, row, col,
	    msb, &known);

out:
	last.gd = gd;
	last.px = px;
	last.py = py;
	last.id = id;
	last.pid = pid;
	last.row = *row;
	last.col = *col;
	last.msb = *msb;
	last.known = known;
	return (known);
}

/* Find an image by serial. */
static struct graphics_image *
graphics_find_serial(u_int serial)
{
	struct graphics_image	find;

	find.serial = serial;
	return (RB_FIND(graphics_serials, &graphics_serials, &find));
}

/*
 * Make a placeholder cell with a client image and placement id and the row
 * and column diacritics.
 */
static void
graphics_set_placeholder(struct grid_cell *gc, u_int serial, u_int pid,
    int row, int col)
{
	struct utf8_data	*ud = &gc->data;
	u_char			*cp;

	gc->fg = colour_join_rgb(serial >> 16, serial >> 8, serial);
	if (pid != 0)
		gc->us = colour_join_rgb(pid >> 16, pid >> 8, pid);
	else
		gc->us = 8;

	if ((u_int)row >= GRAPHICS_NDIACRITICS ||
	    (u_int)col >= GRAPHICS_NDIACRITICS) {
		utf8_set(ud, ' ');
		return;
	}
	cp = ud->data;
	cp += graphics_put_utf8(cp, GRAPHICS_PLACEHOLDER);
	cp += graphics_put_utf8(cp, graphics_diacritics[row]);
	cp += graphics_put_utf8(cp, graphics_diacritics[col]);
	ud->size = cp - ud->data;
	ud->width = 1;
}

/* Get the client placement id for a placeholder, or 0. */
static u_int
graphics_placeholder_part(struct graphics *gr, struct graphics_image *im,
    u_int pid)
{
	struct graphics_placement	*pl;

	if (pid == 0)
		return (0);
	pl = graphics_find_placement(gr, im, pid);
	if (pl == NULL || (~pl->flags & GRAPHICS_VIRTUAL))
		return (0);
	return (graphics_placement_part(pl, 0));
}

/* Remember or forget a placeholder cell which was not drawn. */
static void
graphics_hold(struct graphics *gr, u_int px, u_int py, int hold)
{
	u_int	i;

	if (gr == NULL)
		return;
	for (i = 0; i < graphics_nheld; i++) {
		if (graphics_held[i].gr == gr && graphics_held[i].px == px &&
		    graphics_held[i].py == py)
			break;
	}
	if (i != graphics_nheld) {
		if (!hold)
			graphics_held[i] = graphics_held[--graphics_nheld];
		return;
	}
	if (hold && graphics_nheld != GRAPHICS_MAX_HELD) {
		graphics_held[i].gr = gr;
		graphics_held[i].px = px;
		graphics_held[i].py = py;
		graphics_nheld++;
	}
}

/*
 * Redraw placeholder cells which were held back, once the output which might
 * have finished them has been handled.
 */
void
graphics_flush_held(void)
{
	struct window		*w;
	struct window_pane	*wp;
	u_int			 i;

	for (i = 0; i < graphics_nheld; i++) {
		RB_FOREACH(w, windows, &windows) {
			TAILQ_FOREACH(wp, &w->panes, entry) {
				if (wp->base.graphics != graphics_held[i].gr)
					continue;
				redraw_damage_window(w,
				    wp->xoff + graphics_held[i].px,
				    wp->yoff + graphics_held[i].py, 1, 1);
			}
		}
	}
	graphics_nheld = 0;
}

/*
 * Swap a unicode placeholder cell for one with the client image and placement
 * ids and every diacritic. The line is on screen (not in the history). Returns
 * the cell to draw, or NULL if a live cell should not be drawn yet because the
 * row and column are not known (the diacritics usually follow).
 */
const struct grid_cell *
graphics_draw_cell(struct tty *tty, const struct tty_style_ctx *style_ctx,
    struct grid *gd, u_int px, u_int py, const struct grid_cell *gc,
    struct grid_cell *out, int live)
{
	struct client		*c = tty->client;
	struct graphics		*gr;
	struct graphics_image	*im;
	u_int			 id, pid;
	int			 row, col, msb, known, nomsb;

	if (gc->data.size < 4 || (u_char)gc->data.data[0] != 0xf4)
		return (gc);
	if (!graphics_parse_placeholder(gc, &id, &pid, &row, &col, &msb))
		return (gc);
	memcpy(out, gc, sizeof *out);

	/* A terminal without graphics would show the placeholder itself. */
	if (!graphics_client_supported(c)) {
		utf8_set(&out->data, ' ');
		return (out);
	}

	nomsb = (row >= 0 && col >= 0 && msb < 0);
	known = graphics_resolve_placeholder(gd, px, gd->hsize + py, id, pid,
	    &row, &col, &msb);
	gr = (style_ctx != NULL) ? style_ctx->graphics : NULL;
	if (live && !known) {
		graphics_hold(gr, px, py, 1);
		return (NULL);
	}
	if (gr != NULL && gr->serials) {
		/* Copied from another pane, already has client ids. */
		im = graphics_find_serial(id);
	} else {
		im = graphics_find_id(gr, (u_int)msb << 24 | id);
		if (im != NULL)
			pid = graphics_placeholder_part(gr, im, pid);
	}
	if (im == NULL) {
		/* The high byte may still be to come. */
		if (live && nomsb) {
			graphics_hold(gr, px, py, 1);
			return (NULL);
		}
		graphics_set_placeholder(out, GRAPHICS_SERIAL_UNKNOWN, 0, row,
		    col);
		return (out);
	}
	if (live)
		graphics_hold(gr, px, py, 0);
	graphics_upload(c, im);
	graphics_set_placeholder(out, im->serial, pid, row, col);
	return (out);
}

/*
 * Copy a placeholder cell from one screen to another, such as for a preview,
 * changing it to use client ids since the images belong to the source.
 */
void
graphics_copy_cell(struct screen *src, struct screen *dst, u_int px, u_int py,
    struct grid_cell *gc)
{
	struct graphics		*gr = src->graphics;
	struct graphics_image	*im;
	u_int			 id, pid;
	int			 row, col, msb;

	if (gr == NULL || gr->serials || src == dst)
		return;
	if (!graphics_parse_placeholder(gc, &id, &pid, &row, &col, &msb))
		return;

	if (dst->graphics == NULL) {
		dst->graphics = xcalloc(1, sizeof *dst->graphics);
		TAILQ_INIT(&dst->graphics->images);
		TAILQ_INIT(&dst->graphics->placements);
		dst->graphics->serials = 1;
	}
	if (!dst->graphics->serials) {
		utf8_set(&gc->data, ' ');
		return;
	}

	graphics_resolve_placeholder(src->grid, px, py, id, pid, &row, &col,
	    &msb);
	im = graphics_find_id(gr, (u_int)msb << 24 | id);
	if (im == NULL) {
		utf8_set(&gc->data, ' ');
		return;
	}
	graphics_set_placeholder(gc, im->serial,
	    graphics_placeholder_part(gr, im, pid), row, col);
}

/* Upload an image to a client terminal if it doesn't have it. */
static void
graphics_upload(struct client *c, struct graphics_image *im)
{
	struct graphics_client		*gcl = graphics_client_get(c);
	struct graphics_upload		*gu;
	struct graphics_placement	*pl;
	struct graphics_op		*op;
	struct tty			*tty = &c->tty;
	char				*keys, size[64], o[8];

	if (graphics_client_has(c, im->serial) != NULL)
		return;
	log_debug("%s: uploading image %u to %s (%zu bytes)", __func__,
	    im->serial, c->name, im->total);

	*size = *o = '\0';
	if (im->format != 100)
		xsnprintf(size, sizeof size, ",s=%u,v=%u", im->width, im->height);
	if (im->compression != '\0')
		xsnprintf(o, sizeof o, ",o=%c", im->compression);
	xasprintf(&keys, "a=t,i=%u,f=%u,t=d%s%s", im->serial, im->format, size,
	    o);
	graphics_write(tty, keys, im->data, im->size);
	free(keys);

	TAILQ_FOREACH(op, &im->ops, entry) {
		xasprintf(&keys, "%s%si=%u%s", op->keys,
		    *op->keys == '\0' ? "" : ",", im->serial,
		    op->size != 0 ? ",t=d" : "");
		graphics_write(tty, keys, op->data, op->size);
		free(keys);
	}

	TAILQ_FOREACH(pl, &im->gr->placements, entry) {
		if (pl->im == im && (pl->flags & GRAPHICS_VIRTUAL))
			graphics_write_virtual(tty, pl);
	}

	gu = xcalloc(1, sizeof *gu);
	gu->serial = im->serial;
	RB_INSERT(graphics_uploads, &gcl->uploads, gu);
}

/* Mark a client as needing its placements checked. */
void
graphics_client_check(struct client *c, int force)
{
	if (c == NULL || c->graphics == NULL)
		return;
	c->graphics->flags |= GRAPHICS_CLIENT_CHECK;
	if (force)
		c->graphics->flags |= GRAPHICS_CLIENT_FORCE;
}

/* Work out the part of a placement shown in a client rectangle. */
static void
graphics_make_part(struct client *c, struct graphics_placement *pl,
    u_int cols, u_int rows, u_int col, u_int row, u_int x, u_int y, u_int nx,
    u_int ny, u_int n, struct graphics_parts *parts)
{
	struct tty		*tty = &c->tty;
	struct graphics_part	*gp;
	u_int			 cw, ch, dw, dh, vx0, vx1, vy0, vy1;
	u_int			 px0, px1, py0, py1;
	double			 scale_x, scale_y;

	cw = tty->xpixel == 0 ? DEFAULT_XPIXEL : tty->xpixel;
	ch = tty->ypixel == 0 ? DEFAULT_YPIXEL : tty->ypixel;

	/* The image is drawn in this box inside its cells. */
	if (pl->flags & GRAPHICS_COLS)
		dw = (cols * cw > pl->x_off) ? cols * cw - pl->x_off : 1;
	else
		dw = pl->src_w;
	if (pl->flags & GRAPHICS_ROWS)
		dh = (rows * ch > pl->y_off) ? rows * ch - pl->y_off : 1;
	else
		dh = pl->src_h;

	gp = xcalloc(1, sizeof *gp);
	gp->im = pl->im;
	gp->serial = pl->im->serial;
	gp->id = graphics_placement_part(pl, n);
	gp->x = x;
	gp->y = y;
	gp->z = pl->z;

	if (col == 0 && row == 0 && nx == cols && ny == rows) {
		/* Not cropped. */
		gp->src_x = pl->src_x;
		gp->src_y = pl->src_y;
		gp->src_w = pl->src_w;
		gp->src_h = pl->src_h;
		gp->x_off = pl->x_off;
		gp->y_off = pl->y_off;
		gp->cols = cols;
		gp->rows = rows;
		gp->flags = pl->flags & (GRAPHICS_COLS|GRAPHICS_ROWS);
		TAILQ_INSERT_TAIL(parts, gp, entry);
		return;
	}
	if (dw == 0 || dh == 0 || pl->src_w == 0 || pl->src_h == 0) {
		free(gp);
		return;
	}

	vx0 = col * cw;
	vx1 = (col + nx) * cw;
	vy0 = row * ch;
	vy1 = (row + ny) * ch;
	px0 = (pl->x_off > vx0) ? pl->x_off : vx0;
	px1 = (pl->x_off + dw < vx1) ? pl->x_off + dw : vx1;
	py0 = (pl->y_off > vy0) ? pl->y_off : vy0;
	py1 = (pl->y_off + dh < vy1) ? pl->y_off + dh : vy1;
	if (px0 >= px1 || py0 >= py1) {
		free(gp);
		return;
	}

	scale_x = (double)pl->src_w / dw;
	scale_y = (double)pl->src_h / dh;
	gp->src_x = pl->src_x + (px0 - pl->x_off) * scale_x;
	gp->src_y = pl->src_y + (py0 - pl->y_off) * scale_y;
	gp->src_w = (px1 - px0) * scale_x;
	gp->src_h = (py1 - py0) * scale_y;
	if (gp->src_w == 0)
		gp->src_w = 1;
	if (gp->src_h == 0)
		gp->src_h = 1;
	/* Start in the cell holding the first visible pixel. */
	gp->x += (px0 - vx0) / cw;
	gp->y += (py0 - vy0) / ch;
	vx0 += (px0 - vx0) / cw * cw;
	vy0 += (py0 - vy0) / ch * ch;
	gp->x_off = px0 - vx0;
	gp->y_off = py0 - vy0;

	/* Stretch to the cells if scaled or cut off at a cell edge. */
	if ((pl->flags & GRAPHICS_COLS) || px1 < pl->x_off + dw) {
		gp->cols = (px1 - vx0 + cw - 1) / cw;
		gp->flags |= GRAPHICS_COLS;
	}
	if ((pl->flags & GRAPHICS_ROWS) || py1 < pl->y_off + dh) {
		gp->rows = (py1 - vy0 + ch - 1) / ch;
		gp->flags |= GRAPHICS_ROWS;
	}
	TAILQ_INSERT_TAIL(parts, gp, entry);
}

/* Work out the visible parts of a placement on a client. */
static void
graphics_make_parts(struct client *c, struct window_pane *wp,
    struct graphics_placement *pl, int row, int col,
    struct graphics_parts *parts)
{
	struct tty		*tty = &c->tty;
	struct visible_ranges	*vr;
	struct visible_range	*ri;
	struct {
		u_int	x, nx, y, ny, last;
		int	open;
	}			 rects[GRAPHICS_MAX_PARTS];
	u_int			 nrects = 0, i, j, k, x0, x1, ox, oy, sx, sy;
	u_int			 rx0, rx1, yoff, cy, wy, first, last;
	u_int			 cw, ch, cols, rows;
	int			 y, found;

	tty_window_offset(tty, &ox, &oy, &sx, &sy);
	cw = tty->xpixel == 0 ? DEFAULT_XPIXEL : tty->xpixel;
	ch = tty->ypixel == 0 ? DEFAULT_YPIXEL : tty->ypixel;
	graphics_placement_size(pl, cw, ch, &cols, &rows);
	yoff = (status_at_line(c) == 0) ? status_line_size(c) : 0;

	if (col >= (int)wp->sx || col + (int)cols <= 0)
		return;
	x0 = (col < 0) ? 0 : col;
	x1 = (col + (int)cols > (int)wp->sx) ? wp->sx : col + cols;

	/* Only look at the rows inside the pane. */
	if (row >= (int)wp->sy || row + (int)rows <= 0)
		return;
	first = (row < 0) ? -row : 0;
	last = (row + (int)rows > (int)wp->sy) ? wp->sy - row : rows;

	for (i = first; i < last; i++) {
		for (j = 0; j < nrects; j++) {
			if (rects[j].last + 1 != i)
				rects[j].open = 0;
		}

		y = row + (int)i;
		wy = wp->yoff + y;
		if (wy < oy || wy >= oy + sy)
			continue;
		cy = wy - oy + yoff;

		vr = window_visible_ranges(wp, wp->xoff + x0, wy, x1 - x0, NULL);
		for (k = 0; k < vr->used; k++) {
			ri = &vr->ranges[k];
			if (ri->nx == 0)
				continue;
			rx0 = (ri->px > ox) ? ri->px : ox;
			rx1 = (ri->px + ri->nx < ox + sx) ? ri->px + ri->nx :
			    ox + sx;
			if (rx0 >= rx1)
				continue;

			found = 0;
			for (j = 0; j < nrects; j++) {
				if (rects[j].open &&
				    rects[j].last + 1 == i &&
				    rects[j].x == rx0 - ox &&
				    rects[j].nx == rx1 - rx0) {
					rects[j].ny++;
					rects[j].last = i;
					found = 1;
					break;
				}
			}
			if (found || nrects == GRAPHICS_MAX_PARTS)
				continue;
			rects[nrects].x = rx0 - ox;
			rects[nrects].nx = rx1 - rx0;
			rects[nrects].y = cy;
			rects[nrects].ny = 1;
			rects[nrects].last = i;
			rects[nrects].open = 1;
			nrects++;
		}
	}

	for (j = 0; j < nrects; j++) {
		graphics_make_part(c, pl, cols, rows,
		    rects[j].x + ox - wp->xoff - col,
		    rects[j].last + 1 - rects[j].ny, rects[j].x, rects[j].y,
		    rects[j].nx, rects[j].ny, j, parts);
	}
}

/* Work out every placement part which should be on a client. */
static void
graphics_collect(struct client *c, struct graphics_parts *parts)
{
	struct window			*w = c->session->curw->window;
	struct window_pane		*wp;
	struct graphics			*gr;
	struct graphics_placement	*pl;
	struct grid			*gd;
	u_int				 top;
	int				 mode, py, px;

restart:
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_is_visible(wp) || wp->layout_cell == NULL)
			continue;
		gr = wp->base.graphics;
		if (gr == NULL || TAILQ_EMPTY(&gr->placements))
			continue;

		mode = window_pane_mode(wp);
		if (mode == WINDOW_PANE_NO_MODE) {
			gd = wp->base.grid;
			top = gd->hsize;
		} else if (!window_copy_get_backing(wp, &gd, &top))
			continue;

		TAILQ_FOREACH(pl, &gr->placements, entry) {
			if (pl->flags & GRAPHICS_VIRTUAL)
				continue;
			if (!graphics_placement_position(gd, pl, &py, &px)) {
				/* The line has gone, so has the placement. */
				if (mode == WINDOW_PANE_NO_MODE) {
					graphics_placement_free(pl);
					graphics_free_parts(parts);
					goto restart;
				}
				continue;
			}
			graphics_make_parts(c, wp, pl, py - (int)top, px,
			    parts);
		}
	}
}

/* Free a list of parts. */
static void
graphics_free_parts(struct graphics_parts *parts)
{
	struct graphics_part	*gp, *gp1;

	TAILQ_FOREACH_SAFE(gp, parts, entry, gp1) {
		TAILQ_REMOVE(parts, gp, entry);
		free(gp);
	}
}

/* Find a part in a list. */
static struct graphics_part *
graphics_find_part(struct graphics_parts *parts, struct graphics_part *find)
{
	struct graphics_part	*gp;

	TAILQ_FOREACH(gp, parts, entry) {
		if (gp->serial == find->serial && gp->id == find->id)
			return (gp);
	}
	return (NULL);
}

/* Are two parts the same? */
static int
graphics_same_part(struct graphics_part *gp1, struct graphics_part *gp2)
{
	return (gp1->x == gp2->x &&
	    gp1->y == gp2->y &&
	    gp1->src_x == gp2->src_x &&
	    gp1->src_y == gp2->src_y &&
	    gp1->src_w == gp2->src_w &&
	    gp1->src_h == gp2->src_h &&
	    gp1->x_off == gp2->x_off &&
	    gp1->y_off == gp2->y_off &&
	    gp1->cols == gp2->cols &&
	    gp1->rows == gp2->rows &&
	    gp1->z == gp2->z &&
	    gp1->flags == gp2->flags);
}

/* Put a part on a client terminal. */
static void
graphics_write_part(struct tty *tty, struct graphics_part *gp)
{
	char	*keys, extra[128];
	size_t	 off = 0;

	*extra = '\0';
	if (gp->src_w != 0 && gp->src_h != 0) {
		off += xsnprintf(extra + off, sizeof extra - off,
		    ",x=%u,y=%u,w=%u,h=%u", gp->src_x, gp->src_y, gp->src_w,
		    gp->src_h);
	}
	if (gp->x_off != 0 || gp->y_off != 0) {
		off += xsnprintf(extra + off, sizeof extra - off, ",X=%u,Y=%u",
		    gp->x_off, gp->y_off);
	}
	if (gp->flags & GRAPHICS_COLS)
		off += xsnprintf(extra + off, sizeof extra - off, ",c=%u",
		    gp->cols);
	if (gp->flags & GRAPHICS_ROWS)
		off += xsnprintf(extra + off, sizeof extra - off, ",r=%u",
		    gp->rows);
	if (gp->z != 0)
		xsnprintf(extra + off, sizeof extra - off, ",z=%d", gp->z);

	tty_cursor(tty, gp->x, gp->y);
	xasprintf(&keys, "a=p,i=%u,p=%u,C=1%s", gp->serial, gp->id, extra);
	graphics_write(tty, keys, NULL, 0);
	free(keys);
}

/*
 * Is a pane with placements in a mode? Modes can move the lines without
 * telling anything, so these are always checked.
 */
static int
graphics_client_in_mode(struct client *c)
{
	struct window_pane	*wp;
	struct graphics		*gr;

	TAILQ_FOREACH(wp, &c->session->curw->window->panes, entry) {
		gr = wp->base.graphics;
		if (gr != NULL &&
		    !TAILQ_EMPTY(&gr->placements) &&
		    window_pane_mode(wp) != WINDOW_PANE_NO_MODE)
			return (1);
	}
	return (0);
}

/* Bring the placements on a client terminal up to date. */
void
graphics_client_sync(struct client *c)
{
	struct tty		*tty = &c->tty;
	struct graphics_client	*gcl;
	struct graphics_parts	 parts;
	struct graphics_part	*gp, *old;
	int			 force;

	if (!graphics_client_supported(c) || c->session->curw == NULL)
		return;
	if ((~tty->flags & TTY_STARTED) || (tty->flags & TTY_FREEZE))
		return;
	gcl = graphics_client_get(c);
	if (gcl->flags == 0 &&
	    gcl->generation == graphics_generation &&
	    !graphics_client_in_mode(c))
		return;
	force = (gcl->flags & GRAPHICS_CLIENT_FORCE);

	TAILQ_INIT(&parts);
	graphics_collect(c, &parts);
	gcl->flags = 0;
	gcl->generation = graphics_generation;

	TAILQ_FOREACH(gp, &gcl->shown, entry) {
		if (graphics_find_part(&parts, gp) == NULL)
			graphics_write_delete(tty, gp->serial, gp->id);
	}
	TAILQ_FOREACH(gp, &parts, entry) {
		old = graphics_find_part(&gcl->shown, gp);
		if (!force && old != NULL && graphics_same_part(gp, old))
			continue;
		graphics_upload(c, gp->im);
		graphics_write_part(tty, gp);
	}
	graphics_free_parts(&gcl->shown);
	TAILQ_CONCAT(&gcl->shown, &parts, entry);
	TAILQ_FOREACH(gp, &gcl->shown, entry)
		gp->im = NULL;
}

/*
 * Remove everything from a client terminal before it stops. It is all sent
 * again if it starts again.
 */
void
graphics_client_stop(struct client *c)
{
	struct graphics_client	*gcl = c->graphics;
	struct graphics_upload	*gu, *gu1;
	char			 buf[64];

	if (gcl == NULL || c->tty.term == NULL)
		return;
	tty_raw(&c->tty, "\033_Ga=d,d=A,q=2\033\\");
	RB_FOREACH_SAFE(gu, graphics_uploads, &gcl->uploads, gu1) {
		xsnprintf(buf, sizeof buf, "\033_Ga=d,d=I,i=%u,q=2\033\\",
		    gu->serial);
		tty_raw(&c->tty, buf);
		RB_REMOVE(graphics_uploads, &gcl->uploads, gu);
		free(gu);
	}
	graphics_free_parts(&gcl->shown);
	gcl->flags = GRAPHICS_CLIENT_CHECK|GRAPHICS_CLIENT_FORCE;
}

/* Free client state. */
void
graphics_client_free(struct client *c)
{
	struct graphics_client	*gcl = c->graphics;
	struct graphics_upload	*gu, *gu1;

	if (gcl == NULL)
		return;
	RB_FOREACH_SAFE(gu, graphics_uploads, &gcl->uploads, gu1) {
		RB_REMOVE(graphics_uploads, &gcl->uploads, gu);
		free(gu);
	}
	graphics_free_parts(&gcl->shown);
	free(gcl);
	c->graphics = NULL;
}
