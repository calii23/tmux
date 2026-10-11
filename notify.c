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

#include <netinet/in.h>

#include <ctype.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Kitty desktop notifications protocol (OSC 99), described at:
 *
 *     https://sw.kovidgoyal.net/kitty/desktop-notifications/
 *
 * Every message has the form:
 *
 *     OSC 99 ; key=value:key=value ; payload ST
 *
 * The legacy OSC 9 ; text ST is taken as a notification with only a title.
 *
 * tmux is the terminal for the programs in its panes and a program to the
 * outside terminal:
 *
 * - Notifications. tmux collects every chunk of a notification itself, then
 *   sends it under an id of its own to the most recently used client terminal
 *   with the protocol attached to the window. An update or close with the
 *   same id from the pane goes to the same terminal. If the pane is not the
 *   visible active pane in that client, tmux decides the occasion (o=) itself.
 *   Icon data for a cache id (g=) is kept and sent again with every
 *   notification using it, so the terminal need not have seen it before.
 *
 * - Replies. tmux always asks the terminal for activation reports and close
 *   events. Activation with focus selects the pane in that client. Activation
 *   reports, buttons and close events go back to the pane under its own id if
 *   it asked for them.
 *
 * - Queries. tmux asks each terminal for its capabilities when it starts and
 *   answers queries from panes with them straight away, so the answer comes
 *   before the reply to a following device attributes request. Queries for
 *   live notifications go to the terminals showing them and the answers are
 *   put together.
 */

/* Largest data in one message sent, before base64. */
#define NOTIFY_CHUNK 2048

/* Largest notification tmux will take. */
#define NOTIFY_MAX (1024 * 1024)

/* Most notifications kept for each pane. */
#define NOTIFY_PANE_MAX 32

/* Most alive queries waiting for each client and how long they wait. */
#define NOTIFY_ALIVE_MAX 16
#define NOTIFY_ALIVE_TIMEOUT 5

/* Most icons kept for cache ids. */
#define NOTIFY_ICONS_MAX 32

/* Capabilities if the terminal did not give them. */
#define NOTIFY_DEFAULT_CAPS "a=focus,report:o=always,unfocused,invisible:" \
	"p=title,body:u=0,1,2"

/* Keys passed on to the terminal unchanged. */
static const char notify_keys[] = "fgnstuw";
#define NOTIFY_NKEYS ((sizeof notify_keys) - 1)

/* Payload types collected for a notification, in the order they are sent. */
static const char *notify_types[] = { "title", "body", "buttons", "icon" };
#define NOTIFY_NTYPES nitems(notify_types)

/* A parsed message. Values are pointers into the copy. */
struct notify_msg {
	char	*copy;
	char	*id;
	char	*type;
	char	*actions;
	char	*occasion;
	int	 done;
	int	 base64;
	int	 close;	/* -1 if not given */
	char	*keys[NOTIFY_NKEYS];	/* allocated */
	char	*payload;
};

/* Data of one payload type. */
struct notify_data {
	char	*data;
	size_t	 len;
	char	 quantum[5];	/* base64 not yet decoded */
	size_t	 quantum_len;
};

/* A notification from a pane. */
struct notify {
	u_int			 id;		/* number for the terminal id */
	u_int			 pane;
	char			*pane_id;	/* id the pane gave, NULL if none */
	struct client		*client;	/* terminal it was sent to */
	u_int			 sent;		/* send number when last sent */
	int			 actions;	/* flags when last sent */

	int			 flags;
#define NOTIFY_PENDING 0x1
#define NOTIFY_FAILED 0x2
#define NOTIFY_FOCUS 0x4
#define NOTIFY_REPORT 0x8
#define NOTIFY_CLOSE 0x10
#define NOTIFY_STALE 0x20

	char			*occasion;
	char			*keys[NOTIFY_NKEYS];
	struct notify_data	 data[NOTIFY_NTYPES];
	size_t			 size;

	TAILQ_ENTRY(notify)	 entry;
};
static TAILQ_HEAD(, notify) notify_list = TAILQ_HEAD_INITIALIZER(notify_list);

/* A query for live notifications from a pane. */
struct notify_alive {
	u_int				 id;
	u_int				 pane;
	char				*pane_id;
	u_int				 waiting;	/* terminals not replied */
	char				*list;
	TAILQ_ENTRY(notify_alive)	 entry;
};
static TAILQ_HEAD(, notify_alive) notify_alive_list =
    TAILQ_HEAD_INITIALIZER(notify_alive_list);

/* An icon kept for a cache id. */
struct notify_icon {
	char				*cache_id;
	char				*data;
	size_t				 len;
	TAILQ_ENTRY(notify_icon)	 entry;
};
static TAILQ_HEAD(, notify_icon) notify_icons =
    TAILQ_HEAD_INITIALIZER(notify_icons);
static u_int notify_nicons;

/* An alive query sent to a terminal. */
struct notify_sent_alive {
	u_int	id;
	u_int	sent;		/* send number when the query was sent */
	time_t	time;
};

/* State for a client. */
struct notify_client {
	char			*caps;		/* capabilities from terminal */
	int			 queried;
	struct notify_sent_alive *alive;	/* queries waiting for reply */
	u_int			 nalive;
	struct event		 alive_timer;
};

static u_int notify_next;
static u_int notify_sends;

static void	notify_alive_timer(int, short, void *);

/* Copy a value leaving out characters that are not allowed. */
static char *
notify_copy(const char *value, const char *allowed)
{
	char	*copy, *cp;

	cp = copy = xmalloc(strlen(value) + 1);
	for (; *value != '\0'; value++) {
		if (isalnum((u_char)*value) || strchr(allowed, *value) != NULL)
			*cp++ = *value;
	}
	*cp = '\0';
	return (copy);
}

/* Free a parsed message. */
static void
notify_free_msg(struct notify_msg *msg)
{
	u_int	i;

	for (i = 0; i < NOTIFY_NKEYS; i++)
		free(msg->keys[i]);
	free(msg->copy);
}

/* Parse a message, starting after the "99;". */
static void
notify_parse(struct notify_msg *msg, const char *s)
{
	char		*meta, *pair, *value, *new, *copy;
	const char	*cp;
	u_int		 i;

	memset(msg, 0, sizeof *msg);
	msg->done = 1;
	msg->close = -1;
	msg->copy = xstrdup(s);

	meta = msg->copy;
	if ((msg->payload = strchr(meta, ';')) != NULL)
		*msg->payload++ = '\0';
	else
		msg->payload = meta + strlen(meta);

	while ((pair = strsep(&meta, ":")) != NULL) {
		if ((value = strchr(pair, '=')) == NULL) {
			if (*pair != '\0')
				log_debug("%s: bad key %s", __func__, pair);
			continue;
		}
		*value++ = '\0';
		if (strlen(pair) != 1) {
			log_debug("%s: unknown key %s", __func__, pair);
			continue;
		}
		switch (*pair) {
		case 'i':
			msg->id = value;
			break;
		case 'p':
			msg->type = value;
			break;
		case 'a':
			msg->actions = value;
			break;
		case 'o':
			msg->occasion = value;
			break;
		case 'd':
			msg->done = (strcmp(value, "0") != 0);
			break;
		case 'e':
			msg->base64 = (strcmp(value, "1") == 0);
			break;
		case 'c':
			msg->close = (strcmp(value, "1") == 0);
			break;
		default:
			if ((cp = strchr(notify_keys, *pair)) == NULL) {
				log_debug("%s: unknown key %s", __func__, pair);
				break;
			}
			copy = notify_copy(value, "+/=-_.,");
			i = cp - notify_keys;
			if (*copy == '\0')
				free(copy);
			else if (msg->keys[i] == NULL)
				msg->keys[i] = copy;
			else {
				xasprintf(&new, "%s:%c=%s", msg->keys[i], *pair,
				    copy);
				free(copy);
				free(msg->keys[i]);
				msg->keys[i] = new;
			}
			break;
		}
	}
	if (msg->type == NULL)
		msg->type = (char *)"title";
}

/* Get a pane id from a message, NULL if none. */
static char *
notify_pane_id(const struct notify_msg *msg)
{
	char	*id;

	if (msg->id == NULL)
		return (NULL);
	id = notify_copy(msg->id, "-_+.");
	if (*id == '\0' || strcmp(id, "0") == 0) {
		free(id);
		return (NULL);
	}
	return (id);
}

/* Get the terminal id prefix for this server. */
static const char *
notify_prefix(void)
{
	static char	prefix[32];

	if (*prefix == '\0')
		xsnprintf(prefix, sizeof prefix, "tmux-%ld-", (long)getpid());
	return (prefix);
}

/* Get the number from a terminal id, 0 if it is not one of ours. */
static u_int
notify_tty_number(const char *id)
{
	const char	*prefix = notify_prefix();
	const char	*errstr;
	u_int		 n;

	if (id == NULL || strncmp(id, prefix, strlen(prefix)) != 0)
		return (0);
	n = strtonum(id + strlen(prefix), 1, UINT_MAX, &errstr);
	if (errstr != NULL)
		return (0);
	return (n);
}

/* Send a message to a pane. */
static void
notify_reply(struct window_pane *wp, const char *id, const char *type,
    const char *payload)
{
	char	*s;

	if (wp == NULL || wp->event == NULL)
		return;
	xasprintf(&s, "\033]99;i=%s%s%s;%s\033\\", id == NULL ? "0" : id,
	    type == NULL ? "" : ":p=", type == NULL ? "" : type, payload);
	log_debug("%s: %%%u: %s", __func__, wp->id, s);
	bufferevent_write(wp->event, s, strlen(s));
	free(s);
}

/* Send a message to a client's terminal. */
static void
notify_send(struct client *c, u_int id, const char *meta, const char *payload)
{
	char	*s;

	xasprintf(&s, "\033]99;i=%s%u%s;%s\033\\", notify_prefix(), id, meta,
	    payload);
	log_debug("%s: %s: %.200s", __func__, c->name, s);
	tty_write_noblock(&c->tty, s, strlen(s));
	free(s);
}

/* Get a client's state. */
static struct notify_client *
notify_get_client_state(struct client *c)
{
	if (c->notify == NULL) {
		c->notify = xcalloc(1, sizeof *c->notify);
		evtimer_set(&c->notify->alive_timer, notify_alive_timer, c);
	}
	return (c->notify);
}

/* Is this a client that can show notifications? */
static int
notify_client_ok(struct client *c)
{
	if (c->flags & (CLIENT_UNATTACHEDFLAGS|CLIENT_SUSPENDED))
		return (0);
	if (~c->tty.flags & TTY_STARTED)
		return (0);
	return ((c->tty.term->flags & TERM_KITTYNOTIFY) != 0);
}

/* Can a client show notifications from a pane? */
static int
notify_client_shows(struct client *c, struct window_pane *wp)
{
	if (!notify_client_ok(c))
		return (0);
	return (c->session != NULL && session_has(c->session, wp->window));
}

/*
 * Get the client for notifications from a pane: the most recently active
 * client attached to the window whose terminal supports the protocol.
 */
static struct client *
notify_get_client(struct window_pane *wp)
{
	struct client	*c, *found = NULL;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!notify_client_shows(c, wp))
			continue;
		if (found == NULL ||
		    timercmp(&c->activity_time, &found->activity_time, >))
			found = c;
	}
	return (found);
}

/* Free the data of a notification. */
static void
notify_free_data(struct notify *n)
{
	u_int	i;

	for (i = 0; i < NOTIFY_NTYPES; i++)
		free(n->data[i].data);
	memset(n->data, 0, sizeof n->data);
	n->size = 0;
}

/* Clear a notification for new content from the pane. */
static void
notify_reset(struct notify *n)
{
	u_int	i;

	notify_free_data(n);
	for (i = 0; i < NOTIFY_NKEYS; i++) {
		free(n->keys[i]);
		n->keys[i] = NULL;
	}
	free(n->occasion);
	n->occasion = NULL;
	n->flags = NOTIFY_PENDING|NOTIFY_FOCUS;
}

/* Free a notification. */
static void
notify_free(struct notify *n)
{
	TAILQ_REMOVE(&notify_list, n, entry);
	notify_reset(n);
	free(n->pane_id);
	free(n);
}

/* Find a notification by the id a pane gave. */
static struct notify *
notify_find_pane(u_int pane, const char *id)
{
	struct notify	*n;

	TAILQ_FOREACH(n, &notify_list, entry) {
		if (n->pane != pane)
			continue;
		if (id == NULL) {
			if (n->pane_id == NULL && (n->flags & NOTIFY_PENDING))
				return (n);
		} else if (n->pane_id != NULL && strcmp(n->pane_id, id) == 0)
			return (n);
	}
	return (NULL);
}

/* Find a notification by the id sent to a terminal. */
static struct notify *
notify_find_tty(struct client *c, const char *id)
{
	struct notify	*n;
	u_int		 number;

	if ((number = notify_tty_number(id)) == 0)
		return (NULL);
	TAILQ_FOREACH(n, &notify_list, entry) {
		if (n->id == number && n->client == c)
			return (n);
	}
	return (NULL);
}

/* Create a notification, dropping the oldest for the pane if too many. */
static struct notify *
notify_create(struct window_pane *wp, char *id)
{
	struct notify	*n, *oldest = NULL;
	u_int		 count = 0;

	TAILQ_FOREACH(n, &notify_list, entry) {
		if (n->pane != wp->id)
			continue;
		if (oldest == NULL)
			oldest = n;
		count++;
	}
	if (count >= NOTIFY_PANE_MAX)
		notify_free(oldest);

	n = xcalloc(1, sizeof *n);
	n->id = ++notify_next;
	if (n->id == 0)
		n->id = ++notify_next;
	n->pane = wp->id;
	n->pane_id = id;
	n->flags = NOTIFY_PENDING|NOTIFY_FOCUS;
	TAILQ_INSERT_TAIL(&notify_list, n, entry);
	return (n);
}

/* Decode a group of four base64 characters. */
static int
notify_decode_quantum(struct notify_data *nd)
{
	u_char	out[3];
	int	n;

	nd->quantum[4] = '\0';
	nd->quantum_len = 0;
	if ((n = b64_pton(nd->quantum, out, sizeof out)) == -1)
		return (-1);
	if (n == 0)
		return (0);
	nd->data = xrealloc(nd->data, nd->len + n);
	memcpy(nd->data + nd->len, out, n);
	nd->len += n;
	return (0);
}

/* Add base64 to data, which may be split anywhere between messages. */
static int
notify_add_base64(struct notify_data *nd, const char *s)
{
	for (; *s != '\0'; s++) {
		nd->quantum[nd->quantum_len++] = *s;
		if (nd->quantum_len == 4 && notify_decode_quantum(nd) != 0)
			return (-1);
	}
	return (0);
}

/* Decode base64 left at the end of data, which may not be padded. */
static int
notify_end_base64(struct notify_data *nd)
{
	if (nd->quantum_len == 0)
		return (0);
	while (nd->quantum_len != 4)
		nd->quantum[nd->quantum_len++] = '=';
	return (notify_decode_quantum(nd));
}

/* Apply the metadata in a message to a notification. */
static void
notify_apply(struct notify *n, struct notify_msg *msg)
{
	char	*copy, *cp, *action, *new;
	int	 flag;
	u_int	 i;

	if (msg->actions != NULL) {
		cp = copy = xstrdup(msg->actions);
		while ((action = strsep(&cp, ",")) != NULL) {
			if (strcmp(action + (*action == '-'), "focus") == 0)
				flag = NOTIFY_FOCUS;
			else if (strcmp(action + (*action == '-'), "report") == 0)
				flag = NOTIFY_REPORT;
			else
				continue;
			if (*action == '-')
				n->flags &= ~flag;
			else
				n->flags |= flag;
		}
		free(copy);
	}
	if (msg->close == 1)
		n->flags |= NOTIFY_CLOSE;
	else if (msg->close == 0)
		n->flags &= ~NOTIFY_CLOSE;
	if (msg->occasion != NULL &&
	    (strcmp(msg->occasion, "always") == 0 ||
	    strcmp(msg->occasion, "unfocused") == 0 ||
	    strcmp(msg->occasion, "invisible") == 0)) {
		free(n->occasion);
		n->occasion = xstrdup(msg->occasion);
	}
	for (i = 0; i < NOTIFY_NKEYS; i++) {
		if (msg->keys[i] == NULL)
			continue;
		if (n->keys[i] != NULL && strchr("nt", notify_keys[i]) != NULL) {
			xasprintf(&new, "%s:%c=%s", n->keys[i], notify_keys[i],
			    msg->keys[i]);
			free(n->keys[i]);
			n->keys[i] = new;
			continue;
		}
		free(n->keys[i]);
		n->keys[i] = msg->keys[i];
		msg->keys[i] = NULL;
	}
}

/* Keep icon data for a cache id or fill it in from what was kept. */
static void
notify_cache_icon(struct notify *n)
{
	struct notify_data	*nd = &n->data[NOTIFY_NTYPES - 1];
	struct notify_icon	*ni;
	const char		*cache_id;

	cache_id = n->keys[strchr(notify_keys, 'g') - notify_keys];
	if (cache_id == NULL)
		return;
	TAILQ_FOREACH(ni, &notify_icons, entry) {
		if (strcmp(ni->cache_id, cache_id) == 0)
			break;
	}

	if (nd->len == 0) {
		if (ni != NULL) {
			nd->data = xmalloc(ni->len);
			memcpy(nd->data, ni->data, ni->len);
			nd->len = ni->len;
		}
		return;
	}

	if (ni != NULL) {
		TAILQ_REMOVE(&notify_icons, ni, entry);
		free(ni->data);
	} else {
		if (notify_nicons == NOTIFY_ICONS_MAX) {
			ni = TAILQ_FIRST(&notify_icons);
			TAILQ_REMOVE(&notify_icons, ni, entry);
			free(ni->cache_id);
			free(ni->data);
			free(ni);
			notify_nicons--;
		}
		ni = xcalloc(1, sizeof *ni);
		ni->cache_id = xstrdup(cache_id);
		notify_nicons++;
	}
	ni->data = xmalloc(nd->len);
	memcpy(ni->data, nd->data, nd->len);
	ni->len = nd->len;
	TAILQ_INSERT_TAIL(&notify_icons, ni, entry);
}

/* Work out the occasion for a notification shown by a client. */
static const char *
notify_occasion(struct notify *n, struct client *c, struct window_pane *wp)
{
	struct window	*w = wp->window;

	if (n->occasion == NULL || strcmp(n->occasion, "always") == 0)
		return (NULL);
	if (c->session->curw->window != w || !window_pane_is_visible(wp))
		return ("always");
	if (strcmp(n->occasion, "unfocused") == 0 && wp != w->active)
		return ("always");
	return (n->occasion);
}

/* Send a finished notification to a client's terminal. */
static void
notify_send_notification(struct client *c, struct window_pane *wp,
    struct notify *n)
{
	struct notify_data	*nd;
	const char		*occasion;
	char			*meta, *new, *payload, *chunk;
	size_t			 off, size;
	u_int			 i, last = 0;

	xasprintf(&meta, ":e=1:a=%sfocus,report:c=1",
	    (n->flags & NOTIFY_FOCUS) ? "" : "-");
	if ((occasion = notify_occasion(n, c, wp)) != NULL) {
		xasprintf(&new, "%s:o=%s", meta, occasion);
		free(meta);
		meta = new;
	}
	for (i = 0; i < NOTIFY_NKEYS; i++) {
		if (n->keys[i] == NULL)
			continue;
		xasprintf(&new, "%s:%c=%s", meta, notify_keys[i], n->keys[i]);
		free(meta);
		meta = new;
	}

	for (i = 0; i < NOTIFY_NTYPES; i++) {
		if (n->data[i].len != 0)
			last = i;
	}
	for (i = 0; i <= last; i++) {
		nd = &n->data[i];
		if (nd->len == 0)
			continue;
		for (off = 0; off < nd->len; off += size) {
			size = nd->len - off;
			if (size > NOTIFY_CHUNK)
				size = NOTIFY_CHUNK;
			chunk = xmalloc(4 * ((size + 2) / 3) + 1);
			b64_ntop(nd->data + off, size, chunk,
			    4 * ((size + 2) / 3) + 1);
			xasprintf(&payload, "%s:p=%s%s", meta, notify_types[i],
			    (i == last && off + size == nd->len) ? "" : ":d=0");
			notify_send(c, n->id, payload, chunk);
			free(payload);
			free(chunk);
		}
	}
	free(meta);
}

/*
 * Drop an update that cannot be shown, keeping a notification already sent so
 * replies to it still work.
 */
static void
notify_drop_update(struct notify *n)
{
	if (n->client == NULL) {
		notify_free(n);
		return;
	}
	notify_reset(n);
	n->flags &= ~NOTIFY_PENDING;
}

/* A notification from a pane has all its chunks. */
static void
notify_finish(struct window_pane *wp, struct notify *n)
{
	struct client	*c;
	u_int		 i;

	n->flags &= ~NOTIFY_PENDING;
	for (i = 0; i < NOTIFY_NTYPES; i++) {
		if (notify_end_base64(&n->data[i]) != 0)
			n->flags |= NOTIFY_FAILED;
	}
	if (n->flags & NOTIFY_FAILED) {
		log_debug("%s: %%%u: dropping bad notification", __func__,
		    wp->id);
		notify_drop_update(n);
		return;
	}
	notify_cache_icon(n);

	if (n->data[0].len == 0 && n->data[1].len == 0) {
		log_debug("%s: %%%u: nothing to show", __func__, wp->id);
		notify_drop_update(n);
		return;
	}
	if ((c = n->client) == NULL || !notify_client_shows(c, wp))
		c = notify_get_client(wp);
	if (c == NULL) {
		log_debug("%s: %%%u: no terminal", __func__, wp->id);
		notify_free(n);
		return;
	}
	n->client = c;
	n->sent = ++notify_sends;
	n->actions = n->flags & (NOTIFY_FOCUS|NOTIFY_REPORT|NOTIFY_CLOSE);
	notify_send_notification(c, wp, n);
	notify_free_data(n);
}

/* Handle a chunk of a notification from a pane. */
static void
notify_pane_data(struct window_pane *wp, struct notify_msg *msg, u_int type)
{
	struct notify		*n;
	struct notify_data	*nd;
	char			*id;
	size_t			 len;
	u_int			 i;

	id = notify_pane_id(msg);
	if ((n = notify_find_pane(wp->id, id)) == NULL)
		n = notify_create(wp, id);
	else {
		free(id);
		if (~n->flags & NOTIFY_PENDING)
			notify_reset(n);
		TAILQ_REMOVE(&notify_list, n, entry);
		TAILQ_INSERT_TAIL(&notify_list, n, entry);
	}

	/* Count metadata too, repeated keys are kept from every chunk. */
	len = strlen(msg->payload);
	for (i = 0; i < NOTIFY_NKEYS; i++) {
		if (msg->keys[i] != NULL)
			len += strlen(msg->keys[i]) + 3;
	}
	if (~n->flags & NOTIFY_FAILED) {
		n->size += len;
		if (n->size > NOTIFY_MAX) {
			log_debug("%s: %%%u: notification too big", __func__,
			    wp->id);
			n->flags |= NOTIFY_FAILED;
		}
	}
	if (~n->flags & NOTIFY_FAILED) {
		notify_apply(n, msg);
		len = strlen(msg->payload);
		nd = &n->data[type];
		if (msg->base64) {
			if (notify_add_base64(nd, msg->payload) != 0)
				n->flags |= NOTIFY_FAILED;
		} else if (notify_end_base64(nd) != 0)
			n->flags |= NOTIFY_FAILED;
		else if (len != 0) {
			nd->data = xrealloc(nd->data, nd->len + len);
			memcpy(nd->data + nd->len, msg->payload, len);
			nd->len += len;
		}
		if (n->flags & NOTIFY_FAILED)
			notify_free_data(n);
	}
	if (msg->done)
		notify_finish(wp, n);
}

/* Handle a capabilities query from a pane. */
static void
notify_pane_query(struct window_pane *wp, struct notify_msg *msg)
{
	struct client	*c;
	const char	*caps = NOTIFY_DEFAULT_CAPS;
	char		*id;

	if ((c = notify_get_client(wp)) == NULL) {
		log_debug("%s: %%%u: no terminal", __func__, wp->id);
		return;
	}
	if (c->notify != NULL && c->notify->caps != NULL)
		caps = c->notify->caps;
	id = notify_pane_id(msg);
	notify_reply(wp, id, "?", caps);
	free(id);
}

/* Handle a close from a pane. */
static void
notify_pane_close(struct window_pane *wp, struct notify_msg *msg)
{
	struct notify	*n;
	char		*id;

	if ((id = notify_pane_id(msg)) == NULL)
		return;
	n = notify_find_pane(wp->id, id);
	free(id);
	if (n == NULL)
		return;
	if (n->client != NULL)
		notify_send(n->client, n->id, ":p=close", "");
	if (n->client == NULL || (~n->actions & NOTIFY_CLOSE))
		notify_free(n);
	else if (n->flags & NOTIFY_PENDING) {
		notify_free_data(n);
		n->flags &= ~(NOTIFY_PENDING|NOTIFY_FAILED);
	}
}

/* Free an alive query. */
static void
notify_free_alive(struct notify_alive *na)
{
	TAILQ_REMOVE(&notify_alive_list, na, entry);
	free(na->pane_id);
	free(na->list);
	free(na);
}

/* Reply to an alive query if every terminal has answered. */
static void
notify_check_alive(struct notify_alive *na)
{
	if (na->waiting != 0)
		return;
	notify_reply(window_pane_find_by_id(na->pane), na->pane_id, "alive",
	    na->list == NULL ? "" : na->list);
	notify_free_alive(na);
}

/* Forget an alive query sent to a client. */
static void
notify_remove_sent_alive(struct notify_client *nc, u_int i)
{
	memmove(&nc->alive[i], &nc->alive[i + 1],
	    (nc->nalive - i - 1) * sizeof *nc->alive);
	nc->nalive--;
}

/* Find an alive query from a pane. */
static struct notify_alive *
notify_find_alive(u_int id)
{
	struct notify_alive	*na;

	TAILQ_FOREACH(na, &notify_alive_list, entry) {
		if (na->id == id)
			return (na);
	}
	return (NULL);
}

/* Give up waiting for a client's reply to an alive query. */
static void
notify_drop_sent_alive(struct notify_client *nc, u_int i)
{
	struct notify_alive	*na = notify_find_alive(nc->alive[i].id);

	notify_remove_sent_alive(nc, i);
	if (na != NULL) {
		na->waiting--;
		notify_check_alive(na);
	}
}

/* Give up alive queries a client has not answered in time. */
static void
notify_alive_timer(__unused int fd, __unused short events, void *arg)
{
	struct client		*c = arg;
	struct notify_client	*nc = c->notify;
	struct timeval		 tv = { 0 };
	time_t			 t = time(NULL);

	while (nc->nalive != 0 &&
	    t - nc->alive[0].time >= NOTIFY_ALIVE_TIMEOUT) {
		log_debug("%s: %s: alive query timed out", __func__, c->name);
		notify_drop_sent_alive(nc, 0);
	}
	if (nc->nalive != 0) {
		tv.tv_sec = nc->alive[0].time + NOTIFY_ALIVE_TIMEOUT - t;
		evtimer_add(&nc->alive_timer, &tv);
	}
}

/* Handle an alive query from a pane. */
static void
notify_pane_alive(struct window_pane *wp, struct notify_msg *msg)
{
	struct notify_alive	*na;
	struct notify_client	*nc;
	struct notify		*n;
	struct client		*c;
	struct timeval		 tv = { .tv_sec = NOTIFY_ALIVE_TIMEOUT };

	na = xcalloc(1, sizeof *na);
	na->id = ++notify_next;
	if (na->id == 0)
		na->id = ++notify_next;
	na->pane = wp->id;
	na->pane_id = notify_pane_id(msg);
	TAILQ_INSERT_TAIL(&notify_alive_list, na, entry);

	TAILQ_FOREACH(c, &clients, entry) {
		TAILQ_FOREACH(n, &notify_list, entry) {
			if (n->pane == wp->id && n->client == c &&
			    n->pane_id != NULL)
				break;
		}
		if (n == NULL)
			continue;
		nc = notify_get_client_state(c);
		if (nc->nalive == NOTIFY_ALIVE_MAX) {
			log_debug("%s: %s: too many alive queries", __func__,
			    c->name);
			continue;
		}
		nc->alive = xreallocarray(nc->alive, nc->nalive + 1,
		    sizeof *nc->alive);
		nc->alive[nc->nalive].id = na->id;
		nc->alive[nc->nalive].time = time(NULL);
		nc->alive[nc->nalive++].sent = notify_sends;
		if (!evtimer_pending(&nc->alive_timer, NULL))
			evtimer_add(&nc->alive_timer, &tv);
		notify_send(c, na->id, ":p=alive", "");
		na->waiting++;
	}
	notify_check_alive(na);
}

/* Handle an OSC 99 message from a pane. */
void
notify_pane_message(struct window_pane *wp, const char *s)
{
	struct notify_msg	msg;
	u_int			i;

	notify_parse(&msg, s);
	if (strcmp(msg.type, "?") == 0)
		notify_pane_query(wp, &msg);
	else if (strcmp(msg.type, "alive") == 0)
		notify_pane_alive(wp, &msg);
	else if (strcmp(msg.type, "close") == 0)
		notify_pane_close(wp, &msg);
	else {
		for (i = 0; i < NOTIFY_NTYPES; i++) {
			if (strcmp(msg.type, notify_types[i]) == 0)
				break;
		}
		if (i != NOTIFY_NTYPES)
			notify_pane_data(wp, &msg, i);
		else {
			log_debug("%s: %%%u: unknown type %s", __func__,
			    wp->id, msg.type);
		}
	}
	notify_free_msg(&msg);
}

/* Handle a legacy OSC 9 notification, the text is the title. */
void
notify_pane_text(struct window_pane *wp, const char *text)
{
	char	*s;

	xasprintf(&s, ";%s", text);
	notify_pane_message(wp, s);
	free(s);
}

/* Free a pane's notifications. */
void
notify_pane_destroy(struct window_pane *wp)
{
	struct notify		*n, *n1;
	struct notify_alive	*na, *na1;

	TAILQ_FOREACH_SAFE(n, &notify_list, entry, n1) {
		if (n->pane == wp->id)
			notify_free(n);
	}
	TAILQ_FOREACH_SAFE(na, &notify_alive_list, entry, na1) {
		if (na->pane == wp->id)
			notify_free_alive(na);
	}
}

/* Make a pane the active pane in a client. */
static void
notify_focus(struct client *c, struct window_pane *wp)
{
	struct window	*w = wp->window;
	struct session	*s = c->session;
	struct winlink	*wl;

	if (s == NULL || !session_has(s, w)) {
		RB_FOREACH(s, sessions, &sessions) {
			if (session_has(s, w))
				break;
		}
		if (s == NULL)
			return;
	}
	wl = winlink_find_by_window(&s->windows, w);
	log_debug("%s: %s: focusing %%%u", __func__, c->name, wp->id);

	if (wp != w->active && (w->modal == NULL || wp == w->modal)) {
		if (!window_pane_is_visible(wp))
			window_unzoom(w, 1);
		window_redraw_active_switch(w, wp);
		window_set_active_pane(w, wp, 1);
		server_redraw_window(w);
	}
	if (s != c->session) {
		session_set_current(s, wl);
		server_client_set_session(c, s);
	} else if (session_set_current(s, wl) == 0) {
		server_redraw_session(s);
		w->latest = c;
		recalculate_sizes();
	}
}

/* Handle an alive reply from a terminal. */
static void
notify_tty_alive(struct client *c, struct notify_msg *msg)
{
	struct notify_client	*nc = notify_get_client_state(c);
	struct notify_alive	*na;
	struct notify		*n, *n1;
	char			*copy, *cp, *id, *new;
	u_int			 number, sent, i;

	number = notify_tty_number(msg->id);
	for (i = 0; i < nc->nalive; i++) {
		if (nc->alive[i].id == number)
			break;
	}
	if (number == 0 || i == nc->nalive) {
		log_debug("%s: %s: unknown id %s", __func__, c->name, msg->id);
		return;
	}
	sent = nc->alive[i].sent;
	notify_remove_sent_alive(nc, i);
	na = notify_find_alive(number);

	/* Forget notifications sent before the query that are not live. */
	TAILQ_FOREACH(n, &notify_list, entry) {
		if (n->client == c && (~n->flags & NOTIFY_PENDING) &&
		    n->sent <= sent)
			n->flags |= NOTIFY_STALE;
	}
	cp = copy = xstrdup(msg->payload);
	while ((id = strsep(&cp, ",")) != NULL) {
		if ((n = notify_find_tty(c, id)) == NULL)
			continue;
		n->flags &= ~NOTIFY_STALE;
		if (na == NULL || n->pane != na->pane || n->pane_id == NULL)
			continue;
		if (na->list == NULL)
			na->list = xstrdup(n->pane_id);
		else {
			xasprintf(&new, "%s,%s", na->list, n->pane_id);
			free(na->list);
			na->list = new;
		}
	}
	free(copy);
	TAILQ_FOREACH_SAFE(n, &notify_list, entry, n1) {
		if (n->flags & NOTIFY_STALE)
			notify_free(n);
	}

	if (na != NULL) {
		na->waiting--;
		notify_check_alive(na);
	}
}

/* Handle the capabilities reply from a terminal. */
static void
notify_tty_query(struct client *c, struct notify_msg *msg)
{
	struct notify_client	*nc = notify_get_client_state(c);
	struct tty		*tty = &c->tty;

	free(nc->caps);
	nc->caps = notify_copy(msg->payload, "=:,-_+./? ");
	log_debug("%s: %s: capabilities %s", __func__, c->name, nc->caps);
	if (~tty->term->flags & TERM_KITTYNOTIFY) {
		tty_parse_client_features(c, "kittynotify", ",");
		tty_update_features(tty);
	}
}

/* Handle an OSC 99 message from a client's terminal. */
void
notify_tty_message(struct client *c, const char *buf, size_t len)
{
	struct notify_msg	 msg;
	struct notify		*n;
	struct window_pane	*wp;
	char			*copy, *payload;

	copy = xstrndup(buf, len);
	notify_parse(&msg, copy);
	free(copy);

	if (strcmp(msg.type, "?") == 0) {
		notify_tty_query(c, &msg);
		goto out;
	}
	if (strcmp(msg.type, "alive") == 0) {
		notify_tty_alive(c, &msg);
		goto out;
	}
	if ((n = notify_find_tty(c, msg.id)) == NULL) {
		log_debug("%s: %s: unknown id %s", __func__, c->name,
		    msg.id == NULL ? "" : msg.id);
		goto out;
	}
	wp = window_pane_find_by_id(n->pane);

	payload = notify_copy(msg.payload, "");
	if (strcmp(msg.type, "close") == 0) {
		if (n->actions & NOTIFY_CLOSE)
			notify_reply(wp, n->pane_id, "close", payload);
		if (strcmp(payload, "untracked") != 0 &&
		    (~n->flags & NOTIFY_PENDING))
			notify_free(n);
	} else if (strcmp(msg.type, "title") == 0) {
		if (wp != NULL && (~c->flags & CLIENT_READONLY)) {
			if (n->actions & NOTIFY_FOCUS)
				notify_focus(c, wp);
			if (n->actions & NOTIFY_REPORT)
				notify_reply(wp, n->pane_id, NULL, payload);
		}
	} else
		log_debug("%s: %s: ignoring type %s", __func__, c->name,
		    msg.type);
	free(payload);

out:
	notify_free_msg(&msg);
}

/* Ask a client's terminal for its capabilities if not done already. */
void
notify_client_start(struct client *c)
{
	struct notify_client	*nc = notify_get_client_state(c);

	if (nc->queried)
		return;
	nc->queried = 1;
	tty_puts(&c->tty, "\033]99;i=tmux:p=?;\033\\");
}

/* Is the client waiting for an alive reply? */
int
notify_client_active(struct client *c)
{
	struct notify_client	*nc = c->notify;

	return (nc != NULL && nc->nalive != 0);
}

/* Forget a client's notifications and finish its alive queries. */
void
notify_client_stop(struct client *c)
{
	struct notify_client	*nc = c->notify;
	struct notify		*n, *n1;

	TAILQ_FOREACH_SAFE(n, &notify_list, entry, n1) {
		if (n->client != c)
			continue;
		if (n->flags & NOTIFY_PENDING)
			n->client = NULL;
		else
			notify_free(n);
	}
	if (nc == NULL)
		return;
	evtimer_del(&nc->alive_timer);
	while (nc->nalive != 0)
		notify_drop_sent_alive(nc, 0);
	free(nc->alive);
	nc->alive = NULL;
	nc->nalive = 0;
	nc->queried = 0;
}

/* Free a client's state. */
void
notify_client_free(struct client *c)
{
	notify_client_stop(c);
	if (c->notify != NULL) {
		free(c->notify->caps);
		free(c->notify);
		c->notify = NULL;
	}
}
