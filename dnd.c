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

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Kitty drag and drop protocol (OSC 72), described at:
 *
 *     https://sw.kovidgoyal.net/kitty/dnd-protocol/
 *
 * Every message has the form:
 *
 *     OSC 72 ; key=value:key=value ; payload ST
 *
 * tmux is the program the outside terminal talks to, and in turn acts as the
 * terminal for the programs in its panes:
 *
 * - Drops. Panes announce they accept drops (t=a) and tmux announces the union
 *   of their MIME types to every client terminal. Move and drop events from a
 *   terminal go to the pane under the pointer with pane-relative positions,
 *   and the pane's replies go back to the terminal. After a drop only that
 *   pane may request data, which is passed between it and the terminal.
 *
 * - Drags. Panes offer to start drags (t=o:x=1) and tmux tells the terminals.
 *   A drag gesture from a terminal goes to the offering pane under the
 *   pointer, and from then until the drag finishes everything for the drag is
 *   passed between that pane and the terminal.
 *
 * Payloads (base64 data, MIME lists, error strings) are passed through chunk
 * by chunk without decoding. The only payloads tmux reassembles are MIME
 * lists, which it needs to combine or compare.
 */

/* Message types from the specification. */
static const char dnd_types[] = "aAmMrRoPpeEkq";

/* Largest payload in one message, from the specification. */
#define DND_CHUNK_SIZE 4096

/* Largest MIME list tmux will reassemble. */
#define DND_MIMES_MAX (1024 * 1024)

/* Terminator for messages tmux sends. */
#define DND_END "\033\\"

/* Message waiting to be written to a pane or a client's terminal. */
struct dnd_out {
	const void		*from;	/* client or pane, NULL for tmux */
	char			 type;
	int			 more;
	char			*s;
	TAILQ_ENTRY(dnd_out)	 entry;
};

/*
 * Messages waiting to be written. Once a chunked message has been started,
 * nothing else may be written until it is finished, because the receiver
 * takes any message in between as the next chunk.
 */
struct dnd_queue {
	const void		*sending; /* in a chunked message */
	char			 sending_type;
	TAILQ_HEAD(, dnd_out)	 list;
};

/* Drag and drop state for a pane. */
struct dnd_pane {
	int		 flags;
#define DND_PANE_ACCEPT 0x1
#define DND_PANE_OFFER 0x2

	u_int		 accept_id;	/* i from t=a */
	char		*accept_mimes;	/* MIME list from t=a */
	char		*machine;	/* machine id from t=a:x=1 */
	u_int		 offer_id;	/* i from t=o */
	char		*offer_machine;	/* machine id from t=o:x=1 */

	char		*partial;	/* MIME list being reassembled */
	size_t		 partial_len;

	int		 chunking;	/* in a chunked message */
	struct dnd_msg	 chunk;		/* first chunk's metadata */
	struct client	*chunk_client;	/* client getting the chunked message */
	int		 chunk_lost;	/* rest of chunked message is dropped */

	struct dnd_queue queue;		/* messages to the pane */
};

/* Drag and drop state for a client. */
struct dnd_client {
	int		 flags;
#define DND_CLIENT_ACCEPTING 0x1
#define DND_CLIENT_OFFERING 0x2

	char		*accept_mimes;	/* MIME list sent with t=a */
	char		*machine;	/* machine id sent with t=a:x=1 */
	char		*offer_machine;	/* machine id sent with t=o:x=1 */

	int		 over;		/* pane under the drag */
	int		 drop;		/* pane that got the drop */
	int		 source;	/* pane that is the drag source */

	int		 hover_idx;	/* status line window under the drag */
	u_int		 hover_window;
	struct event	 hover_timer;

	char		*mimes;		/* MIME list offered by the terminal */
	char		*pane_mimes;	/* MIME list last sent to the pane */
	char		*partial;	/* MIME list being reassembled */
	size_t		 partial_len;

	int		 chunking;	/* in a chunked message */
	struct dnd_msg	 chunk;		/* first chunk's metadata */
	int		 ending;	/* chunked message ends a drag or drop */
	int		 chunk_pane;	/* pane getting the chunked message */
	int		 chunk_lost;	/* rest of chunked message is dropped */

	struct dnd_queue queue;		/* messages to the terminal */
};

/* What a pane is to a client. */
enum dnd_role {
	DND_OVER,
	DND_DROP,
	DND_SOURCE
};

/* Message types passed between a client and a pane for each role. */
static const char *dnd_role_types[] = {
	[DND_OVER] = "mM",
	[DND_DROP] = "rR",
	[DND_SOURCE] = "opPeEk"
};

/* Parse one integer value. */
static int
dnd_parse_number(const char *value, int min, int max, int *out)
{
	const char	*errstr;
	long long	 n;

	n = strtonum(value, min, max, &errstr);
	if (errstr != NULL)
		return (-1);
	*out = n;
	return (0);
}

/* Parse one key=value pair. */
static int
dnd_parse_key(struct dnd_msg *msg, const char *key, const char *value)
{
	const char	*errstr;
	long long	 n;

	/*
	 * Ignore unknown keys: the terminal rejects whole messages with keys
	 * it does not know, so they cannot be forwarded.
	 */
	if (key[0] == '\0' || key[1] != '\0') {
		log_debug("%s: unknown key %s", __func__, key);
		return (0);
	}

	switch (key[0]) {
	case 't':
		if (value[0] == '\0' || value[1] != '\0')
			return (-1);
		if (strchr(dnd_types, value[0]) == NULL)
			return (-1);
		msg->type = value[0];
		return (0);
	case 'm':
		return (dnd_parse_number(value, 0, 1, &msg->more));
	case 'i':
		n = strtonum(value, 0, UINT_MAX, &errstr);
		if (errstr != NULL)
			return (-1);
		msg->id = n;
		return (0);
	case 'o':
		return (dnd_parse_number(value, 0, INT_MAX, &msg->op));
	case 'x':
		return (dnd_parse_number(value, INT_MIN, INT_MAX, &msg->x));
	case 'y':
		return (dnd_parse_number(value, INT_MIN, INT_MAX, &msg->y));
	case 'X':
		/* Pixel positions or unsigned directory handles. */
		msg->X = strtonum(value, INT_MIN, UINT_MAX, &errstr);
		return (errstr == NULL ? 0 : -1);
	case 'Y':
		msg->Y = strtonum(value, INT_MIN, UINT_MAX, &errstr);
		return (errstr == NULL ? 0 : -1);
	}
	log_debug("%s: unknown key %s", __func__, key);
	return (0);
}

/*
 * Parse an OSC 72 message, starting after the "72;". Returns 0 on success or
 * -1 if the message is invalid.
 */
int
dnd_parse(struct dnd_msg *msg, const char *s)
{
	char		*copy, *meta, *pair, *value;
	const char	*payload;
	int		 retval = 0;

	memset(msg, 0, sizeof *msg);
	msg->type = 'a';

	payload = strchr(s, ';');
	if (payload == NULL) {
		copy = xstrdup(s);
		payload = "";
	} else {
		copy = xstrndup(s, payload - s);
		payload++;
	}

	meta = copy;
	while ((pair = strsep(&meta, ":")) != NULL) {
		if (*pair == '\0')
			continue;
		value = strchr(pair, '=');
		if (value == NULL) {
			retval = -1;
			break;
		}
		*value++ = '\0';
		if (dnd_parse_key(msg, pair, value) != 0) {
			retval = -1;
			break;
		}
	}
	free(copy);

	if (retval != 0)
		return (-1);
	msg->payload = xstrdup(payload);
	return (0);
}

/* Free a parsed message. */
void
dnd_free(struct dnd_msg *msg)
{
	free(msg->payload);
	msg->payload = NULL;
}

/* Add a key to the metadata if it is not the default. */
static void
dnd_build_key(char **meta, char key, long long value)
{
	char	*new;

	if (value == 0)
		return;
	xasprintf(&new, "%s:%c=%lld", *meta, key, value);
	free(*meta);
	*meta = new;
}

/*
 * Build an OSC 72 message with the given terminator. Keys with the default
 * value of zero are left out, which the specification treats the same as
 * sending them.
 */
char *
dnd_build(const struct dnd_msg *msg, const char *end)
{
	char	*meta, *s;

	xasprintf(&meta, "t=%c", msg->type);
	dnd_build_key(&meta, 'x', msg->x);
	dnd_build_key(&meta, 'y', msg->y);
	dnd_build_key(&meta, 'X', msg->X);
	dnd_build_key(&meta, 'Y', msg->Y);
	dnd_build_key(&meta, 'o', msg->op);
	dnd_build_key(&meta, 'i', msg->id);
	dnd_build_key(&meta, 'm', msg->more);

	if (msg->payload == NULL || *msg->payload == '\0')
		xasprintf(&s, "\033]72;%s%s", meta, end);
	else
		xasprintf(&s, "\033]72;%s;%s%s", meta, msg->payload, end);
	free(meta);
	return (s);
}

static void	dnd_write(struct window_pane *, struct client *, const void *,
		    char, int, const char *);

/*
 * Send a message to a pane or a client, splitting the payload into chunks if
 * it is too long. The chunk size is a multiple of four so base64 is split on
 * a boundary. Messages are from the client or pane from, or from tmux itself
 * if it is NULL.
 */
static void
dnd_send_from(struct window_pane *wp, struct client *c, const void *from,
    const struct dnd_msg *msg)
{
	struct dnd_msg	 chunk = *msg;
	const char	*payload = msg->payload;
	size_t		 left;
	char		*s;

	if (wp != NULL && wp->event == NULL)
		return;

	left = (payload == NULL) ? 0 : strlen(payload);
	do {
		if (left > DND_CHUNK_SIZE) {
			chunk.payload = xstrndup(payload, DND_CHUNK_SIZE);
			chunk.more = 1;
			payload += DND_CHUNK_SIZE;
			left -= DND_CHUNK_SIZE;
		} else {
			chunk.payload = xstrndup(payload == NULL ? "" : payload,
			    left);
			chunk.more = msg->more;
			left = 0;
		}

		s = dnd_build(&chunk, DND_END);
		dnd_write(wp, c, from, msg->type, chunk.more, s);
		free(s);
		free(chunk.payload);
	} while (left != 0);
}

/* Send a message from tmux itself. */
static void
dnd_send(struct window_pane *wp, struct client *c, const struct dnd_msg *msg)
{
	dnd_send_from(wp, c, NULL, msg);
}

/* Send a message with no payload. */
static void
dnd_send_simple(struct window_pane *wp, struct client *c, char type, int x,
    int y, int op, u_int id)
{
	struct dnd_msg	msg;

	memset(&msg, 0, sizeof msg);
	msg.type = type;
	msg.x = x;
	msg.y = y;
	msg.op = op;
	msg.id = id;
	dnd_send(wp, c, &msg);
}

/* Send an error to a pane. */
static void
dnd_send_error(struct window_pane *wp, const struct dnd_msg *req, char type,
    u_int id, const char *error)
{
	struct dnd_msg	msg;

	/*
	 * Without a client that has the protocol, tmux has not told the
	 * program it is supported, so stay quiet.
	 */
	if (dnd_get_client(wp) == NULL)
		return;

	memset(&msg, 0, sizeof msg);
	msg.type = type;
	msg.x = req->x;
	msg.y = req->y;
	msg.Y = req->Y;
	msg.id = id;
	msg.payload = (char *)error;
	dnd_send(wp, NULL, &msg);
}

/*
 * Add a chunk of a MIME list being reassembled. Returns -1 if it is too long,
 * in which case it is discarded.
 */
static int
dnd_append(char **buf, size_t *len, const char *s)
{
	size_t	slen = strlen(s);

	if (*len + slen > DND_MIMES_MAX) {
		free(*buf);
		*buf = NULL;
		*len = 0;
		return (-1);
	}
	*buf = xrealloc(*buf, *len + slen + 1);
	memcpy(*buf + *len, s, slen + 1);
	*len += slen;
	return (0);
}

/*
 * Finish reassembling a MIME list: set list to the complete list, or NULL if
 * nothing was collected. Returns -1 if it is too long.
 */
static int
dnd_append_finish(char **buf, size_t *len, const char *s, char **list)
{
	*list = NULL;
	if (*buf == NULL && *s == '\0')
		return (0);
	if (dnd_append(buf, len, s) != 0)
		return (-1);
	*list = *buf;
	*buf = NULL;
	*len = 0;
	return (0);
}

/*
 * Handle chunking: a message with m=1 starts a chunked message and later
 * chunks use its metadata, whatever they have themselves.
 */
static void
dnd_continue(struct dnd_msg *msg, int *chunking, struct dnd_msg *saved)
{
	char	*payload = msg->payload;
	int	 more = msg->more;

	if (*chunking) {
		*msg = *saved;
		msg->payload = payload;
		msg->more = more;
		if (!more)
			*chunking = 0;
		return;
	}
	if (more) {
		*chunking = 1;
		*saved = *msg;
		saved->payload = NULL;
	}
}

/* Get pane state, creating it if needed. */
static struct dnd_pane *
dnd_get_pane(struct window_pane *wp)
{
	if (wp->dnd == NULL) {
		wp->dnd = xcalloc(1, sizeof *wp->dnd);
		TAILQ_INIT(&wp->dnd->queue.list);
	}
	return (wp->dnd);
}

static void	dnd_hover_timer(int, short, void *);

/* Get client state, creating it if needed. */
static struct dnd_client *
dnd_get_client_state(struct client *c)
{
	struct dnd_client	*dc;

	if (c->dnd == NULL) {
		dc = c->dnd = xcalloc(1, sizeof *c->dnd);
		dc->over = dc->drop = dc->source = dc->chunk_pane = -1;
		dc->hover_idx = -1;
		evtimer_set(&dc->hover_timer, dnd_hover_timer, c);
		TAILQ_INIT(&dc->queue.list);
	}
	return (c->dnd);
}

/* Get the queue for a pane or a client. */
static struct dnd_queue *
dnd_get_queue(struct window_pane *wp, struct client *c)
{
	if (wp != NULL)
		return (&dnd_get_pane(wp)->queue);
	return (&dnd_get_client_state(c)->queue);
}

/* Write a message to a pane or a client now. */
static void
dnd_write_now(struct window_pane *wp, struct client *c, const char *s)
{
	if (wp != NULL) {
		log_debug("%s: %%%u: %s", __func__, wp->id, s);
		if (wp->event != NULL)
			bufferevent_write(wp->event, s, strlen(s));
	} else {
		log_debug("%s: %s: %s", __func__, c->name, s);
		tty_write_noblock(&c->tty, s, strlen(s));
	}
}

/* Write the messages waiting for a pane or a client that can be written. */
static void
dnd_flush(struct window_pane *wp, struct client *c)
{
	struct dnd_queue	*q = dnd_get_queue(wp, c);
	struct dnd_out		*out;

	for (;;) {
		TAILQ_FOREACH(out, &q->list, entry) {
			if (q->sending == NULL || out->from == q->sending)
				break;
		}
		if (out == NULL)
			break;
		TAILQ_REMOVE(&q->list, out, entry);

		dnd_write_now(wp, c, out->s);
		if (out->from != NULL) {
			if (out->more) {
				q->sending = out->from;
				q->sending_type = out->type;
			} else
				q->sending = NULL;
		}
		free(out->s);
		free(out);
	}
}

/* Write a message to a pane or a client, or queue it if it must wait. */
static void
dnd_write(struct window_pane *wp, struct client *c, const void *from,
    char type, int more, const char *s)
{
	struct dnd_queue	*q = dnd_get_queue(wp, c);
	struct dnd_out		*out;

	out = xcalloc(1, sizeof *out);
	out->from = from;
	out->type = type;
	out->more = more;
	out->s = xstrdup(s);
	TAILQ_INSERT_TAIL(&q->list, out, entry);
	dnd_flush(wp, c);
}

/*
 * Drop the messages from a client or pane waiting for a pane or a client,
 * and end a chunked message it was in the middle of. Only messages of the
 * given types are affected, or all if types is NULL.
 */
static void
dnd_end(struct window_pane *wp, struct client *c, const void *from)
{
	struct dnd_queue	*q = dnd_get_queue(wp, c);
	struct dnd_out		*out, *out1;
	char			*s;

	TAILQ_FOREACH_SAFE(out, &q->list, entry, out1) {
		if (out->from != from)
			continue;
		TAILQ_REMOVE(&q->list, out, entry);
		free(out->s);
		free(out);
	}
	if (q->sending == from) {
		xasprintf(&s, "\033]72;t=%c" DND_END, q->sending_type);
		dnd_write_now(wp, c, s);
		free(s);
		q->sending = NULL;
	}
	dnd_flush(wp, c);
}

/* Free the messages waiting in a queue. */
static void
dnd_free_queue(struct dnd_queue *q)
{
	struct dnd_out	*out;

	while ((out = TAILQ_FIRST(&q->list)) != NULL) {
		TAILQ_REMOVE(&q->list, out, entry);
		free(out->s);
		free(out);
	}
}

/* Is this a client that can carry the protocol? */
static int
dnd_client_ok(struct client *c)
{
	if (c->flags & (CLIENT_DEAD|CLIENT_SUSPENDED|CLIENT_READONLY))
		return (0);
	if (~c->tty.flags & TTY_STARTED)
		return (0);
	if (~c->tty.term->flags & TERM_DND)
		return (0);
	return (1);
}

/*
 * Get the client a pane's drag and drop messages go to: the most recently
 * active attached client showing the window whose terminal supports the
 * protocol. Returns NULL if there is none.
 */
struct client *
dnd_get_client(struct window_pane *wp)
{
	struct window	*w = wp->window;
	struct client	*c, *found = NULL;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->flags & CLIENT_UNATTACHEDFLAGS)
			continue;
		if (c->session == NULL || !session_has(c->session, w))
			continue;
		if (!dnd_client_ok(c))
			continue;
		if (found == NULL ||
		    timercmp(&c->activity_time, &found->activity_time, >))
			found = c;
	}
	return (found);
}

/* Find the client for which a pane has a role. */
static struct client *
dnd_find_client(struct window_pane *wp, enum dnd_role role)
{
	struct client		*c;
	struct dnd_client	*dc;
	int			 id = -1;

	TAILQ_FOREACH(c, &clients, entry) {
		if ((dc = c->dnd) == NULL || !dnd_client_ok(c))
			continue;
		switch (role) {
		case DND_OVER:
			id = dc->over;
			break;
		case DND_DROP:
			id = dc->drop;
			break;
		case DND_SOURCE:
			id = dc->source;
			break;
		}
		if (id == (int)wp->id)
			return (c);
	}
	return (NULL);
}

/* Get a pane by id if it can still take part. */
static struct window_pane *
dnd_find_pane(int id)
{
	struct window_pane	*wp;

	if (id == -1)
		return (NULL);
	wp = window_pane_find_by_id(id);
	if (wp == NULL || (wp->flags & PANE_DESTROYED) || wp->dnd == NULL)
		return (NULL);
	return (wp);
}

/*
 * A pane no longer has a role for a client: end anything for the role that is
 * in flight between them, so neither is left waiting for the rest of a
 * chunked message that will now not be passed on. Messages already waiting
 * are still delivered.
 */
static void
dnd_end_role(struct client *c, int id, enum dnd_role role)
{
	struct dnd_client	*dc = c->dnd;
	struct window_pane	*wp;
	struct dnd_pane		*dp;
	const char		*types = dnd_role_types[role];
	struct dnd_msg		 msg;
	char			*s;

	if ((wp = dnd_find_pane(id)) == NULL)
		return;
	dp = wp->dnd;

	if (dp->chunking && dp->chunk_client == c &&
	    strchr(types, dp->chunk.type) != NULL) {
		memset(&msg, 0, sizeof msg);
		msg.type = dp->chunk.type;
		s = dnd_build(&msg, DND_END);
		dnd_write(NULL, c, wp, msg.type, 0, s);
		free(s);
		dp->chunk_client = NULL;
		dp->chunk_lost = 1;
	}

	if (dc->chunking && dc->chunk_pane == id &&
	    strchr(types, dc->chunk.type) != NULL) {
		memset(&msg, 0, sizeof msg);
		msg.type = dc->chunk.type;
		if (role == DND_SOURCE)
			msg.id = wp->dnd->offer_id;
		else
			msg.id = wp->dnd->accept_id;
		s = dnd_build(&msg, DND_END);
		dnd_write(wp, NULL, c, msg.type, 0, s);
		free(s);
		dc->chunk_pane = -1;
		dc->chunk_lost = 1;
	}
}

/* Pass a message from a pane to a terminal. */
static void
dnd_pane_send(struct window_pane *wp, struct client *c, struct dnd_msg *msg)
{
	wp->dnd->chunk_client = msg->more ? c : NULL;
	msg->id = 0;
	dnd_send_from(NULL, c, wp, msg);
}

/* Pass a message from a terminal to a pane. */
static void
dnd_tty_send(struct client *c, struct window_pane *wp, struct dnd_msg *msg)
{
	struct dnd_client	*dc = c->dnd;

	dc->chunk_pane = msg->more ? (int)wp->id : -1;
	dnd_send_from(wp, NULL, c, msg);
}

/* Is a pane under a drag or drop from another client? */
static int
dnd_pane_taken(struct client *c, struct window_pane *wp, enum dnd_role role)
{
	struct client		*loop;
	struct dnd_client	*dc;

	TAILQ_FOREACH(loop, &clients, entry) {
		if (loop == c || (dc = loop->dnd) == NULL)
			continue;
		if (role == DND_SOURCE) {
			if (dc->source == (int)wp->id)
				return (1);
		} else if (dc->over == (int)wp->id || dc->drop == (int)wp->id)
			return (1);
	}
	return (0);
}

/* Word in a MIME list, for finding duplicates. */
struct dnd_word {
	const char		*s;
	size_t			 len;
	RB_ENTRY(dnd_word)	 entry;
};
RB_HEAD(dnd_words, dnd_word);

static int
dnd_word_cmp(struct dnd_word *w1, struct dnd_word *w2)
{
	int	r;

	r = memcmp(w1->s, w2->s, w1->len < w2->len ? w1->len : w2->len);
	if (r != 0)
		return (r);
	if (w1->len < w2->len)
		return (-1);
	return (w1->len > w2->len);
}
RB_GENERATE_STATIC(dnd_words, dnd_word, entry, dnd_word_cmp);

/* Build the union of the MIME lists of every pane accepting drops. */
static char *
dnd_accept_mimes(void)
{
	struct dnd_words	 words = RB_INITIALIZER(&words);
	struct dnd_word		*dw, *dw1;
	struct window		*w;
	struct window_pane	*wp;
	const char		*s, *end;
	char			*list = NULL;
	size_t			 size = 0, used = 0;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->dnd == NULL ||
			    (~wp->dnd->flags & DND_PANE_ACCEPT) ||
			    wp->dnd->accept_mimes == NULL)
				continue;
			s = wp->dnd->accept_mimes;
			while (*s != '\0') {
				while (*s == ' ')
					s++;
				end = s;
				while (*end != '\0' && *end != ' ')
					end++;
				if (end == s)
					break;
				dw = xmalloc(sizeof *dw);
				dw->s = s;
				dw->len = end - s;
				if (RB_INSERT(dnd_words, &words, dw) != NULL)
					free(dw);
				else {
					if (used + dw->len + 2 > size) {
						size = (used + dw->len + 2) * 2;
						list = xrealloc(list, size);
					}
					if (used != 0)
						list[used++] = ' ';
					memcpy(list + used, s, dw->len);
					used += dw->len;
				}
				s = end;
			}
		}
	}
	RB_FOREACH_SAFE(dw, dnd_words, &words, dw1) {
		RB_REMOVE(dnd_words, &words, dw);
		free(dw);
	}
	if (list == NULL)
		return (xstrdup(""));
	list[used] = '\0';
	return (list);
}

/* Compare two strings either of which may be NULL. */
static int
dnd_strcmp(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return (a != b);
	return (strcmp(a, b));
}

/*
 * Tell a client's terminal whether tmux accepts drops and offers drags, from
 * what the panes want.
 */
static void
dnd_update_client_mimes(struct client *c, const char *mimes)
{
	struct dnd_client	*dc = dnd_get_client_state(c);
	struct window		*w;
	struct window_pane	*wp, *over, *source;
	struct dnd_msg		 msg;
	const char		*machine = NULL, *offer_machine = NULL;
	int			 accept = 0, offer = 0;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->dnd == NULL)
				continue;
			if (wp->dnd->flags & DND_PANE_ACCEPT)
				accept = 1;
			if (wp->dnd->flags & DND_PANE_OFFER) {
				/* Only a machine id all panes agree on. */
				if (!offer)
					offer_machine = wp->dnd->offer_machine;
				else if (dnd_strcmp(offer_machine,
				    wp->dnd->offer_machine) != 0)
					offer_machine = NULL;
				offer = 1;
			}
		}
	}
	if ((over = dnd_find_pane(dc->over)) != NULL)
		machine = over->dnd->machine;
	if ((source = dnd_find_pane(dc->source)) != NULL &&
	    (source->dnd->flags & DND_PANE_OFFER))
		offer_machine = source->dnd->offer_machine;

	if (!accept) {
		if (dc->flags & DND_CLIENT_ACCEPTING) {
			dnd_send_simple(NULL, c, 'A', 0, 0, 0, 0);
			dc->flags &= ~DND_CLIENT_ACCEPTING;
		}
		/* No more moves will come, so forget the window under it. */
		evtimer_del(&dc->hover_timer);
		dc->hover_idx = -1;
		free(dc->accept_mimes);
		dc->accept_mimes = NULL;
		free(dc->machine);
		dc->machine = NULL;
	} else if (~dc->flags & DND_CLIENT_ACCEPTING ||
	    dnd_strcmp(mimes, dc->accept_mimes) != 0 ||
	    dnd_strcmp(machine, dc->machine) != 0) {
		/* A plain t=a also forgets any machine id, so send it first. */
		memset(&msg, 0, sizeof msg);
		msg.type = 'a';
		msg.payload = (char *)mimes;
		dnd_send(NULL, c, &msg);
		dc->flags |= DND_CLIENT_ACCEPTING;
		free(dc->accept_mimes);
		dc->accept_mimes = xstrdup(mimes);

		free(dc->machine);
		dc->machine = NULL;
		if (machine != NULL) {
			msg.x = 1;
			msg.payload = (char *)machine;
			dnd_send(NULL, c, &msg);
			dc->machine = xstrdup(machine);
		}
	}

	if (offer && ((~dc->flags & DND_CLIENT_OFFERING) ||
	    dnd_strcmp(offer_machine, dc->offer_machine) != 0)) {
		memset(&msg, 0, sizeof msg);
		msg.type = 'o';
		msg.x = 1;
		msg.payload = (char *)offer_machine;
		dnd_send(NULL, c, &msg);
		dc->flags |= DND_CLIENT_OFFERING;
		free(dc->offer_machine);
		dc->offer_machine = (offer_machine == NULL) ? NULL :
		    xstrdup(offer_machine);
	} else if (!offer && (dc->flags & DND_CLIENT_OFFERING)) {
		dnd_send_simple(NULL, c, 'o', 2, 0, 0, 0);
		dc->flags &= ~DND_CLIENT_OFFERING;
		free(dc->offer_machine);
		dc->offer_machine = NULL;
	}
}

/* Update one client. */
static void
dnd_update_client(struct client *c)
{
	char	*mimes;

	mimes = dnd_accept_mimes();
	dnd_update_client_mimes(c, mimes);
	free(mimes);
}

/* Update every client that can carry the protocol. */
static void
dnd_update_clients(void)
{
	struct client	*c;
	char		*mimes;

	mimes = dnd_accept_mimes();
	TAILQ_FOREACH(c, &clients, entry) {
		if (dnd_client_ok(c))
			dnd_update_client_mimes(c, mimes);
	}
	free(mimes);
}

/*
 * Client is ready or its flags have changed: tell its terminal what the panes
 * want, or that tmux no longer takes part if the client cannot carry the
 * protocol (for example because it is now read-only).
 */
void
dnd_client_start(struct client *c)
{
	if (dnd_client_ok(c))
		dnd_update_client(c);
	else if (c->tty.flags & TTY_STARTED)
		dnd_client_stop(c);
}

/* Is a client in the middle of a drag or drop? */
int
dnd_client_active(struct client *c)
{
	struct dnd_client	*dc = c->dnd;

	if (dc == NULL)
		return (0);
	return (dc->over != -1 || dc->drop != -1 || dc->source != -1 ||
	    dc->chunking || dc->partial != NULL);
}

/*
 * Find the pane at a position on a client, returning the position inside the
 * pane and the client position of the pane's top-left cell.
 */
static struct window_pane *
dnd_pane_at(struct client *c, int x, int y, int *px, int *py, int *cx,
    int *cy)
{
	struct session		*s = c->session;
	struct window		*w;
	struct window_pane	*wp;
	u_int			 ox, oy, sx, sy, xx, yy, lines;
	int			 at;

	if (s == NULL || x < 0 || y < 0)
		return (NULL);
	w = s->curw->window;
	xx = x;
	yy = y;

	at = status_at_line(c);
	lines = status_line_size(c);
	if (at != -1 && yy >= (u_int)at && yy < at + lines)
		return (NULL);
	if (at == 0)
		yy -= lines;

	tty_window_offset(&c->tty, &ox, &oy, &sx, &sy);
	if (xx >= sx || yy >= sy)
		return (NULL);
	xx += ox;
	yy += oy;

	if (w->modal != NULL && !window_pane_contains(w->modal, xx, yy))
		return (NULL);
	wp = window_get_active_at(w, xx, yy);
	if (wp == NULL)
		return (NULL);
	if ((int)xx < wp->xoff || (int)xx >= wp->xoff + (int)wp->sx)
		return (NULL);
	if ((int)yy < wp->yoff || (int)yy >= wp->yoff + (int)wp->sy)
		return (NULL);

	*px = xx - wp->xoff;
	*py = yy - wp->yoff;
	*cx = x - *px;
	*cy = y - *py;
	return (wp);
}

/* Make a position from a terminal relative to a pane. */
static void
dnd_pane_position(struct client *c, struct dnd_msg *msg, int x, int y,
    int cx, int cy)
{
	msg->x = x;
	msg->y = y;
	if (c->tty.xpixel != 0) {
		msg->X -= cx * (int)c->tty.xpixel;
		if (msg->X < 0)
			msg->X = 0;
	}
	if (c->tty.ypixel != 0) {
		msg->Y -= cy * (int)c->tty.ypixel;
		if (msg->Y < 0)
			msg->Y = 0;
	}
}

/* Tell the pane under a drag that it has left. */
static void
dnd_leave(struct client *c)
{
	struct dnd_client	*dc = c->dnd;
	struct window_pane	*wp;

	dnd_end_role(c, dc->over, DND_OVER);
	if ((wp = dnd_find_pane(dc->over)) != NULL &&
	    (wp->dnd->flags & DND_PANE_ACCEPT)) {
		dnd_send_simple(wp, NULL, 'm', -1, -1, 0,
		    wp->dnd->accept_id);
	}
	dc->over = -1;
	free(dc->pane_mimes);
	dc->pane_mimes = NULL;
}

/* Find the window at a position in a client's status line. */
static struct winlink *
dnd_status_window_at(struct client *c, int x, int y)
{
	struct session		*s = c->session;
	struct style_range	*sr;
	u_int			 lines;
	int			 at;

	if (s == NULL || x < 0 || y < 0)
		return (NULL);
	at = status_at_line(c);
	lines = status_line_size(c);
	if (at == -1 || y < at || (u_int)y >= at + lines)
		return (NULL);
	sr = status_get_range(c, x, y - at);
	if (sr == NULL || sr->type != STYLE_RANGE_WINDOW)
		return (NULL);
	return (winlink_find_by_index(&s->windows, sr->argument));
}

/* Hovered long enough over a window in the status line, so switch to it. */
static void
dnd_hover_timer(__unused int fd, __unused short events, void *arg)
{
	struct client		*c = arg;
	struct dnd_client	*dc = c->dnd;
	struct session		*s = c->session;
	struct winlink		*wl;

	if (dc == NULL || s == NULL)
		return;
	wl = winlink_find_by_index(&s->windows, dc->hover_idx);
	dc->hover_idx = -1;
	if (wl == NULL || wl == s->curw || wl->window->id != dc->hover_window)
		return;
	if (!options_get_number(s->options, "drag-select-window"))
		return;
	log_debug("%s: drag switching to window %d", c->name, wl->idx);
	if (session_select(s, wl->idx) == 0) {
		server_redraw_session(s);
		s->curw->window->latest = c;
		recalculate_sizes();
	}
}

/*
 * Start or cancel switching to a window in the status line under a drag, like
 * hovering over a browser tab.
 */
static void
dnd_hover(struct client *c, struct winlink *wl)
{
	struct dnd_client	*dc = c->dnd;
	struct session		*s = c->session;
	struct timeval		 tv;
	int			 delay;

	if (wl != NULL && (wl == s->curw ||
	    !options_get_number(s->options, "drag-select-window")))
		wl = NULL;
	if (wl != NULL && dc->hover_idx == wl->idx &&
	    dc->hover_window == wl->window->id)
		return;
	evtimer_del(&dc->hover_timer);
	dc->hover_idx = -1;
	if (wl == NULL)
		return;
	dc->hover_idx = wl->idx;
	dc->hover_window = wl->window->id;
	delay = options_get_number(s->options, "drag-select-window-time");
	tv.tv_sec = delay / 1000;
	tv.tv_usec = (delay % 1000) * 1000;
	evtimer_add(&dc->hover_timer, &tv);
}

/* Handle a move or drop from a terminal. */
static void
dnd_tty_move(struct client *c, struct dnd_msg *msg)
{
	struct dnd_client	*dc = c->dnd;
	struct window_pane	*wp;
	struct dnd_msg		 out;
	char			*mimes = NULL;
	int			 px, py, cx, cy, error;

	/* Collect the MIME list, which may be chunked. */
	if (msg->more) {
		error = dnd_append(&dc->partial, &dc->partial_len,
		    msg->payload);
	} else {
		error = dnd_append_finish(&dc->partial, &dc->partial_len,
		    msg->payload, &mimes);
	}
	if (error != 0) {
		/* Too long, so drop the whole message and reject the drag. */
		log_debug("%s: MIME list too long", c->name);
		dc->chunk_lost = 1;
		free(dc->mimes);
		dc->mimes = NULL;
		dnd_hover(c, NULL);
		dnd_leave(c);
		if (msg->type == 'M')
			dnd_send_simple(NULL, c, 'r', 0, 0, 0, 0);
		else
			dnd_send_simple(NULL, c, 'm', 0, 0, 0, 0);
		return;
	}
	if (msg->more)
		return;
	if (mimes != NULL) {
		free(dc->mimes);
		dc->mimes = mimes;
	}

	if (msg->x == -1 && msg->y == -1) {
		dnd_hover(c, NULL);
		dnd_leave(c);
		return;
	}
	if (msg->type == 'M')
		dnd_hover(c, NULL);
	else
		dnd_hover(c, dnd_status_window_at(c, msg->x, msg->y));

	wp = dnd_pane_at(c, msg->x, msg->y, &px, &py, &cx, &cy);
	if (wp != NULL && (wp->dnd == NULL ||
	    (~wp->dnd->flags & DND_PANE_ACCEPT) ||
	    dnd_pane_taken(c, wp, DND_OVER)))
		wp = NULL;
	if ((wp == NULL ? -1 : (int)wp->id) != dc->over) {
		dnd_leave(c);
		if (wp != NULL)
			dc->over = wp->id;

		/*
		 * The terminal keeps the last answer until it gets another, so
		 * reject until the new pane answers for itself.
		 */
		dnd_send_simple(NULL, c, 'm', 0, 0, 0, 0);
		dnd_update_client(c);
	}

	if (wp == NULL) {
		/* A drop with nowhere to go is cancelled. */
		if (msg->type == 'M')
			dnd_send_simple(NULL, c, 'r', 0, 0, 0, 0);
		return;
	}

	out = *msg;
	dnd_pane_position(c, &out, px, py, cx, cy);
	out.id = wp->dnd->accept_id;
	out.more = 0;
	if (msg->type == 'M' || dnd_strcmp(dc->mimes, dc->pane_mimes) != 0)
		out.payload = dc->mimes;
	else
		out.payload = NULL;
	free(dc->pane_mimes);
	dc->pane_mimes = (dc->mimes == NULL) ? NULL : xstrdup(dc->mimes);
	dnd_tty_send(c, wp, &out);

	if (msg->type == 'M') {
		/*
		 * Only this pane may now ask for data. The next move is a new
		 * drag, so forget the pane it is over.
		 */
		dnd_end_role(c, dc->over, DND_OVER);
		dnd_end_role(c, dc->drop, DND_DROP);
		dc->drop = wp->id;
		dc->over = -1;
		free(dc->pane_mimes);
		dc->pane_mimes = NULL;
	}
}

/* Pass a message from a terminal to a pane. */
static void
dnd_tty_forward(struct client *c, struct dnd_msg *msg, int id, int drag)
{
	struct window_pane	*wp;
	struct dnd_msg		 out;

	if ((wp = dnd_find_pane(id)) == NULL) {
		log_debug("%s: no pane for t=%c", c->name, msg->type);
		return;
	}
	out = *msg;
	out.id = drag ? wp->dnd->offer_id : wp->dnd->accept_id;
	dnd_tty_send(c, wp, &out);
}

/* Handle a drag gesture from a terminal. */
static void
dnd_tty_gesture(struct client *c, struct dnd_msg *msg)
{
	struct dnd_client	*dc = c->dnd;
	struct window_pane	*wp;
	struct dnd_msg		 out;
	int			 px, py, cx, cy;

	wp = dnd_pane_at(c, msg->x, msg->y, &px, &py, &cx, &cy);
	if (wp == NULL || wp->dnd == NULL ||
	    (~wp->dnd->flags & DND_PANE_OFFER) ||
	    dnd_pane_taken(c, wp, DND_SOURCE)) {
		log_debug("%s: no pane offering at %d,%d", c->name, msg->x,
		    msg->y);
		return;
	}
	dnd_end_role(c, dc->source, DND_SOURCE);
	dc->source = wp->id;

	/* The terminal needs the source pane's machine id. */
	dnd_update_client(c);

	out = *msg;
	dnd_pane_position(c, &out, px, py, cx, cy);
	out.id = wp->dnd->offer_id;
	dnd_tty_send(c, wp, &out);
}

/*
 * Handle an OSC 72 message from the outside terminal, without the leading
 * "\033]72;" or the terminator.
 */
void
dnd_tty_message(struct client *c, const char *buf, size_t len)
{
	struct tty		*tty = &c->tty;
	struct dnd_client	*dc;
	struct dnd_msg		 msg;
	char			*copy;
	int			 first;

	copy = xstrndup(buf, len);
	if (dnd_parse(&msg, copy) != 0) {
		log_debug("%s: bad OSC 72: %s", c->name, copy);
		free(copy);
		return;
	}
	free(copy);

	if (msg.type == 'q') {
		/* Reply to our query, so the terminal has the protocol. */
		if (~tty->term->flags & TERM_DND) {
			log_debug("%s: terminal supports dnd", c->name);
			tty_parse_client_features(c, "dnd", ",");
			tty_update_features(tty);
		}
		dnd_client_start(c);
		dnd_free(&msg);
		return;
	}
	if (!dnd_client_ok(c)) {
		/* Read-only clients may not send anything to panes. */
		log_debug("%s: ignoring OSC 72 t=%c", c->name, msg.type);
		dnd_client_start(c);
		dnd_free(&msg);
		return;
	}
	dc = dnd_get_client_state(c);
	first = !dc->chunking;
	dnd_continue(&msg, &dc->chunking, &dc->chunk);
	if (first)
		dc->chunk_lost = 0;
	else if (dc->chunk_lost) {
		/* What it was part of has ended. */
		dnd_free(&msg);
		return;
	}

	switch (msg.type) {
	case 'm':
	case 'M':
		dnd_tty_move(c, &msg);
		break;
	case 'r':
	case 'R':
		/* Data or an error for the pane that got the drop. */
		dnd_tty_forward(c, &msg, dc->drop, 0);

		/* An error ends the drop once it has been passed on. */
		if (msg.type == 'R' && !msg.more) {
			dnd_end_role(c, dc->drop, DND_DROP);
			dc->drop = -1;
		}
		break;
	case 'o':
		dnd_tty_gesture(c, &msg);
		break;
	case 'e':
	case 'E':
	case 'k':
		/* Events, errors or requests for the drag source. */
		dnd_tty_forward(c, &msg, dc->source, 1);

		/*
		 * A drop or an error other than OK ends the drag, but only
		 * once the whole message has been passed on.
		 */
		if (first) {
			dc->ending = ((msg.type == 'e' && msg.x == 4) ||
			    (msg.type == 'E' &&
			    strncmp(msg.payload, "OK", 2) != 0));
		}
		if (!msg.more && dc->ending) {
			dnd_end_role(c, dc->source, DND_SOURCE);
			dc->source = -1;
			dc->ending = 0;
		}
		break;
	default:
		log_debug("%s: unhandled OSC 72 type %c", c->name, msg.type);
		break;
	}
	dnd_free(&msg);
}

/* Handle a t=a or t=A from a pane. */
static void
dnd_pane_accept(struct window_pane *wp, struct dnd_msg *msg)
{
	struct dnd_pane		*dp = wp->dnd;
	struct client		*c;
	struct dnd_client	*dc;
	char			*list = NULL;
	int			 error;

	if (msg->type == 'A') {
		/* A drag over the pane is no longer over anything. */
		TAILQ_FOREACH(c, &clients, entry) {
			if ((dc = c->dnd) == NULL || dc->over != (int)wp->id)
				continue;
			dnd_end_role(c, dc->over, DND_OVER);
			dc->over = -1;
			free(dc->pane_mimes);
			dc->pane_mimes = NULL;
			if (dnd_client_ok(c))
				dnd_send_simple(NULL, c, 'm', 0, 0, 0, 0);
		}
		dp->flags &= ~DND_PANE_ACCEPT;
		free(dp->accept_mimes);
		dp->accept_mimes = NULL;
		free(dp->machine);
		dp->machine = NULL;
		dnd_update_clients();
		return;
	}

	if (msg->more) {
		error = dnd_append(&dp->partial, &dp->partial_len,
		    msg->payload);
	} else {
		error = dnd_append_finish(&dp->partial, &dp->partial_len,
		    msg->payload, &list);
	}
	if (error != 0) {
		/* Too long, so ignore the whole message. */
		log_debug("%s: %%%u: MIME list too long", __func__, wp->id);
		dp->chunk_lost = 1;
		return;
	}
	if (msg->more)
		return;

	if (msg->x == 1) {
		/* Machine id for remote drops. */
		free(dp->machine);
		dp->machine = list;
	} else {
		dp->flags |= DND_PANE_ACCEPT;
		dp->accept_id = msg->id;
		free(dp->accept_mimes);
		dp->accept_mimes = list;
	}
	dnd_update_clients();
}

/* Handle a t=o from a pane. */
static void
dnd_pane_offer(struct window_pane *wp, struct dnd_msg *msg)
{
	struct dnd_pane	*dp = wp->dnd;
	struct client	*c;
	char		*machine = NULL;
	int		 error;

	if (msg->id != 0)
		dp->offer_id = msg->id;
	switch (msg->x) {
	case 1:
		/* A new registration, so its id even if zero. */
		dp->offer_id = msg->id;
		/* The payload is the machine id for remote drags. */
		if (msg->more) {
			error = dnd_append(&dp->partial, &dp->partial_len,
			    msg->payload);
		} else {
			error = dnd_append_finish(&dp->partial,
			    &dp->partial_len, msg->payload, &machine);
		}
		if (error != 0) {
			/* Too long, so ignore the whole message. */
			log_debug("%s: %%%u: machine id too long", __func__,
			    wp->id);
			dp->chunk_lost = 1;
			return;
		}
		if (msg->more)
			return;
		free(dp->offer_machine);
		dp->offer_machine = machine;
		dp->flags |= DND_PANE_OFFER;
		dnd_update_clients();
		return;
	case 2:
		dp->flags &= ~DND_PANE_OFFER;
		free(dp->offer_machine);
		dp->offer_machine = NULL;
		dnd_update_clients();
		return;
	}

	/* The MIME types for a drag after a gesture. */
	if ((c = dnd_find_client(wp, DND_SOURCE)) == NULL) {
		dnd_send_error(wp, msg, 'E', dp->offer_id,
		    "EINVAL:no drag in progress");
		return;
	}
	dnd_pane_send(wp, c, msg);
}

/* Handle a t=m reply from a pane. */
static void
dnd_pane_move(struct window_pane *wp, struct dnd_msg *msg)
{
	struct client	*c;

	if ((c = dnd_find_client(wp, DND_OVER)) == NULL) {
		log_debug("%s: %%%u is not under a drag", __func__, wp->id);
		return;
	}
	dnd_pane_send(wp, c, msg);
}

/* Handle a t=r request or completion from a pane. */
static void
dnd_pane_request(struct window_pane *wp, struct dnd_msg *msg)
{
	struct dnd_pane	*dp = wp->dnd;
	struct client	*c;
	int		 done;

	done = (msg->x == 0 && msg->y == 0 && msg->Y == 0);
	if ((c = dnd_find_client(wp, DND_DROP)) == NULL) {
		if (!done) {
			dnd_send_error(wp, msg, 'R', dp->accept_id,
			    "EPERM:drop data can only be requested after a "
			    "drop");
		}
		return;
	}
	dnd_pane_send(wp, c, msg);
	if (done && !msg->more) {
		dnd_end_role(c, c->dnd->drop, DND_DROP);
		c->dnd->drop = -1;
	}
}

/* Handle a message for the drag source's terminal from a pane. */
static void
dnd_pane_drag(struct window_pane *wp, struct dnd_msg *msg)
{
	struct dnd_pane	*dp = wp->dnd;
	struct client	*c;

	if ((c = dnd_find_client(wp, DND_SOURCE)) == NULL) {
		if (msg->type == 'P' && msg->x == -1) {
			dnd_send_error(wp, msg, 'E', dp->offer_id,
			    "EPERM:no drag in progress");
		}
		return;
	}
	dnd_pane_send(wp, c, msg);
	if (msg->type == 'E' && msg->y == -1 && !msg->more) {
		dnd_end_role(c, c->dnd->source, DND_SOURCE);
		c->dnd->source = -1;
	}
}

/* Handle an OSC 72 message other than a query from a pane. */
void
dnd_pane_message(struct window_pane *wp, struct dnd_msg *msg)
{
	struct dnd_pane	*dp = dnd_get_pane(wp);
	int		 first = !dp->chunking;

	dnd_continue(msg, &dp->chunking, &dp->chunk);
	if (first)
		dp->chunk_lost = 0;
	else if (dp->chunk_lost) {
		/* What it was part of has ended. */
		return;
	}

	switch (msg->type) {
	case 'a':
	case 'A':
		dnd_pane_accept(wp, msg);
		break;
	case 'o':
		dnd_pane_offer(wp, msg);
		break;
	case 'm':
		dnd_pane_move(wp, msg);
		break;
	case 'r':
		dnd_pane_request(wp, msg);
		break;
	case 'p':
	case 'P':
	case 'e':
	case 'E':
	case 'k':
		dnd_pane_drag(wp, msg);
		break;
	default:
		log_debug("%s: %%%u: unhandled type %c", __func__, wp->id,
		    msg->type);
		break;
	}
}

/* Pane is being destroyed: end anything it is part of. */
void
dnd_pane_destroy(struct window_pane *wp)
{
	struct dnd_pane		*dp = wp->dnd;
	struct client		*c;
	struct dnd_client	*dc;

	if (dp == NULL)
		return;

	TAILQ_FOREACH(c, &clients, entry) {
		if ((dc = c->dnd) == NULL || !dnd_client_ok(c))
			continue;
		dnd_end(NULL, c, wp);
		if (dc->over == (int)wp->id) {
			dnd_send_simple(NULL, c, 'm', 0, 0, 0, 0);
			dc->over = -1;
		}
		if (dc->drop == (int)wp->id) {
			dnd_send_simple(NULL, c, 'r', 0, 0, 0, 0);
			dc->drop = -1;
		}
		if (dc->source == (int)wp->id) {
			dnd_send_simple(NULL, c, 'E', 0, -1, 0, 0);
			dc->source = -1;
		}
	}

	free(dp->accept_mimes);
	free(dp->machine);
	free(dp->offer_machine);
	free(dp->partial);
	dnd_free_queue(&dp->queue);
	free(dp);
	wp->dnd = NULL;

	dnd_update_clients();
}

/*
 * Client terminal is being stopped: end any drag or drop and tell the
 * terminal tmux no longer takes part, so it does not send messages to the
 * shell. It is announced again if the client is started again.
 */
void
dnd_client_stop(struct client *c)
{
	struct dnd_client	*dc = c->dnd;
	struct window_pane	*wp;
	char			*s;

	if (dc == NULL)
		return;

	/* Messages still waiting are dropped, so end a chunked message. */
	if (dc->queue.sending != NULL) {
		xasprintf(&s, "\033]72;t=%c" DND_END, dc->queue.sending_type);
		tty_raw(&c->tty, s);
		free(s);
	}
	if ((wp = dnd_find_pane(dc->over)) != NULL &&
	    (wp->dnd->flags & DND_PANE_ACCEPT)) {
		dnd_send_simple(wp, NULL, 'm', -1, -1, 0,
		    wp->dnd->accept_id);
	}
	if (dc->drop != -1)
		tty_raw(&c->tty, "\033]72;t=r" DND_END);
	if ((wp = dnd_find_pane(dc->source)) != NULL)
		dnd_send_simple(wp, NULL, 'e', 4, 1, 0, wp->dnd->offer_id);
	if (dc->flags & DND_CLIENT_ACCEPTING)
		tty_raw(&c->tty, "\033]72;t=A" DND_END);
	if (dc->flags & DND_CLIENT_OFFERING)
		tty_raw(&c->tty, "\033]72;t=o:x=2" DND_END);

	dnd_client_free(c);
}

/* Client is going away: free its state. */
void
dnd_client_free(struct client *c)
{
	struct dnd_client	*dc = c->dnd;
	struct window		*w;
	struct window_pane	*wp;

	if (dc == NULL)
		return;

	/* Finish anything the client was sending to a pane. */
	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->dnd == NULL)
				continue;
			dnd_end(wp, NULL, c);
			if (wp->dnd->chunk_client == c) {
				wp->dnd->chunk_client = NULL;
				wp->dnd->chunk_lost = 1;
			}
		}
	}

	evtimer_del(&dc->hover_timer);
	dnd_free_queue(&dc->queue);
	free(dc->accept_mimes);
	free(dc->machine);
	free(dc->offer_machine);
	free(dc->mimes);
	free(dc->pane_mimes);
	free(dc->partial);
	free(dc);
	c->dnd = NULL;
}
