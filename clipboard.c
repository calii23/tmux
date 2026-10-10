/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Lucy Schelbach <lucy.schelbach@procure.ai>
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

#include "tmux.h"

/*
 * Kitty clipboard protocol (OSC 5522), described at:
 *
 *     https://sw.kovidgoyal.net/kitty/clipboard/
 *
 * Every message has the form:
 *
 *     OSC 5522 ; key=value:key=value ; payload ST
 *
 * tmux is the terminal for the programs in its panes and a program to the
 * outside terminal:
 *
 * - Writes. tmux collects the whole write itself, puts any plain text into a
 *   paste buffer and sends the write on to each client terminal showing the
 *   pane (as OSC 52 text if the terminal does not have the protocol). This
 *   follows the set-clipboard option. The reply to the pane comes from the
 *   terminal reads would go to, or is DONE straight away without one. Writes
 *   are sent to that terminal one at a time, since it may ask the user.
 *
 * - Reads. Unless get-clipboard is off, tmux sends the read to the most
 *   recently used client terminal with the protocol, which asks the user for
 *   permission itself, and passes the replies back. Only one read per client
 *   is sent at a time. Without such a terminal, tmux answers with the newest
 *   paste buffer.
 *
 * - Paste events. Mode 5522 of the active pane is set on the terminal, and
 *   unsolicited MIME lists from the terminal go to the active pane. A read
 *   with the one time password from the event always goes to the terminal.
 */

/* Largest data in one message sent, before base64. */
#define CLIPBOARD_CHUNK 4096

/* Largest write tmux will take, the minimum from the specification. */
#define CLIPBOARD_MAX (64 * 1024 * 1024)

/* Most MIME types and bytes of MIME types and aliases in one write. */
#define CLIPBOARD_TYPES_MAX 1024
#define CLIPBOARD_NAMES_MAX (1024 * 1024)

/* Most writes waiting for a terminal's reply for each client. */
#define CLIPBOARD_WRITES_MAX 16

/* Time before a read sent to a terminal may be given up. */
#define CLIPBOARD_READ_TIMEOUT 60

/* A parsed message. Values are pointers into the copy. */
struct clipboard_msg {
	char	*copy;
	char	*type;
	char	*status;
	char	*mime;
	char	*id;
	char	*name;
	char	*pw;
	char	*loc;
	char	*payload;
};

/* Data for one MIME type in a write. */
struct clipboard_data {
	char				*mime;		/* decoded MIME type */
	char				*aliases;	/* decoded alias list */
	size_t				 aliases_len;
	size_t				 aliases_size;
	int				 written;	/* wdata was sent */
	char				*data;
	size_t				 len;
	char				 quantum[4];	/* base64 not yet decoded */
	size_t				 quantum_len;
	TAILQ_ENTRY(clipboard_data)	 entry;
};
TAILQ_HEAD(clipboard_data_list, clipboard_data);

/* Write in progress for a pane. */
struct clipboard_pane {
	int				 flags;
#define CLIPBOARD_WRITING 0x1
#define CLIPBOARD_FAILED 0x2

	char				*id;
	char				*name;
	char				*pw;
	int				 primary;
	size_t				 size;
	u_int				 types;
	size_t				 names;		/* bytes of names */
	struct clipboard_data_list	 data;
};

/*
 * Write for a client's terminal. Only the first is sent, the rest wait for its
 * reply. The reply goes to the pane only from one terminal.
 */
struct clipboard_write {
	int				 pane;		/* -1 if destroyed */
	int				 reply;		/* reply to the pane */
	char				*id;		/* id the pane gave */
	char				*tty_id;	/* terminal id */
	struct evbuffer			*out;		/* NULL once sent */
	size_t				 size;
	TAILQ_ENTRY(clipboard_write)	 entry;
};
TAILQ_HEAD(clipboard_write_list, clipboard_write);

/* Read, write and paste event state for a client. */
struct clipboard_client {
	u_int		 next;		/* number for the next id */

	int		 read_pane;	/* pane waiting for a read, -1 if none */
	char		*read_id;	/* id the pane gave */
	char		*read_tty_id;	/* id sent to the terminal */
	time_t		 read_time;
	int		 read_started;	/* OK has been passed on */
	int		 read_buffer;	/* create a paste buffer */
	int		 read_osc52;	/* reply to the pane with OSC 52 */
	char		 read_clip;	/* OSC 52 selection */
	const char	*read_end;	/* OSC 52 terminator */
	int		 read_text;	/* current data is plain text */
	int		 read_big;	/* plain text was too big */
	char		*text_mime;	/* MIME type of the plain text */
	char		*text;
	size_t		 text_len;

	int		 paste_pane;	/* pane getting a paste event */
	char		*paste_pw;	/* password from the last paste event */
	int		 paste_primary;

	struct clipboard_write_list writes;
	u_int		 nwrites;
	size_t		 queued;	/* bytes of writes not sent */
};

/* Is this a valid base64 string with correct padding? */
static int
clipboard_valid(const char *s, size_t len)
{
	size_t	i, pad = 0;

	if (len % 4 != 0)
		return (0);
	for (i = 0; i < len; i++) {
		if (s[i] == '=') {
			if (i + 2 < len)
				return (0);
			pad++;
			continue;
		}
		if (pad != 0)
			return (0);
		if (!isalnum((u_char)s[i]) && s[i] != '+' && s[i] != '/')
			return (0);
	}
	return (1);
}

/* Decode a base64 string. Returns NULL if it is invalid. */
static char *
clipboard_decode(const char *s, size_t *outlen)
{
	size_t	 len = strlen(s), size;
	char	*out;
	int	 n;

	if (!clipboard_valid(s, len))
		return (NULL);
	size = (len / 4) * 3 + 1;
	out = xmalloc(size);
	if ((n = b64_pton(s, out, size)) == -1) {
		free(out);
		return (NULL);
	}
	out[n] = '\0';
	if (outlen != NULL)
		*outlen = n;
	return (out);
}

/* Encode as base64. */
static char *
clipboard_encode(const void *buf, size_t len)
{
	size_t	 size = 4 * ((len + 2) / 3) + 1;
	char	*out;

	out = xmalloc(size);
	if (b64_ntop(buf, len, out, size) == -1)
		out[0] = '\0';
	return (out);
}

/* Value of a base64 character, or -1. */
static int
clipboard_value(u_char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return (ch - 'A');
	if (ch >= 'a' && ch <= 'z')
		return (ch - 'a' + 26);
	if (ch >= '0' && ch <= '9')
		return (ch - '0' + 52);
	if (ch == '+')
		return (62);
	if (ch == '/')
		return (63);
	return (-1);
}

/*
 * Decode base64 for one MIME type of a write, which may be split anywhere
 * between messages. Each group of four characters may be padded, since
 * programs often pad every message. Returns -1 if invalid.
 */
static int
clipboard_add_data(struct clipboard_data *cd, const char *s, size_t len)
{
	const char	*q = cd->quantum;
	size_t		 size;
	int		 v[4], n, i;

	size = cd->len + (cd->quantum_len + len) / 4 * 3;
	if (size > cd->len)
		cd->data = xrealloc(cd->data, size);
	for (; len != 0; s++, len--) {
		cd->quantum[cd->quantum_len++] = *s;
		if (cd->quantum_len != 4)
			continue;
		cd->quantum_len = 0;

		n = 3;
		if (q[3] == '=') {
			n = 2;
			if (q[2] == '=')
				n = 1;
		}
		for (i = 0; i < 4; i++) {
			v[i] = (i <= n ? clipboard_value(q[i]) : 0);
			if (v[i] == -1)
				return (-1);
		}
		cd->data[cd->len++] = (v[0] << 2) | (v[1] >> 4);
		if (n > 1)
			cd->data[cd->len++] = (v[1] << 4) | (v[2] >> 2);
		if (n > 2)
			cd->data[cd->len++] = (v[2] << 6) | v[3];
	}
	return (0);
}

/* Decode a MIME type. Returns NULL if invalid. */
static char *
clipboard_decode_mime(const char *s)
{
	char	*mime, *cp;
	int	 empty = 1;

	if (s == NULL || (mime = clipboard_decode(s, NULL)) == NULL)
		return (NULL);
	for (cp = mime; *cp != '\0'; cp++) {
		if (!isprint((u_char)*cp))
			break;
		if (*cp != ' ')
			empty = 0;
	}
	if (empty || *cp != '\0') {
		free(mime);
		return (NULL);
	}
	return (mime);
}

/* Is this a plain text MIME type? */
static int
clipboard_is_text(const char *mime)
{
	return (strcmp(mime, "text/plain") == 0 ||
	    strncmp(mime, "text/plain;", 11) == 0);
}

/* Does a whitespace separated list contain plain text? */
static int
clipboard_list_has_text(const char *list)
{
	char	*copy, *cp, *mime;
	int	 found = 0;

	cp = copy = xstrdup(list);
	while (!found && (mime = strsep(&cp, " \t\r\n")) != NULL)
		found = clipboard_is_text(mime);
	free(copy);
	return (found);
}

/* Copy an id or status leaving out characters that are not allowed. */
static char *
clipboard_copy_id(const char *id)
{
	char	*copy, *cp;

	if (id == NULL)
		return (NULL);
	cp = copy = xmalloc(strlen(id) + 1);
	for (; *id != '\0'; id++) {
		if (isalnum((u_char)*id) || strchr("-_+.", *id) != NULL)
			*cp++ = *id;
	}
	*cp = '\0';
	return (copy);
}

/* Parse a message, starting after the "5522;". Returns -1 if invalid. */
static int
clipboard_parse(struct clipboard_msg *msg, const char *s)
{
	char	*meta, *pair, *value;

	memset(msg, 0, sizeof *msg);
	msg->copy = xstrdup(s);

	meta = msg->copy;
	if ((msg->payload = strchr(meta, ';')) != NULL)
		*msg->payload++ = '\0';
	else
		msg->payload = meta + strlen(meta);

	while ((pair = strsep(&meta, ":")) != NULL) {
		if (*pair == '\0')
			continue;
		if ((value = strchr(pair, '=')) == NULL)
			goto fail;
		*value++ = '\0';
		if (strcmp(pair, "type") == 0)
			msg->type = value;
		else if (strcmp(pair, "status") == 0)
			msg->status = value;
		else if (strcmp(pair, "mime") == 0)
			msg->mime = value;
		else if (strcmp(pair, "id") == 0)
			msg->id = value;
		else if (strcmp(pair, "name") == 0)
			msg->name = value;
		else if (strcmp(pair, "pw") == 0)
			msg->pw = value;
		else if (strcmp(pair, "loc") == 0)
			msg->loc = value;
		else
			log_debug("%s: unknown key %s", __func__, pair);
	}
	if (msg->type == NULL)
		goto fail;

	/* Drop names and passwords that would not be valid to pass on. */
	if (msg->name != NULL &&
	    !clipboard_valid(msg->name, strlen(msg->name)))
		msg->name = NULL;
	if (msg->pw != NULL && !clipboard_valid(msg->pw, strlen(msg->pw)))
		msg->pw = NULL;
	return (0);

fail:
	free(msg->copy);
	return (-1);
}

/*
 * Drop a password without a name from a request, the specification treats it
 * as no password.
 */
static void
clipboard_check_password(struct clipboard_msg *msg)
{
	if (msg->name == NULL || msg->pw == NULL)
		msg->name = msg->pw = NULL;
}

/* Is this message for the primary selection? */
static int
clipboard_primary(const struct clipboard_msg *msg)
{
	return (msg->loc != NULL && strcmp(msg->loc, "primary") == 0);
}

/* Add a key to a message being built. */
static void
clipboard_build_key(char **s, const char *key, const char *value)
{
	char	*new;

	if (value == NULL)
		return;
	xasprintf(&new, "%s:%s=%s", *s, key, value);
	free(*s);
	*s = new;
}

/* Build a message. */
static char *
clipboard_build(const struct clipboard_msg *msg)
{
	char	*s, *new;

	xasprintf(&s, "\033]5522;type=%s", msg->type);
	clipboard_build_key(&s, "status", msg->status);
	clipboard_build_key(&s, "mime", msg->mime);
	clipboard_build_key(&s, "id", msg->id);
	clipboard_build_key(&s, "name", msg->name);
	clipboard_build_key(&s, "pw", msg->pw);
	clipboard_build_key(&s, "loc", msg->loc);
	if (msg->payload != NULL && *msg->payload != '\0')
		xasprintf(&new, "%s;%s\033\\", s, msg->payload);
	else
		xasprintf(&new, "%s\033\\", s);
	free(s);
	return (new);
}

/* Send a message to a pane. */
static void
clipboard_send_pane(struct window_pane *wp, const struct clipboard_msg *msg)
{
	char	*s;

	if (wp == NULL || wp->event == NULL)
		return;
	s = clipboard_build(msg);
	log_debug("%s: %%%u: %s", __func__, wp->id, s);
	bufferevent_write(wp->event, s, strlen(s));
	free(s);
}

/* Send a message to a client's terminal. */
static void
clipboard_send_client(struct client *c, const struct clipboard_msg *msg)
{
	char	*s;

	s = clipboard_build(msg);
	log_debug("%s: %s: %.200s", __func__, c->name, s);
	tty_write_noblock(&c->tty, s, strlen(s));
	free(s);
}

/* Add a message to a buffer for a client's terminal. */
static void
clipboard_add_client(struct evbuffer *evb, const struct clipboard_msg *msg)
{
	char	*s;

	s = clipboard_build(msg);
	evbuffer_add(evb, s, strlen(s));
	free(s);
}

/* Send a buffer to a client's terminal. */
static void
clipboard_send_buffer(struct client *c, struct evbuffer *evb)
{
	log_debug("%s: %s: %zu bytes", __func__, c->name, EVBUFFER_LENGTH(evb));
	tty_write_noblock(&c->tty, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
}

/* Send a status to a pane. */
static void
clipboard_reply(struct window_pane *wp, const char *type, const char *status,
    const char *id)
{
	struct clipboard_msg	msg;

	memset(&msg, 0, sizeof msg);
	msg.type = (char *)type;
	msg.status = (char *)status;
	msg.id = (char *)id;
	clipboard_send_pane(wp, &msg);
}

/* Send data for a MIME type in chunks. */
static void
clipboard_send_data(struct window_pane *wp, struct evbuffer *evb,
    struct clipboard_msg *msg, const char *data, size_t len)
{
	size_t	 off = 0, n;

	do {
		n = len - off;
		if (n > CLIPBOARD_CHUNK)
			n = CLIPBOARD_CHUNK;
		msg->payload = clipboard_encode(data + off, n);
		if (wp != NULL)
			clipboard_send_pane(wp, msg);
		else
			clipboard_add_client(evb, msg);
		free(msg->payload);
		off += n;
	} while (off != len);
	msg->payload = NULL;
}

/* Get a pane's state. */
static struct clipboard_pane *
clipboard_get_pane(struct window_pane *wp)
{
	struct clipboard_pane	*cp = wp->clipboard;

	if (cp == NULL) {
		cp = wp->clipboard = xcalloc(1, sizeof *cp);
		TAILQ_INIT(&cp->data);
	}
	return (cp);
}

/* Get a client's state. */
static struct clipboard_client *
clipboard_get_client_state(struct client *c)
{
	struct clipboard_client	*cc = c->clipboard;

	if (cc == NULL) {
		cc = c->clipboard = xcalloc(1, sizeof *cc);
		cc->read_pane = -1;
		cc->paste_pane = -1;
		TAILQ_INIT(&cc->writes);
	}
	return (cc);
}

/* Forget a write waiting for a terminal's reply. */
static void
clipboard_free_write(struct clipboard_client *cc, struct clipboard_write *cw)
{
	TAILQ_REMOVE(&cc->writes, cw, entry);
	cc->nwrites--;
	if (cw->out != NULL) {
		cc->queued -= cw->size;
		evbuffer_free(cw->out);
	}
	free(cw->id);
	free(cw->tty_id);
	free(cw);
}

/* Send the first write for a client's terminal if it is waiting. */
static void
clipboard_next_write(struct client *c, struct clipboard_client *cc)
{
	struct clipboard_write	*cw = TAILQ_FIRST(&cc->writes);

	if (cw == NULL || cw->out == NULL)
		return;
	clipboard_send_buffer(c, cw->out);
	evbuffer_free(cw->out);
	cw->out = NULL;
	cc->queued -= cw->size;
}

/* Is a client's queue of writes too full for another? */
static int
clipboard_writes_full(struct clipboard_client *cc, size_t size)
{
	if (cc->nwrites >= CLIPBOARD_WRITES_MAX)
		return (1);
	return (cc->nwrites != 0 && cc->queued + size > CLIPBOARD_MAX);
}

/*
 * Forget writes from a pane. One already sent stays until the terminal
 * replies so the next is not sent before.
 */
static void
clipboard_drop_writes(struct window_pane *wp)
{
	struct client		*c;
	struct clipboard_client	*cc;
	struct clipboard_write	*cw, *cw1;

	TAILQ_FOREACH(c, &clients, entry) {
		if ((cc = c->clipboard) == NULL)
			continue;
		TAILQ_FOREACH_SAFE(cw, &cc->writes, entry, cw1) {
			if (cw->pane != (int)wp->id)
				continue;
			if (cw->out == NULL)
				cw->pane = -1;
			else
				clipboard_free_write(cc, cw);
		}
	}
}

/* Is this a client that can carry the protocol? */
static int
clipboard_client_ok(struct client *c)
{
	if (c->flags & (CLIENT_UNATTACHEDFLAGS|CLIENT_SUSPENDED))
		return (0);
	if (~c->tty.flags & TTY_STARTED)
		return (0);
	return (1);
}

/*
 * Get the client for reads from a pane: the most recently active client
 * showing the window whose terminal supports the protocol.
 */
static struct client *
clipboard_get_client(struct window_pane *wp)
{
	struct client	*c, *found = NULL;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!clipboard_client_ok(c))
			continue;
		if (c->session == NULL || !session_has(c->session, wp->window))
			continue;
		if (~c->tty.term->flags & TERM_KITTYCLIPBOARD)
			continue;
		if (found == NULL ||
		    timercmp(&c->activity_time, &found->activity_time, >))
			found = c;
	}
	return (found);
}

/* Find the client whose paste event gave the password in a read. */
static struct client *
clipboard_get_paste_client(struct window_pane *wp, struct clipboard_msg *msg)
{
	struct client		*c;
	struct clipboard_client	*cc;

	if (msg->pw == NULL)
		return (NULL);
	TAILQ_FOREACH(c, &clients, entry) {
		if ((cc = c->clipboard) == NULL || cc->paste_pw == NULL)
			continue;
		if (!clipboard_client_ok(c))
			continue;
		if (c->session == NULL || !session_has(c->session, wp->window))
			continue;
		if (~c->tty.term->flags & TERM_KITTYCLIPBOARD)
			continue;
		if (strcmp(cc->paste_pw, msg->pw) == 0 &&
		    cc->paste_primary == clipboard_primary(msg))
			return (c);
	}
	return (NULL);
}

/*
 * Find the data for a MIME type in a write, adding it if missing. Returns NULL
 * if there are too many types.
 */
static struct clipboard_data *
clipboard_find_data(struct clipboard_pane *cp, char *mime)
{
	struct clipboard_data	*cd;
	size_t			 len = strlen(mime);

	TAILQ_FOREACH(cd, &cp->data, entry) {
		if (strcmp(cd->mime, mime) == 0) {
			free(mime);
			return (cd);
		}
	}
	if (cp->types == CLIPBOARD_TYPES_MAX ||
	    cp->names + len > CLIPBOARD_NAMES_MAX) {
		free(mime);
		return (NULL);
	}
	cp->types++;
	cp->names += len;

	cd = xcalloc(1, sizeof *cd);
	cd->mime = mime;
	TAILQ_INSERT_TAIL(&cp->data, cd, entry);
	return (cd);
}

/* Throw away a pane's write. */
static void
clipboard_reset_write(struct clipboard_pane *cp)
{
	struct clipboard_data	*cd, *cd1;

	TAILQ_FOREACH_SAFE(cd, &cp->data, entry, cd1) {
		TAILQ_REMOVE(&cp->data, cd, entry);
		free(cd->mime);
		free(cd->aliases);
		free(cd->data);
		free(cd);
	}
	free(cp->id);
	free(cp->name);
	free(cp->pw);
	cp->id = cp->name = cp->pw = NULL;
	cp->flags = 0;
	cp->primary = 0;
	cp->size = 0;
	cp->types = 0;
	cp->names = 0;
}

/* Fail a pane's write and ignore it until the next one starts. */
static void
clipboard_fail_write(struct window_pane *wp, struct clipboard_pane *cp,
    const char *status)
{
	log_debug("%s: %%%u: %s", __func__, wp->id, status);
	clipboard_reply(wp, "write", status, cp->id);
	clipboard_reset_write(cp);
	cp->flags = CLIPBOARD_FAILED;
}

/* Build a complete write for a client's terminal. */
static struct evbuffer *
clipboard_build_write(struct clipboard_pane *cp, const char *id)
{
	struct evbuffer		*evb;
	struct clipboard_data	*cd;
	struct clipboard_msg	 msg;

	if ((evb = evbuffer_new()) == NULL)
		fatalx("out of memory");

	memset(&msg, 0, sizeof msg);
	msg.type = (char *)"write";
	msg.id = (char *)id;
	msg.name = cp->name;
	msg.pw = cp->pw;
	if (cp->primary)
		msg.loc = (char *)"primary";
	clipboard_add_client(evb, &msg);

	memset(&msg, 0, sizeof msg);
	TAILQ_FOREACH(cd, &cp->data, entry) {
		msg.mime = clipboard_encode(cd->mime, strlen(cd->mime));
		if (cd->written) {
			msg.type = (char *)"wdata";
			clipboard_send_data(NULL, evb, &msg, cd->data, cd->len);
		}
		if (cd->aliases != NULL) {
			msg.type = (char *)"walias";
			msg.payload = clipboard_encode(cd->aliases,
			    strlen(cd->aliases));
			clipboard_add_client(evb, &msg);
			free(msg.payload);
			msg.payload = NULL;
		}
		free(msg.mime);
	}
	msg.type = (char *)"wdata";
	msg.mime = NULL;
	clipboard_add_client(evb, &msg);
	return (evb);
}

/* Find the plain text in a write and decode it. */
static char *
clipboard_find_text(struct clipboard_pane *cp, size_t *len)
{
	struct clipboard_data	*cd, *found = NULL;
	char			*text;

	TAILQ_FOREACH(cd, &cp->data, entry) {
		if (cd->len != 0 && clipboard_is_text(cd->mime)) {
			found = cd;
			break;
		}
	}
	TAILQ_FOREACH(cd, &cp->data, entry) {
		if (found != NULL)
			break;
		if (cd->len != 0 && cd->aliases != NULL &&
		    clipboard_list_has_text(cd->aliases))
			found = cd;
	}
	if (found == NULL)
		return (NULL);
	text = xmalloc(found->len);
	memcpy(text, found->data, found->len);
	*len = found->len;
	return (text);
}

/* Finish a pane's write: check it and pass it on. */
static void
clipboard_finish_write(struct window_pane *wp, struct clipboard_pane *cp)
{
	struct clipboard_data	*cd;
	struct client		*c, *reply;
	struct clipboard_client	*cc;
	struct clipboard_write	*cw;
	char			*text;
	size_t			 len = 0;

	TAILQ_FOREACH(cd, &cp->data, entry) {
		if (cd->quantum_len != 0) {
			clipboard_fail_write(wp, cp, "EINVAL");
			return;
		}
	}

	/* The reply to the pane comes from the same terminal as reads. */
	reply = clipboard_get_client(wp);
	if (reply != NULL) {
		cc = clipboard_get_client_state(reply);
		if (clipboard_writes_full(cc, cp->size)) {
			clipboard_fail_write(wp, cp, "EBUSY");
			return;
		}
	}
	text = clipboard_find_text(cp, &len);
	TAILQ_FOREACH(c, &clients, entry) {
		if (!clipboard_client_ok(c))
			continue;
		if (c->session == NULL || !session_has(c->session, wp->window))
			continue;
		if (c->tty.term->flags & TERM_KITTYCLIPBOARD) {
			cc = clipboard_get_client_state(c);
			if (clipboard_writes_full(cc, cp->size)) {
				log_debug("%s: %s: too many writes", __func__,
				    c->name);
				continue;
			}
			cw = xcalloc(1, sizeof *cw);
			cw->pane = wp->id;
			cw->reply = (c == reply);
			cw->id = (cp->id == NULL ? NULL : xstrdup(cp->id));
			xasprintf(&cw->tty_id, "tmux-w%u", cc->next++);
			cw->out = clipboard_build_write(cp, cw->tty_id);
			cw->size = cp->size;
			TAILQ_INSERT_TAIL(&cc->writes, cw, entry);
			cc->nwrites++;
			cc->queued += cw->size;
			clipboard_next_write(c, cc);
		} else if (text != NULL)
			tty_set_selection(&c->tty, cp->primary ? "p" : "", text,
			    len);
	}
	if (text != NULL)
		paste_add(NULL, text, len);
	events_fire_pane("pane-set-clipboard", wp);

	if (reply == NULL)
		clipboard_reply(wp, "write", "DONE", cp->id);
	clipboard_reset_write(cp);
}

/* Handle the start of a write from a pane. */
static void
clipboard_pane_write(struct window_pane *wp, struct clipboard_msg *msg)
{
	struct clipboard_pane	*cp = clipboard_get_pane(wp);

	clipboard_reset_write(cp);
	cp->id = clipboard_copy_id(msg->id);
	if (options_get_number(global_options, "set-clipboard") != 2) {
		clipboard_fail_write(wp, cp, "EPERM");
		return;
	}
	if (msg->name != NULL) {
		cp->name = xstrdup(msg->name);
		cp->pw = xstrdup(msg->pw);
	}
	cp->primary = clipboard_primary(msg);
	cp->flags = CLIPBOARD_WRITING;
}

/* Handle a data or end of data message for a write from a pane. */
static void
clipboard_pane_wdata(struct window_pane *wp, struct clipboard_msg *msg)
{
	struct clipboard_pane	*cp = clipboard_get_pane(wp);
	struct clipboard_data	*cd;
	char			*mime;
	size_t			 len = strlen(msg->payload);

	if (~cp->flags & CLIPBOARD_WRITING)
		return;
	if (msg->mime == NULL) {
		if (len != 0)
			clipboard_fail_write(wp, cp, "EINVAL");
		else
			clipboard_finish_write(wp, cp);
		return;
	}
	if ((mime = clipboard_decode_mime(msg->mime)) == NULL) {
		clipboard_fail_write(wp, cp, "EINVAL");
		return;
	}
	if ((cd = clipboard_find_data(cp, mime)) == NULL) {
		clipboard_fail_write(wp, cp, "EFBIG");
		return;
	}
	cd->written = 1;
	cp->size -= cd->len;
	if (clipboard_add_data(cd, msg->payload, len) != 0) {
		clipboard_fail_write(wp, cp, "EINVAL");
		return;
	}
	cp->size += cd->len;
	if (cp->size > CLIPBOARD_MAX)
		clipboard_fail_write(wp, cp, "EFBIG");
}

/* Handle an alias message for a write from a pane. */
static void
clipboard_pane_walias(struct window_pane *wp, struct clipboard_msg *msg)
{
	struct clipboard_pane	*cp = clipboard_get_pane(wp);
	struct clipboard_data	*cd;
	char			*mime, *aliases;
	size_t			 len;

	if (~cp->flags & CLIPBOARD_WRITING)
		return;
	if ((mime = clipboard_decode_mime(msg->mime)) == NULL ||
	    (aliases = clipboard_decode(msg->payload, &len)) == NULL) {
		free(mime);
		clipboard_fail_write(wp, cp, "EINVAL");
		return;
	}
	if ((cd = clipboard_find_data(cp, mime)) == NULL ||
	    cp->names + len + 1 > CLIPBOARD_NAMES_MAX) {
		free(aliases);
		clipboard_fail_write(wp, cp, "EFBIG");
		return;
	}
	cp->names += len + 1;

	/* Grow by doubling so many small aliases are not copied each time. */
	if (cd->aliases_len + len + 2 > cd->aliases_size) {
		cd->aliases_size = (cd->aliases_len + len + 2) * 2;
		cd->aliases = xrealloc(cd->aliases, cd->aliases_size);
	}
	if (cd->aliases_len != 0)
		cd->aliases[cd->aliases_len++] = ' ';
	memcpy(cd->aliases + cd->aliases_len, aliases, len + 1);
	cd->aliases_len += len;
	free(aliases);
}

/* Answer a read from a pane with the newest paste buffer. */
static void
clipboard_read_buffer(struct window_pane *wp, const char *list,
    const char *id)
{
	struct paste_buffer	*pb = paste_get_top(NULL);
	struct clipboard_msg	 msg;
	char			*copy, *cp, *mime;
	const char		*buf;
	size_t			 len;
	int			 sent = 0;

	clipboard_reply(wp, "read", "OK", id);

	memset(&msg, 0, sizeof msg);
	msg.type = (char *)"read";
	msg.status = (char *)"DATA";
	msg.id = (char *)id;

	cp = copy = xstrdup(list);
	while (pb != NULL && (mime = strsep(&cp, " \t\r\n")) != NULL) {
		if (strcmp(mime, ".") == 0) {
			buf = "text/plain";
			len = strlen(buf);
		} else if (!sent && clipboard_is_text(mime)) {
			buf = paste_buffer_data(pb, &len);
			sent = 1;
		} else
			continue;
		msg.mime = clipboard_encode(mime, strlen(mime));
		clipboard_send_data(wp, NULL, &msg, buf, len);
		free(msg.mime);
	}
	free(copy);

	clipboard_reply(wp, "read", "DONE", id);
}

/* Finish the read a client is waiting for. */
static void
clipboard_end_read(struct clipboard_client *cc)
{
	struct window_pane	*wp;

	if (cc->read_osc52) {
		wp = window_pane_find_by_id(cc->read_pane);
		if (wp != NULL && wp->event != NULL) {
			input_reply_clipboard(wp->event, cc->text, cc->text_len,
			    cc->read_end, cc->read_clip);
		}
	}
	if (cc->read_buffer && cc->text != NULL) {
		paste_add(NULL, cc->text, cc->text_len);
		cc->text = NULL;
	}
	free(cc->text);
	cc->text = NULL;
	cc->text_len = 0;
	free(cc->text_mime);
	cc->text_mime = NULL;

	free(cc->read_id);
	free(cc->read_tty_id);
	cc->read_id = cc->read_tty_id = NULL;
	cc->read_pane = -1;
}

/* Give up the read a client is waiting for and tell the pane. */
static void
clipboard_cancel_read(struct clipboard_client *cc)
{
	struct window_pane	*wp;

	if (cc->read_pane == -1)
		return;
	wp = window_pane_find_by_id(cc->read_pane);
	if (cc->read_osc52) {
		free(cc->text);
		cc->text = NULL;
		cc->text_len = 0;
	} else if (cc->read_started)
		clipboard_reply(wp, "read", "DONE", cc->read_id);
	else
		clipboard_reply(wp, "read", "EBUSY", cc->read_id);
	cc->read_buffer = 0;
	clipboard_end_read(cc);
}

/*
 * Send a read from a pane to a client's terminal. Returns -1 if another read
 * is still waiting.
 */
static int
clipboard_send_read(struct client *c, struct window_pane *wp,
    struct clipboard_msg *msg, const char *id, int buffer)
{
	struct clipboard_client	*cc = clipboard_get_client_state(c);
	struct clipboard_msg	 out;

	if (cc->read_pane != -1) {
		if (time(NULL) - cc->read_time < CLIPBOARD_READ_TIMEOUT)
			return (-1);
		clipboard_cancel_read(cc);
	}

	cc->read_pane = wp->id;
	cc->read_id = (id == NULL ? NULL : xstrdup(id));
	xasprintf(&cc->read_tty_id, "tmux-%u", cc->next++);
	cc->read_time = time(NULL);
	cc->read_started = 0;
	cc->read_buffer = buffer;
	cc->read_osc52 = 0;
	cc->read_text = 0;
	cc->read_big = 0;

	memset(&out, 0, sizeof out);
	out.type = (char *)"read";
	out.id = cc->read_tty_id;
	out.name = msg->name;
	out.pw = msg->pw;
	if (clipboard_primary(msg))
		out.loc = (char *)"primary";
	out.payload = msg->payload;
	clipboard_send_client(c, &out);
	return (0);
}

/* Handle a read from a pane. */
static void
clipboard_pane_read(struct window_pane *wp, struct clipboard_msg *msg)
{
	struct client		*c;
	struct clipboard_client	*cc;
	char			*list, *id;
	int			 state, paste = 0;

	if ((list = clipboard_decode(msg->payload, NULL)) == NULL) {
		log_debug("%s: %%%u: bad MIME list", __func__, wp->id);
		return;
	}
	id = clipboard_copy_id(msg->id);

	/* A read with the password from a paste event may always go. */
	if ((c = clipboard_get_paste_client(wp, msg)) != NULL) {
		cc = clipboard_get_client_state(c);
		paste = 1;
	} else
		c = clipboard_get_client(wp);

	state = options_get_number(global_options, "get-clipboard");
	if (c != NULL && (paste || state != 0)) {
		if (clipboard_send_read(c, wp, msg, id, state == 3) != 0)
			clipboard_reply(wp, "read", "EBUSY", id);
		else if (paste) {
			/* The password is only good for one read. */
			free(cc->paste_pw);
			cc->paste_pw = NULL;
		}
	} else if (state == 0)
		clipboard_reply(wp, "read", "EPERM", id);
	else
		clipboard_read_buffer(wp, list, id);
	free(id);
	free(list);
}

/*
 * Send an OSC 52 read from a pane to a client's terminal with the protocol
 * and reply with OSC 52 when it is done. Returns -1 if there is no client.
 */
int
clipboard_osc52_read(struct window_pane *wp, char clip, const char *end,
    int buffer)
{
	struct client		*c = clipboard_get_client(wp);
	struct clipboard_client	*cc;
	struct clipboard_msg	 msg;

	if (c == NULL)
		return (-1);
	cc = clipboard_get_client_state(c);

	memset(&msg, 0, sizeof msg);
	if (clip == 'p')
		msg.loc = (char *)"primary";
	msg.payload = clipboard_encode("text/plain", 10);
	if (clipboard_send_read(c, wp, &msg, NULL, buffer) != 0)
		input_reply_clipboard(wp->event, NULL, 0, end, clip);
	else {
		cc->read_osc52 = 1;
		cc->read_clip = clip;
		cc->read_end = end;
	}
	free(msg.payload);
	return (0);
}

/* Handle an OSC 5522 message from a pane. */
void
clipboard_pane_message(struct window_pane *wp, const char *s)
{
	struct clipboard_msg	msg;

	if (clipboard_parse(&msg, s) != 0) {
		log_debug("%s: %%%u: bad OSC 5522: %.200s", __func__, wp->id,
		    s);
		return;
	}
	clipboard_check_password(&msg);
	if (strcmp(msg.type, "read") == 0)
		clipboard_pane_read(wp, &msg);
	else if (strcmp(msg.type, "write") == 0)
		clipboard_pane_write(wp, &msg);
	else if (strcmp(msg.type, "wdata") == 0)
		clipboard_pane_wdata(wp, &msg);
	else if (strcmp(msg.type, "walias") == 0)
		clipboard_pane_walias(wp, &msg);
	else
		log_debug("%s: %%%u: unknown type %s", __func__, wp->id,
		    msg.type);
	free(msg.copy);
}

/* Free a pane's state. */
void
clipboard_pane_destroy(struct window_pane *wp)
{
	struct clipboard_pane	*cp = wp->clipboard;

	clipboard_drop_writes(wp);
	if (cp == NULL)
		return;
	clipboard_reset_write(cp);
	free(cp);
	wp->clipboard = NULL;
}

/* Pass a reply to a read on to the pane waiting for it. */
static void
clipboard_tty_read(struct clipboard_client *cc, struct clipboard_msg *msg)
{
	struct window_pane	*wp = window_pane_find_by_id(cc->read_pane);
	char			*mime, *data;
	size_t			 len;

	if (msg->status == NULL)
		return;
	if (strcmp(msg->status, "DATA") == 0 &&
	    (cc->read_buffer || cc->read_osc52)) {
		if (msg->mime != NULL) {
			mime = clipboard_decode_mime(msg->mime);
			if (mime == NULL)
				cc->read_text = 0;
			else if (cc->text_mime != NULL)
				cc->read_text = (strcmp(mime, cc->text_mime) == 0);
			else if ((cc->read_text = clipboard_is_text(mime))) {
				cc->text_mime = mime;
				mime = NULL;
			}
			free(mime);
		}
		if (cc->read_text &&
		    (data = clipboard_decode(msg->payload, &len)) != NULL) {
			if (cc->read_big)
				/* nothing */;
			else if (cc->text_len + len > CLIPBOARD_MAX) {
				log_debug("%s: plain text too big", __func__);
				free(cc->text);
				cc->text = NULL;
				cc->text_len = 0;
				cc->read_big = 1;
			} else if (len != 0) {
				cc->text = xrealloc(cc->text,
				    cc->text_len + len);
				memcpy(cc->text + cc->text_len, data, len);
				cc->text_len += len;
			}
			free(data);
		}
	}

	if (!cc->read_osc52) {
		msg->id = cc->read_id;
		clipboard_send_pane(wp, msg);
	}

	if (strcmp(msg->status, "OK") == 0)
		cc->read_started = 1;
	else if (strcmp(msg->status, "DATA") != 0)
		clipboard_end_read(cc);
}

/* Handle the reply to a write and send the next. */
static void
clipboard_tty_write(struct client *c, struct clipboard_client *cc,
    struct clipboard_msg *msg)
{
	struct clipboard_write	*cw;
	char			*status;

	TAILQ_FOREACH(cw, &cc->writes, entry) {
		if (cw->out == NULL && strcmp(cw->tty_id, msg->id) == 0)
			break;
	}
	if (cw == NULL || msg->status == NULL)
		return;
	if (cw->reply) {
		status = clipboard_copy_id(msg->status);
		clipboard_reply(window_pane_find_by_id(cw->pane), "write",
		    status, cw->id);
		free(status);
	}
	clipboard_free_write(cc, cw);
	clipboard_next_write(c, cc);
}

/* Pass a paste event on to the active pane if it wants it. */
static void
clipboard_tty_paste(struct client *c, struct clipboard_client *cc,
    struct clipboard_msg *msg)
{
	struct window_pane	*wp = NULL;

	if (msg->status != NULL && strcmp(msg->status, "OK") == 0) {
		cc->paste_pane = -1;
		free(cc->paste_pw);
		cc->paste_pw = NULL;
		if (c->session != NULL)
			wp = c->session->curw->window->active;
		if (wp == NULL || (~wp->base.mode & MODE_PASTE_EVENTS)) {
			log_debug("%s: %s: no pane for paste", __func__,
			    c->name);
			return;
		}
		cc->paste_pane = wp->id;
		if (msg->pw != NULL)
			cc->paste_pw = xstrdup(msg->pw);
		cc->paste_primary = clipboard_primary(msg);
	} else if (cc->paste_pane != -1)
		wp = window_pane_find_by_id(cc->paste_pane);
	if (wp == NULL)
		return;

	clipboard_send_pane(wp, msg);
	if (msg->status == NULL || (strcmp(msg->status, "OK") != 0 &&
	    strcmp(msg->status, "DATA") != 0))
		cc->paste_pane = -1;
}

/* Handle an OSC 5522 message from a client's terminal. */
void
clipboard_tty_message(struct client *c, const char *buf, size_t len)
{
	struct clipboard_client	*cc = clipboard_get_client_state(c);
	struct clipboard_msg	 msg;
	char			*copy;

	copy = xstrndup(buf, len);
	if (clipboard_parse(&msg, copy) != 0) {
		log_debug("%s: %s: bad OSC 5522: %.200s", __func__, c->name,
		    copy);
		free(copy);
		return;
	}
	free(copy);

	if (strcmp(msg.type, "write") == 0 && msg.id != NULL)
		clipboard_tty_write(c, cc, &msg);
	else if (strcmp(msg.type, "read") != 0)
		log_debug("%s: %s: ignoring type %s", __func__, c->name,
		    msg.type);
	else if (msg.id == NULL) {
		if (!clipboard_client_ok(c) || (c->flags & CLIENT_READONLY))
			log_debug("%s: %s: ignoring paste", __func__, c->name);
		else
			clipboard_tty_paste(c, cc, &msg);
	} else if (cc->read_pane != -1 && strcmp(msg.id, cc->read_tty_id) == 0)
		clipboard_tty_read(cc, &msg);
	else
		log_debug("%s: %s: unknown id %s", __func__, c->name, msg.id);
	free(msg.copy);
}

/* Is the client in the middle of an exchange with its terminal? */
int
clipboard_client_active(struct client *c)
{
	struct clipboard_client	*cc = c->clipboard;

	return (cc != NULL && (cc->read_pane != -1 || cc->paste_pane != -1 ||
	    !TAILQ_EMPTY(&cc->writes)));
}

/* Stop any exchange with a client's terminal. */
void
clipboard_client_stop(struct client *c)
{
	struct clipboard_client	*cc = c->clipboard;
	struct clipboard_write	*cw, *cw1;

	if (cc == NULL)
		return;
	clipboard_cancel_read(cc);
	TAILQ_FOREACH_SAFE(cw, &cc->writes, entry, cw1) {
		if (cw->reply) {
			clipboard_reply(window_pane_find_by_id(cw->pane),
			    "write", "EIO", cw->id);
		}
		clipboard_free_write(cc, cw);
	}
	cc->paste_pane = -1;
	free(cc->paste_pw);
	cc->paste_pw = NULL;
}

/* Free a client's state. */
void
clipboard_client_free(struct client *c)
{
	clipboard_client_stop(c);
	free(c->clipboard);
	c->clipboard = NULL;
}
