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
#include <sys/socket.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Remote panes. A remote is another tmux server reached by running ssh as
 * configured in the remote-servers option, which starts tmux in control
 * mode there. tmux is a control client of that server and mirrors single
 * remote panes into local panes.
 *
 * Every mirrored pane is kept alone in its own remote window, linked into the
 * session the control client is attached to. The local server owns the layout
 * and sets each remote window size with refresh-client -C, so local windows
 * can mix local panes and panes from any number of remotes.
 *
 * A local remote pane has no process: its file descriptor is one end of a
 * socketpair. Pane output (%output) is written into the other end and parsed
 * as for any other pane; anything written to the pane (keys, paste) is read
 * back from it and sent with send-keys -H. Replies to terminal queries from
 * the local parser are dropped because the remote server answers them.
 *
 * When a pane is adopted, its contents and modes are fetched with
 * capture-pane and display-message and replayed into the local pane before
 * any further output.
 *
 * When the connection is lost, panes are kept with their contents and a
 * marker and input is dropped. After disconnect-remote they are disconnected
 * and stay so until connected again. Otherwise (connection lost, or no window
 * with a pane of the remote current for the idle time) they are inactive and
 * the remote is connected again when a window with one of them becomes current
 * or a key is pressed in it. Every new connection starts with list-panes,
 * which revives these panes by pane ID (if the remote server process is the
 * same) and closes those which have gone.
 */

struct remote;

typedef void (*remote_cb)(struct remote *, u_int, int, char **, u_int);

struct remote_command {
	remote_cb			 cb;
	u_int				 pane;
	int				 grouped;

	TAILQ_ENTRY(remote_command)	 entry;
};
TAILQ_HEAD(remote_commands, remote_command);

struct remote_pane {
	char			*name;
	struct remote		*remote;
	struct window_pane	*wp;

	int			 flags;
#define REMOTE_PANE_ID 0x1
#define REMOTE_PANE_WINDOW 0x2
#define REMOTE_PANE_SEEDING 0x4
#define REMOTE_PANE_CLOSING 0x8
#define REMOTE_PANE_INACTIVE 0x10
#define REMOTE_PANE_VISIBLE 0x20
#define REMOTE_PANE_DEAD 0x40
#define REMOTE_PANE_STARTING 0x80
#define REMOTE_PANE_DEFAULT 0x100

	u_int			 id;
	u_int			 window;
	char			*command;
	char			*start;
	char			*default_command;
	long			 pid;

	u_int			 sx;
	u_int			 sy;

	struct evbuffer		*seed;
	int			 seed_alternate;
	char			*seed_modes;
	char			*seed_cursor;
	char			*seed_pending;
	char			*seed_saved;
	char			*seed_wrap;
	int			 dead_status;

	int			 fd;
	struct bufferevent	*event;

	TAILQ_ENTRY(remote_pane) entry;
};
static TAILQ_HEAD(, remote_pane) remote_panes =
    TAILQ_HEAD_INITIALIZER(remote_panes);

struct remote {
	char			*name;
	struct job		*job;

	int			 flags;
#define REMOTE_SESSION 0x1
#define REMOTE_DISCONNECT 0x2

	u_int			 session;
	long			 pid;

	u_int			 idle;
	struct event		 idle_timer;

	struct remote_commands	 commands;
	struct remote_command	*current;
	char			*tag;
	char			**lines;
	u_int			 nlines;

	char			*error;
	struct cmdq_item	*item;
	int			 detached;

	TAILQ_ENTRY(remote)	 entry;
};
static TAILQ_HEAD(, remote) remotes = TAILQ_HEAD_INITIALIZER(remotes);

/* An entry from list-panes on connect. */
struct remote_entry {
	u_int	 pane;
	u_int	 window;
	u_int	 panes;
	int	 dead;
	int	 status;
	int	 ours;
	int	 wanted;
	int	 moved;
	char	*name;
};

#define REMOTE_LIST_FORMAT \
	"#{pid} #{session_id} #{session_attached} #{window_id} #{window_panes} " \
	"#{pane_id} #{pane_dead} #{pane_dead_status}:#{pane_dead_signal} " \
	"#{window_name}"
#define REMOTE_MODES_FORMAT \
	"#{cursor_x} #{cursor_y} #{scroll_region_upper} " \
	"#{scroll_region_lower} #{alternate_on} #{cursor_flag} " \
	"#{origin_flag} #{keypad_cursor_flag} #{keypad_flag} #{wrap_flag} " \
	"#{insert_flag} #{mouse_standard_flag} #{mouse_button_flag} " \
	"#{mouse_all_flag} #{mouse_sgr_flag} #{bracket_paste_flag} " \
	"#{alternate_saved_x} #{alternate_saved_y} t#{pane_tabs} " \
	"#{pane_key_mode}"
#define REMOTE_CURSOR_FORMAT \
	"#{cursor_shape}:#{cursor_blinking}:#{cursor_attributes}"
#define REMOTE_SAVED_FORMAT \
	"#{cursor_saved_x} #{cursor_saved_y} #{cursor_saved_origin_flag} " \
	"#{cursor_saved_attributes}"
#define REMOTE_DEAD_FORMAT \
	"remote-dead:%*:#{pane_dead}:#{pane_dead_status}:#{pane_dead_signal}"

static void	remote_pane_attach(struct remote_pane *);
static void	remote_pane_read_callback(struct bufferevent *, void *);
static int	remote_pane_parse_modes(struct remote_pane *, u_int *,
		    const char **, char **);
static void	remote_pane_seed_tabs(struct remote_pane *);
static int	remote_dead_status(const char *, const char *);
static void	remote_pane_died(struct remote_pane *, int);

/* A remote server from the remote-servers option. */
struct remote_config {
	char	 *name;
	char	 *host;
	char	 *user;
	char	 *ssh;
	char	 *tmux;
	u_int	  idle;
	int	  sshargc;
	char	**sshargv;
	int	  tmuxargc;
	char	**tmuxargv;
};

static const struct args_parse remote_config_args = { "a:H:i:o:s:t:u:", 0, 0,
    NULL };

/* Split a string into words with sh(1)-like quoting. */
static int
remote_split(const char *s, int *argc, char ***argv, char **cause)
{
	char	*word;
	size_t	 len;
	int	 quote, inword;

	*argc = 0;
	*argv = NULL;
	word = xmalloc(strlen(s) + 1);
	len = 0;
	quote = inword = 0;
	for (;; s++) {
		if (*s == '\0' || (quote == 0 && (*s == ' ' || *s == '\t'))) {
			if (quote != 0) {
				xasprintf(cause, "missing %c", quote);
				goto fail;
			}
			if (inword) {
				word[len] = '\0';
				*argv = xreallocarray(*argv, *argc + 1,
				    sizeof **argv);
				(*argv)[(*argc)++] = xstrdup(word);
				len = inword = 0;
			}
			if (*s == '\0')
				break;
			continue;
		}
		inword = 1;
		if (quote == '\'') {
			if (*s == '\'')
				quote = 0;
			else
				word[len++] = *s;
		} else if (*s == '\\' && s[1] != '\0' &&
		    (quote == 0 || strchr("\"\\$`", s[1]) != NULL))
			word[len++] = *++s;
		else if (*s == quote)
			quote = 0;
		else if (quote == 0 && (*s == '\'' || *s == '"'))
			quote = *s;
		else
			word[len++] = *s;
	}
	free(word);
	return (0);

fail:
	free(word);
	cmd_free_argv(*argc, *argv);
	*argc = 0;
	*argv = NULL;
	return (-1);
}

/* Free a remote server configuration. */
static void
remote_free_config(struct remote_config *rc)
{
	free(rc->name);
	free(rc->host);
	free(rc->user);
	free(rc->ssh);
	free(rc->tmux);
	cmd_free_argv(rc->sshargc, rc->sshargv);
	cmd_free_argv(rc->tmuxargc, rc->tmuxargv);
	free(rc);
}

/* Parse a remote-servers entry. */
static struct remote_config *
remote_parse_config(const char *entry, char **cause)
{
	struct remote_config	*rc;
	struct args		*args;
	struct args_value	*values;
	const char		*s;
	char			**argv, *error = NULL;
	int			 argc;

	if (remote_split(entry, &argc, &argv, &error) != 0) {
		xasprintf(cause, "bad remote: %s: %s", entry, error);
		free(error);
		return (NULL);
	}
	if (argc == 0) {
		xasprintf(cause, "empty remote");
		return (NULL);
	}
	if (*argv[0] == '\0' || *argv[0] == '-' ||
	    argv[0][strcspn(argv[0], ":.")] != '\0') {
		xasprintf(cause, "bad remote name: %s", argv[0]);
		cmd_free_argv(argc, argv);
		return (NULL);
	}

	values = args_from_vector(argc, argv);
	args = args_parse(&remote_config_args, values, argc, &error);
	args_free_values(values, argc);
	free(values);
	if (args == NULL) {
		xasprintf(cause, "bad remote: %s: %s", argv[0], error);
		free(error);
		cmd_free_argv(argc, argv);
		return (NULL);
	}

	rc = xcalloc(1, sizeof *rc);
	rc->name = xstrdup(argv[0]);
	cmd_free_argv(argc, argv);
	if ((s = args_get(args, 'H')) == NULL)
		s = rc->name;
	rc->host = xstrdup(s);
	if ((s = args_get(args, 'u')) != NULL)
		rc->user = xstrdup(s);
	if ((s = args_get(args, 's')) == NULL)
		s = "ssh";
	rc->ssh = xstrdup(s);
	if ((s = args_get(args, 't')) == NULL)
		s = "tmux";
	rc->tmux = xstrdup(s);
	if ((s = args_get(args, 'o')) != NULL &&
	    remote_split(s, &rc->sshargc, &rc->sshargv, &error) != 0) {
		xasprintf(cause, "bad remote: %s: -o: %s", rc->name, error);
		goto fail;
	}
	if ((s = args_get(args, 'a')) != NULL &&
	    remote_split(s, &rc->tmuxargc, &rc->tmuxargv, &error) != 0) {
		xasprintf(cause, "bad remote: %s: -a: %s", rc->name, error);
		goto fail;
	}
	rc->idle = 300;
	if (args_has(args, 'i')) {
		rc->idle = args_strtonum(args, 'i', 0, INT_MAX, &error);
		if (error != NULL) {
			xasprintf(cause, "bad remote: %s: -i %s", rc->name,
			    error);
			goto fail;
		}
	}
	if (*rc->host == '\0' || *rc->ssh == '\0' || *rc->tmux == '\0') {
		xasprintf(cause, "bad remote: %s: empty value", rc->name);
		goto fail;
	}
	args_free(args);
	return (rc);

fail:
	free(error);
	args_free(args);
	remote_free_config(rc);
	return (NULL);
}

/* Find a remote server in the remote-servers option. */
static struct remote_config *
remote_find_config(const char *name, char **cause)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	union options_value		*ov;
	size_t				 n;

	o = options_get_only(global_options, "remote-servers");
	a = (o == NULL ? NULL : options_array_first(o));
	while (a != NULL) {
		ov = options_array_item_value(a);
		n = strcspn(ov->string, " \t");
		if (n == strlen(name) && strncmp(ov->string, name, n) == 0)
			return (remote_parse_config(ov->string, cause));
		a = options_array_next(a);
	}
	xasprintf(cause, "unknown remote: %s", name);
	return (NULL);
}

/* Quote a word for the remote shell. */
static char *
remote_quote(const char *s)
{
	const char	*cp;
	char		*out;
	size_t		 len = 2;

	for (cp = s; *cp != '\0'; cp++)
		len += (*cp == '\'' ? 4 : 1);
	out = xmalloc(len + 1);
	len = 0;
	out[len++] = '\'';
	for (cp = s; *cp != '\0'; cp++) {
		if (*cp == '\'') {
			memcpy(out + len, "'\\''", 4);
			len += 4;
		} else
			out[len++] = *cp;
	}
	out[len++] = '\'';
	out[len] = '\0';
	return (out);
}

/* Build the ssh command line for a remote server. */
static void
remote_config_argv(struct remote_config *rc, int *argc, char ***argv)
{
	char	 host[HOST_NAME_MAX + 1], *command, *word, *new_command;
	int	 i, n;

	if (gethostname(host, sizeof host) != 0 || *host == '\0')
		strlcpy(host, "local", sizeof host);
	host[strcspn(host, ".:")] = '\0';

	command = remote_quote(rc->tmux);
	for (i = 0; i < rc->tmuxargc; i++) {
		word = remote_quote(rc->tmuxargv[i]);
		xasprintf(&new_command, "%s %s", command, word);
		free(word);
		free(command);
		command = new_command;
	}
	word = remote_quote(host);
	xasprintf(&new_command, "%s -C new-session -A -s %s", command, word);
	free(word);
	free(command);
	command = new_command;

	*argv = xcalloc(rc->sshargc + 7, sizeof **argv);
	n = 0;
	(*argv)[n++] = xstrdup(rc->ssh);
	(*argv)[n++] = xstrdup("-T");
	for (i = 0; i < rc->sshargc; i++)
		(*argv)[n++] = xstrdup(rc->sshargv[i]);
	if (rc->user != NULL) {
		(*argv)[n++] = xstrdup("-l");
		(*argv)[n++] = xstrdup(rc->user);
	}
	(*argv)[n++] = xstrdup("--");
	(*argv)[n++] = xstrdup(rc->host);
	(*argv)[n++] = command;
	*argc = n;
}

/* Find a connected remote. */
static struct remote *
remote_find(const char *name)
{
	struct remote	*r;

	TAILQ_FOREACH(r, &remotes, entry) {
		if (strcmp(r->name, name) == 0)
			return (r);
	}
	return (NULL);
}

/* Find a remote pane by remote pane ID. */
static struct remote_pane *
remote_find_pane(struct remote *r, u_int id)
{
	struct remote_pane	*rp;

	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (rp->remote == r && (rp->flags & REMOTE_PANE_ID) &&
		    rp->id == id)
			return (rp);
	}
	return (NULL);
}

/* Find a remote pane by remote window ID. */
static struct remote_pane *
remote_find_window(struct remote *r, u_int window)
{
	struct remote_pane	*rp;

	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (rp->remote == r && (rp->flags & REMOTE_PANE_WINDOW) &&
		    rp->window == window)
			return (rp);
	}
	return (NULL);
}

/* Get the remote pane for a local pane ID. */
static struct remote_pane *
remote_get_pane(struct remote *r, u_int pane)
{
	struct window_pane	*wp;

	wp = window_pane_find_by_id(pane);
	if (wp == NULL || wp->remote == NULL || wp->remote->remote != r)
		return (NULL);
	return (wp->remote);
}

/* Decode \ooo escapes as used in control mode output. */
static char *
remote_unescape(const char *s, size_t *size)
{
	char		*buf;
	const char	*cp;
	size_t		 n = 0;

	buf = xmalloc(strlen(s) + 1);
	for (cp = s; *cp != '\0'; cp++) {
		if (cp[0] == '\\' &&
		    cp[1] >= '0' && cp[1] <= '7' &&
		    cp[2] >= '0' && cp[2] <= '7' &&
		    cp[3] >= '0' && cp[3] <= '7') {
			buf[n++] = ((cp[1] - '0') << 6) |
			    ((cp[2] - '0') << 3) |
			    (cp[3] - '0');
			cp += 3;
		} else
			buf[n++] = *cp;
	}
	buf[n] = '\0';
	*size = n;
	return (buf);
}

/*
 * Expect the output block of a command. A grouped command follows the one
 * before in the same command sequence.
 */
static void
remote_queue(struct remote *r, remote_cb cb, u_int pane, int grouped)
{
	struct remote_command	*rc;

	rc = xcalloc(1, sizeof *rc);
	rc->cb = cb;
	rc->pane = pane;
	rc->grouped = grouped;
	TAILQ_INSERT_TAIL(&r->commands, rc, entry);
}

/* Send a command to a remote. */
static void printflike(4, 5)
remote_send(struct remote *r, remote_cb cb, u_int pane, const char *fmt, ...)
{
	struct bufferevent	*event = job_get_event(r->job);
	va_list			 ap;
	char			*s;

	va_start(ap, fmt);
	xvasprintf(&s, fmt, ap);
	va_end(ap);
	log_debug("%s: %s: %s", __func__, r->name, s);

	remote_queue(r, cb, pane, 0);
	bufferevent_write(event, s, strlen(s));
	bufferevent_write(event, "\n", 1);
	free(s);
}

/* Free collected command output. */
static void
remote_free_lines(struct remote *r)
{
	u_int	i;

	for (i = 0; i < r->nlines; i++)
		free(r->lines[i]);
	free(r->lines);
	r->lines = NULL;
	r->nlines = 0;
}

/* Finish a pending connect-remote. */
static void
remote_finish(struct remote *r, const char *cause)
{
	struct cmdq_item	*item = r->item;
	struct client		*c;
	struct session		*s;

	if (item == NULL)
		return;
	r->item = NULL;

	c = cmdq_get_client(item);
	if (cause != NULL) {
		cmdq_error(item, "remote %s: %s", r->name, cause);
		if (c != NULL)
			c->retval = 1;
	} else if (!r->detached && c != NULL && c->session != NULL) {
		s = session_find(r->name);
		if (s != NULL && s != c->session) {
			c->last_session = c->session;
			server_client_set_session(c, s);
		}
	}
	cmdq_continue(item);
}

/* Pass any buffered keys once the remote pane is known. */
static void
remote_pane_flush(struct remote_pane *rp)
{
	if (rp->event != NULL)
		remote_pane_read_callback(rp->event, rp);
}

/* Stop sending to a local pane, so it exits. */
static void
remote_pane_close(struct remote_pane *rp, int status, const char *msg)
{
	struct window_pane	*wp = rp->wp;

	if (rp->event == NULL || (rp->flags & REMOTE_PANE_CLOSING))
		return;
	log_debug("%s: %%%u (remote %s)", __func__, wp->id, rp->name);

	if (msg != NULL)
		bufferevent_write(rp->event, msg, strlen(msg));
	wp->status = status;
	wp->flags |= PANE_STATUSREADY;

	rp->flags |= REMOTE_PANE_CLOSING;
	rp->flags &= ~REMOTE_PANE_SEEDING;
	if (EVBUFFER_LENGTH(EVBUFFER_OUTPUT(rp->event)) == 0)
		shutdown(rp->fd, SHUT_WR);
}

/* Close the remote end of the socketpair. */
static void
remote_pane_shutdown(struct remote_pane *rp)
{
	if (rp->event != NULL) {
		bufferevent_free(rp->event);
		rp->event = NULL;
	}
	if (rp->fd != -1) {
		close(rp->fd);
		rp->fd = -1;
	}
}

/* Free seed state. */
static void
remote_pane_free_seed(struct remote_pane *rp)
{
	if (rp->seed != NULL) {
		evbuffer_free(rp->seed);
		rp->seed = NULL;
	}
	free(rp->seed_modes);
	rp->seed_modes = NULL;
	free(rp->seed_cursor);
	rp->seed_cursor = NULL;
	free(rp->seed_pending);
	rp->seed_pending = NULL;
	free(rp->seed_saved);
	rp->seed_saved = NULL;
	free(rp->seed_wrap);
	rp->seed_wrap = NULL;
	rp->seed_alternate = 0;
}

/* Keys and replies written to the local pane: send them on. */
static void
remote_pane_read_callback(__unused struct bufferevent *bufev, void *data)
{
	struct remote_pane	*rp = data;
	struct remote		*r = rp->remote;
	struct evbuffer		*evb = EVBUFFER_INPUT(rp->event);
	u_char			*buf;
	size_t			 size, i;
	char			*s;

	if (r == NULL || (rp->flags & REMOTE_PANE_CLOSING)) {
		evbuffer_drain(evb, EVBUFFER_LENGTH(evb));
		return;
	}
	if (~rp->flags & REMOTE_PANE_ID)
		return;

	while ((size = EVBUFFER_LENGTH(evb)) != 0) {
		if (size > 256)
			size = 256;
		buf = EVBUFFER_DATA(evb);

		s = xmalloc(size * 3 + 1);
		for (i = 0; i < size; i++)
			xsnprintf(s + i * 3, 4, " %02x", buf[i]);
		remote_send(r, NULL, 0, "send-keys -H -t %%%u%s", rp->id, s);
		free(s);

		evbuffer_drain(evb, size);
	}
}

/* Output to the local pane drained. */
static void
remote_pane_write_callback(__unused struct bufferevent *bufev, void *data)
{
	struct remote_pane	*rp = data;

	if ((rp->flags & REMOTE_PANE_CLOSING) &&
	    EVBUFFER_LENGTH(EVBUFFER_OUTPUT(rp->event)) == 0)
		shutdown(rp->fd, SHUT_WR);
}

/* Local end closed. */
static void
remote_pane_error_callback(__unused struct bufferevent *bufev,
    __unused short what, void *data)
{
	struct remote_pane	*rp = data;

	log_debug("%s: %%%u", __func__, rp->wp->id);
	bufferevent_disable(rp->event, EV_READ|EV_WRITE);
}

/* Give a local pane a new socketpair. */
static void
remote_pane_open(struct remote_pane *rp)
{
	struct window_pane	*wp = rp->wp;
	int			 fds[2];

	remote_pane_shutdown(rp);
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, fds) != 0)
		fatal("socketpair failed");
	window_pane_set_fd(wp, fds[0]);
	wp->base.mode &= ~MODE_CRLF;
	wp->base.mode |= MODE_CURSOR;

	rp->fd = fds[1];
	setblocking(rp->fd, 0);
	rp->event = bufferevent_new(rp->fd, remote_pane_read_callback,
	    remote_pane_write_callback, remote_pane_error_callback, rp);
	if (rp->event == NULL)
		fatalx("out of memory");
	bufferevent_enable(rp->event, EV_READ|EV_WRITE);
}

/* Make a local pane into a remote pane. */
static struct remote_pane *
remote_pane_create(struct remote *r, struct window_pane *wp)
{
	struct remote_pane	*rp;

	rp = xcalloc(1, sizeof *rp);
	rp->name = xstrdup(r->name);
	rp->remote = r;
	rp->wp = wp;
	rp->fd = -1;
	rp->sx = screen_size_x(&wp->base);
	rp->sy = screen_size_y(&wp->base);
	TAILQ_INSERT_TAIL(&remote_panes, rp, entry);

	wp->remote = rp;
	remote_pane_open(rp);
	return (rp);
}

/* Free a remote pane. */
static void
remote_pane_free(struct remote_pane *rp)
{
	TAILQ_REMOVE(&remote_panes, rp, entry);
	rp->wp->remote = NULL;

	remote_pane_shutdown(rp);
	remote_pane_free_seed(rp);
	free(rp->command);
	free(rp->start);
	free(rp->default_command);
	free(rp->name);
	free(rp);
}

/* Tell the remote the window size. */
static void
remote_pane_send_size(struct remote_pane *rp)
{
	struct remote	*r = rp->remote;

	if (r == NULL || (~rp->flags & REMOTE_PANE_WINDOW))
		return;
	remote_send(r, NULL, 0, "refresh-client -C @%u:%ux%u", rp->window,
	    rp->sx, rp->sy);
}

/* Append captured lines to the seed. */
static void
remote_pane_seed_lines(struct remote_pane *rp, char **lines, u_int nlines)
{
	u_int	i;

	for (i = 0; i < nlines; i++) {
		if (i != 0)
			evbuffer_add(rp->seed, "\033[m\017\r\n", 6);
		evbuffer_add(rp->seed, lines[i], strlen(lines[i]));
	}
	evbuffer_add(rp->seed, "\033[m\017", 4);
}

/* Got the pane modes. */
static void
remote_pane_modes_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	u_int			 cx, cy, upper, lower, alternate;

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	if (error || nlines == 0 || sscanf(lines[0], "%u %u %u %u %u", &cx,
	    &cy, &upper, &lower, &alternate) != 5)
		return;
	rp->seed_alternate = alternate;
	rp->seed_modes = xstrdup(lines[0]);
}

/* Got the cursor style and drawing attributes. */
static void
remote_pane_cursor_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	if (!error && nlines != 0)
		rp->seed_cursor = xstrdup(lines[0]);
}

/* Got the cursor saved by DECSC. */
static void
remote_pane_saved_cursor_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	if (!error && nlines != 0)
		rp->seed_saved = xstrdup(lines[0]);
}

/* Got the start of an unfinished escape sequence. */
static void
remote_pane_pending_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	if (!error && nlines != 0 && *lines[0] != '\0')
		rp->seed_pending = xstrdup(lines[0]);
}

/* Got the normal screen of a pane on the alternate screen. */
static void
remote_pane_saved_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	u_int			 v[18];

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	remote_pane_seed_tabs(rp);
	if (error || !rp->seed_alternate)
		return;
	remote_pane_seed_lines(rp, lines, nlines);
	if (remote_pane_parse_modes(rp, v, NULL, NULL) == 0) {
		evbuffer_add_printf(rp->seed, "\033[%u;%uH", v[17] + 1,
		    v[16] + 1);
	}
	evbuffer_add_printf(rp->seed, "\033[?1049h\033[H");
}

/* Parse the modes from display-message. */
static int
remote_pane_parse_modes(struct remote_pane *rp, u_int *v,
    const char **key_mode, char **tabs)
{
	const char	*cp;
	int		 off = 0;
	size_t		 n;

	if (rp->seed_modes == NULL)
		return (-1);
	if (sscanf(rp->seed_modes,
	    "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %n", &v[0],
	    &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
	    &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17],
	    &off) != 18)
		return (-1);
	cp = rp->seed_modes + off;
	if (tabs != NULL)
		*tabs = NULL;
	if (*cp == 't') {
		n = strcspn(cp + 1, " ");
		if (tabs != NULL)
			*tabs = xstrndup(cp + 1, n);
		cp += 1 + n;
		if (*cp == ' ')
			cp++;
	}
	if (key_mode != NULL)
		*key_mode = cp;
	return (0);
}

/*
 * Got the visible lines with trailing spaces: keep the cursor line if the
 * cursor is waiting to wrap.
 */
static void
remote_pane_wrap_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	u_int			 v[18];

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	if (error || remote_pane_parse_modes(rp, v, NULL, NULL) != 0)
		return;
	if (v[0] >= rp->sx && v[1] < nlines)
		rp->seed_wrap = xstrdup(lines[v[1]]);
}

/*
 * Append the tab stops to the seed. Captured lines contain tabs, so this
 * comes before them, from the first capture.
 */
static void
remote_pane_seed_tabs(struct remote_pane *rp)
{
	struct evbuffer	*evb = rp->seed;
	u_int		 v[18], n;
	const char	*errstr;
	char		*tabs, *copy, *next;

	if (remote_pane_parse_modes(rp, v, NULL, &tabs) != 0 || tabs == NULL)
		return;
	evbuffer_add_printf(evb, "\033[3g");
	copy = tabs;
	while ((next = strsep(&copy, ",")) != NULL) {
		n = strtonum(next, 0, rp->sx - 1, &errstr);
		if (errstr == NULL)
			evbuffer_add_printf(evb, "\033[1;%uH\033H", n + 1);
	}
	evbuffer_add_printf(evb, "\033[H");
	free(tabs);
}

/*
 * Append the cursor saved by DECSC to the seed. This needs the
 * cursor_saved_* formats, which older tmux does not have. The scroll region
 * and origin mode are not yet set, so the position is absolute.
 */
static void
remote_pane_seed_saved(struct remote_pane *rp)
{
	struct evbuffer	*evb = rp->seed;
	u_int		 cx, cy;
	int		 origin, off = 0;
	char		*buf;
	size_t		 size;

	if (rp->seed_saved == NULL || sscanf(rp->seed_saved, "%u %u %d %n",
	    &cx, &cy, &origin, &off) != 3)
		return;
	if (origin)
		evbuffer_add_printf(evb, "\033[?6h");
	evbuffer_add_printf(evb, "\033[%u;%uH\033(B\033)B\017\033[m", cy + 1,
	    cx + 1);
	buf = remote_unescape(rp->seed_saved + off, &size);
	evbuffer_add(evb, buf, size);
	free(buf);
	evbuffer_add_printf(evb, "\0337\033(B\033)0\017\033[m");
	if (origin)
		evbuffer_add_printf(evb, "\033[?6l");
}

/*
 * Append the modes from display-message to the seed. A cursor past the last
 * column is waiting to wrap, which a cursor movement cannot restore, so
 * redraw its line from the captured lines instead.
 */
static void
remote_pane_seed_modes(struct remote_pane *rp)
{
	struct evbuffer	*evb = rp->seed;
	u_int		 v[18], n, y;
	const char	*key_mode;

	if (remote_pane_parse_modes(rp, v, &key_mode, NULL) != 0)
		return;


	if (v[2] != 0 || v[3] != rp->sy - 1)
		evbuffer_add_printf(evb, "\033[%u;%ur", v[2] + 1, v[3] + 1);
	y = v[1] + 1;
	if (v[6]) {
		evbuffer_add_printf(evb, "\033[?6h");
		y -= v[2];
	}
	if (rp->seed_wrap != NULL)
		evbuffer_add_printf(evb, "\033[%u;1H%s\033[m\017", y,
		    rp->seed_wrap);
	else
		evbuffer_add_printf(evb, "\033[%u;%uH", y, v[0] + 1);
	if (!v[5])
		evbuffer_add_printf(evb, "\033[?25l");
	if (v[7])
		evbuffer_add_printf(evb, "\033[?1h");
	if (v[8])
		evbuffer_add_printf(evb, "\033=");
	if (!v[9])
		evbuffer_add_printf(evb, "\033[?7l");
	if (v[10])
		evbuffer_add_printf(evb, "\033[4h");
	if (v[11])
		evbuffer_add_printf(evb, "\033[?1000h");
	if (v[12])
		evbuffer_add_printf(evb, "\033[?1002h");
	if (v[13])
		evbuffer_add_printf(evb, "\033[?1003h");
	if (v[14])
		evbuffer_add_printf(evb, "\033[?1006h");
	if (v[15])
		evbuffer_add_printf(evb, "\033[?2004h");
	if (sscanf(key_mode, "Kitty %u", &n) == 1)
		evbuffer_add_printf(evb, "\033[>%uu", n);
	else if (sscanf(key_mode, "Ext %u", &n) == 1)
		evbuffer_add_printf(evb, "\033[>4;%um", n);
}

/*
 * Append the cursor style and drawing attributes to the seed. The attributes
 * need cursor_attributes, which older tmux does not have.
 */
static void
remote_pane_seed_cursor(struct remote_pane *rp)
{
	struct evbuffer	*evb = rp->seed;
	char		*copy, *shape, *blinking, *attributes, *buf;
	size_t		 size;
	u_int		 n = 0;

	if (rp->seed_cursor == NULL)
		return;
	copy = xstrdup(rp->seed_cursor);
	attributes = copy;
	shape = strsep(&attributes, ":");
	blinking = strsep(&attributes, ":");
	if (shape != NULL && blinking != NULL) {
		if (strcmp(shape, "block") == 0)
			n = 1;
		else if (strcmp(shape, "underline") == 0)
			n = 3;
		else if (strcmp(shape, "bar") == 0)
			n = 5;
		if (n != 0 && strcmp(blinking, "1") != 0)
			n++;
		if (n != 0)
			evbuffer_add_printf(evb, "\033[%u q", n);
	}
	if (attributes != NULL && *attributes != '\0') {
		buf = remote_unescape(attributes, &size);
		evbuffer_add(evb, buf, size);
		free(buf);
	}
	free(copy);
}

/* Got the pane contents: write the seed and start passing output. */
static void
remote_pane_capture_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	char			*buf;
	size_t			 size;

	if (rp == NULL || (~rp->flags & REMOTE_PANE_SEEDING))
		return;
	if (!error)
		remote_pane_seed_lines(rp, lines, nlines);
	remote_pane_seed_saved(rp);
	remote_pane_seed_modes(rp);
	evbuffer_add_printf(rp->seed, "\033(B\033)B\017\033[m");
	remote_pane_seed_cursor(rp);
	if (rp->seed_pending != NULL) {
		buf = remote_unescape(rp->seed_pending, &size);
		evbuffer_add(rp->seed, buf, size);
		free(buf);
	}

	bufferevent_write_buffer(rp->event, rp->seed);
	remote_pane_free_seed(rp);
	rp->flags &= ~REMOTE_PANE_SEEDING;

	if (rp->flags & REMOTE_PANE_DEAD)
		remote_pane_close(rp, rp->dead_status, NULL);
}

/* Start fetching the pane contents. */
static void
remote_pane_start_seed(struct remote_pane *rp)
{
	struct remote		*r = rp->remote;
	struct window_pane	*wp = rp->wp;

	window_pane_reset_mode_all(wp);
	input_reset(wp->ictx, 0);
	evbuffer_drain(input_pending(wp->ictx),
	    EVBUFFER_LENGTH(input_pending(wp->ictx)));
	screen_reinit(&wp->base, 0);
	grid_clear_history(wp->base.grid);
	wp->base.mode &= ~MODE_CRLF;
	wp->flags |= PANE_REDRAW;

	remote_pane_free_seed(rp);
	rp->seed = evbuffer_new();
	if (rp->seed == NULL)
		fatalx("out of memory");
	evbuffer_add_printf(rp->seed, "\033)0");

	/* One sequence, so no pane output is processed in between. */
	remote_send(r, remote_pane_modes_callback, wp->id,
	    "display-message -p -t %%%u '%s' ; "
	    "display-message -p -t %%%u '%s' ; "
	    "display-message -p -t %%%u '%s' ; "
	    "capture-pane -p -C -P -t %%%u ; "
	    "capture-pane -p -e -J -a -q -S - -t %%%u ; "
	    "capture-pane -p -e -N -t %%%u ; "
	    "capture-pane -p -e -J -S - -t %%%u",
	    rp->id, REMOTE_MODES_FORMAT, rp->id, REMOTE_CURSOR_FORMAT,
	    rp->id, REMOTE_SAVED_FORMAT, rp->id, rp->id, rp->id, rp->id);
	remote_queue(r, remote_pane_cursor_callback, wp->id, 1);
	remote_queue(r, remote_pane_saved_cursor_callback, wp->id, 1);
	remote_queue(r, remote_pane_pending_callback, wp->id, 1);
	remote_queue(r, remote_pane_saved_callback, wp->id, 1);
	remote_queue(r, remote_pane_wrap_callback, wp->id, 1);
	remote_queue(r, remote_pane_capture_callback, wp->id, 1);
}

/* Remote pane has a window of its own in our session. */
static void
remote_pane_attach(struct remote_pane *rp)
{
	remote_pane_send_size(rp);
	if (rp->flags & REMOTE_PANE_SEEDING)
		remote_pane_start_seed(rp);
	remote_pane_flush(rp);
}

/* Pane broken into its own window. */
static void
remote_pane_break_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	u_int			 window;

	if (rp == NULL)
		return;
	if (error || nlines == 0 || sscanf(lines[0], "@%u", &window) != 1) {
		remote_pane_close(rp, 1 << 8, "\r\nbreak-pane failed\r\n");
		return;
	}
	rp->window = window;
	rp->flags |= REMOTE_PANE_WINDOW;
	remote_pane_attach(rp);
}

/* Starting the command in a new remote pane failed. */
static void
remote_pane_start_failed(struct remote_pane *rp, const char *cause)
{
	char	*msg;

	rp->flags &= ~REMOTE_PANE_STARTING;
	remote_send(rp->remote, NULL, 0, "kill-pane -t %%%u", rp->id);
	xasprintf(&msg, "\r\n%s\r\n", cause);
	remote_pane_close(rp, 1 << 8, msg);
	free(msg);
}

/* Command started in the new remote pane. */
static void
remote_pane_respawn_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);

	if (rp == NULL || (~rp->flags & REMOTE_PANE_STARTING))
		return;
	if (error) {
		remote_pane_start_failed(rp,
		    nlines == 0 ? "respawn-pane failed" : lines[0]);
		return;
	}
	rp->flags &= ~REMOTE_PANE_STARTING;
	rp->pid = r->pid;
	rp->flags |= (REMOTE_PANE_ID|REMOTE_PANE_WINDOW);
	remote_pane_attach(rp);
}

/*
 * Got the default command and shell of a new remote pane. Start the command
 * as new-window would: with the shell if there is a default command, or
 * otherwise a login shell (respawn-pane can only ask for this with -l).
 */
static void
remote_pane_default_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	const char		*value = (nlines == 0 ? "" : lines[0]);
	char			*arg, *args, *tmp;
	u_int			 i;

	if (rp == NULL || (~rp->flags & REMOTE_PANE_STARTING))
		return;
	if (error) {
		remote_pane_start_failed(rp,
		    nlines == 0 ? "display-message failed" : lines[0]);
		return;
	}
	if (rp->default_command == NULL) {
		/* The command may have several lines. */
		rp->default_command = xstrdup(value);
		for (i = 1; i < nlines; i++) {
			xasprintf(&tmp, "%s\n%s", rp->default_command,
			    lines[i]);
			free(rp->default_command);
			rp->default_command = tmp;
		}
		return;
	}

	free(rp->command);
	if (*rp->default_command != '\0') {
		args = args_escape(rp->default_command);
		rp->command = parse_window_name(rp->default_command);
	} else {
		if (*value == '\0')
			value = _PATH_BSHELL;
		arg = args_escape(value);
		xasprintf(&args, "%s -l", arg);
		free(arg);
		rp->command = parse_window_name(value);
	}
	remote_send(r, remote_pane_respawn_callback, pane,
	    "respawn-pane -k -t %%%u%s %s", rp->id, rp->start, args);
	free(args);
}

/*
 * New remote window created with a placeholder. Keep the pane when it exits
 * and give it the local size before starting the command, so neither a hook
 * that waits nor the first size the command sees can be wrong.
 */
static void
remote_pane_placeholder_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	u_int			 id, window;
	char			*msg;

	if (error || nlines == 0 ||
	    sscanf(lines[0], "%%%u @%u", &id, &window) != 2) {
		if (rp != NULL) {
			xasprintf(&msg, "\r\n%s\r\n",
			    nlines == 0 ? "new-window failed" : lines[0]);
			remote_pane_close(rp, 1 << 8, msg);
			free(msg);
		}
		return;
	}
	if (rp == NULL) {
		remote_send(r, NULL, 0, "kill-pane -t %%%u", id);
		return;
	}

	rp->id = id;
	rp->window = window;
	rp->flags |= REMOTE_PANE_STARTING;
	if (rp->flags & REMOTE_PANE_DEFAULT) {
		remote_send(r, NULL, 0, "set-option -p -t %%%u remain-on-exit "
		    "on ; refresh-client -C @%u:%ux%u ; display-message -p -t "
		    "%%%u '#{?#{window-default-command},"
		    "#{window-default-command},#{default-command}}' ; "
		    "display-message -p -t %%%u '#{default-shell}'", id,
		    window, rp->sx, rp->sy, id, id);
		remote_queue(r, NULL, 0, 1);
		remote_queue(r, remote_pane_default_callback, pane, 1);
		remote_queue(r, remote_pane_default_callback, pane, 1);
		return;
	}
	remote_send(r, NULL, 0, "set-option -p -t %%%u remain-on-exit on ; "
	    "refresh-client -C @%u:%ux%u ; respawn-pane -k -t %%%u%s", id,
	    window, rp->sx, rp->sy, id, rp->start);
	remote_queue(r, NULL, 0, 1);
	remote_queue(r, remote_pane_respawn_callback, pane, 1);
}

/*
 * Create a new remote window for a remote pane. A placeholder that never exits
 * runs until the command is started; start is the respawn-pane arguments, or
 * only -c if the default command should be used.
 */
static void
remote_pane_new_window(struct remote_pane *rp, char *start, int dflt)
{
	free(rp->start);
	rp->start = start;
	if (dflt)
		rp->flags |= REMOTE_PANE_DEFAULT;
	remote_send(rp->remote, remote_pane_placeholder_callback, rp->wp->id,
	    "new-window -P -F '#{pane_id} #{window_id}' sleep 2147483647");
}

/* Window unlinked somewhere: close the pane if it has gone. */
static void
remote_pane_check_callback(struct remote *r, u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_pane	*rp = remote_get_pane(r, pane);
	u_int			 window;

	if (rp == NULL)
		return;
	if (error || nlines == 0 || sscanf(lines[0], "@%u", &window) != 1) {
		remote_pane_close(rp, 0, NULL);
		return;
	}
	if (window != rp->window) {
		rp->window = window;
		remote_pane_send_size(rp);
	}
}

/* Find or create the local session for adopted panes. */
static struct session *
remote_get_session(struct remote *r, int *created)
{
	struct session	*s;
	const char	*home;

	*created = 0;
	if ((s = session_find(r->name)) != NULL)
		return (s);

	if ((home = find_home()) == NULL)
		home = "/";
	s = session_create(NULL, r->name, home, environ_create(),
	    options_create(global_s_options), NULL);
	*created = 1;
	return (s);
}

/* Create a local window with an empty pane for a remote pane. */
static struct window_pane *
remote_spawn_window(struct remote *r)
{
	struct spawn_context	 sc = { 0 };
	struct session		*s;
	struct winlink		*wl;
	char			*cause;
	int			 created;

	s = remote_get_session(r, &created);

	sc.s = s;
	sc.idx = -1;
	sc.environ = environ_create();
	sc.flags = SPAWN_EMPTY|SPAWN_DETACHED;
	wl = spawn_window(&sc, &cause);
	environ_free(sc.environ);
	if (wl == NULL) {
		log_debug("%s: %s", __func__, cause);
		free(cause);
		if (created)
			session_destroy(s, 0, __func__);
		return (NULL);
	}
	if (created)
		events_fire_session("session-created", s);
	server_status_session(s);
	return (wl->window->active);
}

/* Is this a pane left from an earlier connection to this remote? */
static int
remote_pane_disconnected(struct remote_pane *rp, struct remote *r)
{
	return (rp->remote == NULL &&
	    (rp->flags & REMOTE_PANE_ID) &&
	    (~rp->flags & REMOTE_PANE_CLOSING) &&
	    strcmp(rp->name, r->name) == 0);
}

/* Find a disconnected pane of the same remote server. */
static struct remote_pane *
remote_find_disconnected(struct remote *r, u_int id)
{
	struct remote_pane	*rp;

	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (remote_pane_disconnected(rp, r) &&
		    rp->id == id &&
		    rp->pid == r->pid)
			return (rp);
	}
	return (NULL);
}

/* Adopt an existing remote pane. */
static void
remote_adopt(struct remote *r, struct remote_entry *re, int split)
{
	struct remote_pane	*rp;
	struct window_pane	*wp;

	if ((rp = remote_find_disconnected(r, re->pane)) != NULL) {
		wp = rp->wp;
		log_debug("%s: reviving %%%u for %s %%%u", __func__, wp->id,
		    r->name, re->pane);
		rp->remote = r;
		rp->flags &= ~(REMOTE_PANE_WINDOW|REMOTE_PANE_INACTIVE);
		rp->sx = wp->sx;
		rp->sy = wp->sy;
		if (rp->event == NULL)
			remote_pane_open(rp);
	} else {
		if ((wp = remote_spawn_window(r)) == NULL)
			return;
		log_debug("%s: %%%u for %s %%%u", __func__, wp->id, r->name,
		    re->pane);
		rp = remote_pane_create(r, wp);
	}
	rp->id = re->pane;
	rp->pid = r->pid;
	rp->flags |= (REMOTE_PANE_ID|REMOTE_PANE_SEEDING);
	free(rp->command);
	rp->command = xstrdup(re->name);

	/* Keep the pane when it exits until its status has been seen. */
	remote_send(r, NULL, 0, "set-option -p -t %%%u remain-on-exit on",
	    re->pane);

	/* Dead already: fetch its last contents, then close and kill it. */
	if (re->dead) {
		rp->dead_status = re->status;
		rp->flags |= REMOTE_PANE_DEAD;
		split = 0;
	}

	if (split) {
		remote_send(r, remote_pane_break_callback, wp->id,
		    "break-pane -d -s %%%u -t '$%u:' -P -F '#{window_id}'",
		    re->pane, r->session);
		return;
	}
	if (!re->ours) {
		remote_send(r, NULL, 0, "link-window -d -s @%u -t '$%u:'",
		    re->window, r->session);
	}
	rp->window = re->window;
	rp->flags |= REMOTE_PANE_WINDOW;
	remote_pane_attach(rp);
	if (rp->flags & REMOTE_PANE_DEAD)
		remote_send(r, NULL, 0, "kill-pane -t %%%u", re->pane);
}

/* Got the list of remote panes on connect. */
static void
remote_list_callback(struct remote *r, __unused u_int pane, int error,
    char **lines, u_int nlines)
{
	struct remote_entry	*list = NULL, *re;
	struct remote_pane	*rp;
	struct window_pane	*wp;
	u_int			 n = 0, i, j, session, attached, window;
	u_int			 panes, id, adopted = 0, moved;
	int			 off, dead;
	long			 pid;
	char			 status[64], *signal;

	if (error || (~r->flags & REMOTE_SESSION)) {
		remote_finish(r, nlines == 0 ? "list-panes failed" : lines[0]);
		return;
	}

	for (i = 0; i < nlines; i++) {
		off = 0;
		if (sscanf(lines[i], "%ld $%u %u @%u %u %%%u %d %63s %n", &pid,
		    &session, &attached, &window, &panes, &id, &dead, status,
		    &off) != 8 || off == 0)
			continue;
		r->pid = pid;
		for (j = 0; j < n; j++) {
			if (list[j].pane == id)
				break;
		}
		if (j == n) {
			list = xreallocarray(list, n + 1, sizeof *list);
			re = &list[n++];
			memset(re, 0, sizeof *re);
			re->pane = id;
			re->window = window;
			re->panes = panes;
			re->dead = dead;
			if (dead && (signal = strchr(status, ':')) != NULL) {
				*signal++ = '\0';
				re->status = remote_dead_status(status, signal);
			}
			re->name = xstrdup(lines[i] + off);
		} else
			re = &list[j];
		if (session == r->session)
			re->ours = 1;
		if ((session == r->session || attached == 0) && !dead)
			re->wanted = 1;
	}

	for (i = 0; i < n; i++) {
		re = &list[i];
		if ((rp = remote_find_pane(r, re->pane)) != NULL) {
			if (re->dead && (~rp->flags &
			    (REMOTE_PANE_DEAD|REMOTE_PANE_CLOSING)))
				remote_pane_died(rp, re->status);
			continue;
		}
		if (re->wanted || remote_find_disconnected(r, re->pane) != NULL) {
			/* Break out unless it is the last pane left. */
			moved = 0;
			for (j = 0; j < i; j++) {
				if (list[j].window == re->window && list[j].moved)
					moved++;
			}
			re->moved = (!re->dead && re->panes - moved > 1);
			remote_adopt(r, re, re->moved);
			adopted++;
		}
	}
	for (i = 0; i < n; i++)
		free(list[i].name);
	free(list);

	/* Panes not revived have gone. */
	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (remote_pane_disconnected(rp, r))
			remote_pane_close(rp, 0, NULL);
	}

	if (r->item == NULL)
		return;
	if (adopted == 0 && session_find(r->name) == NULL) {
		wp = remote_spawn_window(r);
		if (wp == NULL) {
			remote_finish(r, "no panes");
			return;
		}
		remote_pane_new_window(remote_pane_create(r, wp),
		    xstrdup(""), 1);
	}
	remote_finish(r, NULL);
}

/* Command output block finished. */
static void
remote_end_block(struct remote *r, int error)
{
	struct remote_command	*rc = r->current;
	int			 own = (rc != NULL);

	r->current = NULL;
	free(r->tag);
	r->tag = NULL;

	if (rc != NULL) {
		if (error) {
			log_debug("%s: %s: error: %s", __func__, r->name,
			    r->nlines == 0 ? "" : r->lines[0]);
		}
		if (rc->cb != NULL)
			rc->cb(r, rc->pane, error, r->lines, r->nlines);
		free(rc);
	}
	while (error && own && (rc = TAILQ_FIRST(&r->commands)) != NULL &&
	    rc->grouped) {
		TAILQ_REMOVE(&r->commands, rc, entry);
		if (rc->cb != NULL)
			rc->cb(r, rc->pane, 1, NULL, 0);
		free(rc);
	}
	remote_free_lines(r);
}

/* Write pane output to a local pane. */
static void
remote_output(struct remote *r, const char *line)
{
	struct remote_pane	*rp;
	u_int			 id;
	char			*end, *buf;
	size_t			 n;

	id = strtoul(line, &end, 10);
	if (*end != ' ')
		return;
	rp = remote_find_pane(r, id);
	if (rp == NULL || rp->event == NULL)
		return;
	if (rp->flags & (REMOTE_PANE_SEEDING|REMOTE_PANE_CLOSING))
		return;

	buf = remote_unescape(end + 1, &n);
	bufferevent_write(rp->event, buf, n);
	free(buf);
}

/* Get a signal number from a name or number. */
static int
remote_signal(const char *s)
{
	const char	*errstr;
	int		 sig;

	sig = strtonum(s, 1, NSIG - 1, &errstr);
	if (errstr == NULL)
		return (sig);
	for (sig = 1; sig < NSIG; sig++) {
		if (strcmp(sig2name(sig), s) == 0)
			return (sig);
	}
	return (0);
}

/* A connected remote pane has died: close it and kill the remote pane. */
static void
remote_pane_died(struct remote_pane *rp, int status)
{
	log_debug("%s: %s %%%u status %d", __func__, rp->name, rp->id, status);
	rp->dead_status = status;
	rp->flags |= REMOTE_PANE_DEAD;
	remote_send(rp->remote, NULL, 0, "kill-pane -t %%%u", rp->id);
	if (~rp->flags & REMOTE_PANE_SEEDING)
		remote_pane_close(rp, rp->dead_status, NULL);
}

/* Get a local exit status from a remote pane's status and signal. */
static int
remote_dead_status(const char *status, const char *signal)
{
	int	sig;

	if (signal != NULL && (sig = remote_signal(signal)) != 0)
		return (sig);
	if (status != NULL && *status != '\0')
		return ((atoi(status) & 0xff) << 8);
	return (0);
}

/*
 * A remote pane is dead or alive. Pass its exit status to the local pane and
 * kill it.
 */
static void
remote_dead(struct remote *r, const char *line)
{
	struct remote_pane	*rp;
	const char		*cp;
	char			*copy, *next, *dead, *status, *signal;
	u_int			 id;

	if ((cp = strstr(line, " %")) == NULL)
		return;
	id = strtoul(cp + 2, NULL, 10);
	if ((cp = strstr(cp, " : ")) == NULL)
		return;
	if ((rp = remote_find_pane(r, id)) == NULL ||
	    (rp->flags & (REMOTE_PANE_CLOSING|REMOTE_PANE_DEAD)))
		return;

	next = copy = xstrdup(cp + 3);
	dead = strsep(&next, ":");
	status = strsep(&next, ":");
	signal = next;
	if (strcmp(dead, "1") != 0) {
		free(copy);
		return;
	}
	remote_pane_died(rp, remote_dead_status(status, signal));
	free(copy);
}

/* Handle a line from the remote. */
static void
remote_parse(struct remote *r, const char *line)
{
	struct remote_pane	*rp;
	u_int			 id;
	int			 flags;
	char			*end;
	const char		*cp;

	if (r->tag != NULL) {
		if (strncmp(line, "%end ", 5) == 0 &&
		    strcmp(line + 5, r->tag) == 0)
			remote_end_block(r, 0);
		else if (strncmp(line, "%error ", 7) == 0 &&
		    strcmp(line + 7, r->tag) == 0)
			remote_end_block(r, 1);
		else if (r->current != NULL) {
			r->lines = xreallocarray(r->lines, r->nlines + 1,
			    sizeof *r->lines);
			r->lines[r->nlines++] = xstrdup(line);
		}
		return;
	}

	if (strncmp(line, "%begin ", 7) == 0) {
		if (sscanf(line + 7, "%*s %*s %d", &flags) != 1)
			return;
		r->tag = xstrdup(line + 7);
		if ((flags & 1) &&
		    (r->current = TAILQ_FIRST(&r->commands)) != NULL)
			TAILQ_REMOVE(&r->commands, r->current, entry);
		return;
	}

	if (strncmp(line, "%output %", 9) == 0) {
		remote_output(r, line + 9);
		return;
	}
	if (strncmp(line, "%extended-output %", 18) == 0) {
		id = strtoul(line + 18, &end, 10);
		if ((cp = strstr(end, " : ")) != NULL) {
			xasprintf(&end, "%u %s", id, cp + 3);
			remote_output(r, end);
			free(end);
		}
		return;
	}
	log_debug("%s: %s: %s", __func__, r->name, line);

	if (*line != '%') {
		free(r->error);
		r->error = xstrdup(line);
		return;
	}
	if (strncmp(line, "%session-changed $", 18) == 0) {
		r->session = strtoul(line + 18, NULL, 10);
		r->flags |= REMOTE_SESSION;
		return;
	}
	if (strncmp(line, "%window-close @", 15) == 0 ||
	    strncmp(line, "%unlinked-window-close @", 24) == 0) {
		id = strtoul(strchr(line, '@') + 1, NULL, 10);
		if ((rp = remote_find_window(r, id)) != NULL) {
			remote_send(r, remote_pane_check_callback, rp->wp->id,
			    "display-message -p -t %%%u '#{window_id}'",
			    rp->id);
		}
		return;
	}
	if (strncmp(line, "%window-renamed @", 17) == 0) {
		id = strtoul(line + 17, &end, 10);
		if (*end == ' ' && (rp = remote_find_window(r, id)) != NULL) {
			free(rp->command);
			rp->command = xstrdup(end + 1);
		}
		return;
	}
	if (strncmp(line, "%subscription-changed remote-dead ", 34) == 0) {
		remote_dead(r, line + 34);
		return;
	}
	if (strncmp(line, "%pause %", 8) == 0) {
		id = strtoul(line + 8, NULL, 10);
		remote_send(r, NULL, 0, "refresh-client -A '%%%u:continue'",
		    id);
		return;
	}
}

/* Data from the remote. */
static void
remote_update_callback(struct job *job)
{
	struct remote	*r = job_get_data(job);
	struct evbuffer	*evb = EVBUFFER_INPUT(job_get_event(job));
	char		*line;

	while ((line = evbuffer_readln(evb, NULL, EVBUFFER_EOL_LF)) != NULL) {
		remote_parse(r, line);
		free(line);
	}
}

/* Draw the inactive or disconnected marker over a pane. */
static void
remote_pane_marker(struct remote_pane *rp)
{
	const char	*style, *state;
	char		*msg;

	if (rp->event == NULL || (rp->flags & REMOTE_PANE_CLOSING))
		return;
	if (rp->flags & REMOTE_PANE_INACTIVE) {
		style = "\033[30;43m";
		state = "inactive";
	} else {
		style = "\033[7m";
		state = "disconnected";
	}
	xasprintf(&msg, "\0337\033[%u;1H\033[m%s\033[2K[remote %s %s]\033[m\0338",
	    rp->wp->sy, style, rp->name, state);
	bufferevent_write(rp->event, msg, strlen(msg));
	free(msg);
	server_redraw_window_borders(rp->wp->window);
	server_status_window(rp->wp->window);
}

/* Remote connection gone. */
static void
remote_free_callback(void *data)
{
	struct remote		*r = data;
	struct remote_pane	*rp;
	struct remote_command	*rc, *rc1;
	char			*msg;

	log_debug("%s: %s", __func__, r->name);
	TAILQ_REMOVE(&remotes, r, entry);
	evtimer_del(&r->idle_timer);

	remote_finish(r, r->error != NULL ? r->error : "connection closed");

	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (rp->remote != r)
			continue;
		remote_pane_free_seed(rp);
		rp->flags &= ~REMOTE_PANE_SEEDING;
		if (rp->flags & REMOTE_PANE_CLOSING)
			;
		else if (rp->flags & REMOTE_PANE_DEAD)
			remote_pane_close(rp, rp->dead_status, NULL);
		else if (rp->flags & REMOTE_PANE_ID) {
			/* Keep the pane until reconnected, with a marker. */
			if (r->flags & REMOTE_DISCONNECT)
				rp->flags &= ~REMOTE_PANE_INACTIVE;
			else
				rp->flags |= REMOTE_PANE_INACTIVE;
			rp->remote = NULL;
			remote_pane_marker(rp);
		} else {
			xasprintf(&msg, "\r\n\033[m[remote %s disconnected]\r\n",
			    r->name);
			options_set_number(rp->wp->options, "remain-on-exit",
			    1);
			remote_pane_close(rp, 1 << 8, msg);
			free(msg);
		}
		rp->remote = NULL;
		server_redraw_window_borders(rp->wp->window);
		server_status_window(rp->wp->window);
	}

	TAILQ_FOREACH_SAFE(rc, &r->commands, entry, rc1) {
		TAILQ_REMOVE(&r->commands, rc, entry);
		free(rc);
	}
	free(r->current);
	free(r->tag);
	remote_free_lines(r);

	free(r->error);
	free(r->name);
	free(r);
}

/* Nothing of the remote has been seen for a while: disconnect. */
static void
remote_idle_callback(__unused int fd, __unused short events, void *data)
{
	struct remote	*r = data;

	log_debug("%s: %s", __func__, r->name);
	job_free(r->job);
}

/* Start a connection to a remote if not already connected. */
static struct remote *
remote_start(const char *name, char **cause)
{
	struct remote		*r;
	struct remote_config	*rc;
	char			**argv, *command;
	int			  argc;
	const char		 *home;

	if ((r = remote_find(name)) != NULL)
		return (r);
	if ((rc = remote_find_config(name, cause)) == NULL)
		return (NULL);
	remote_config_argv(rc, &argc, &argv);
	command = cmd_stringify_argv(argc, argv);

	r = xcalloc(1, sizeof *r);
	r->name = xstrdup(name);
	r->idle = rc->idle;
	TAILQ_INIT(&r->commands);
	evtimer_set(&r->idle_timer, remote_idle_callback, r);
	remote_free_config(rc);

	if ((home = find_home()) == NULL)
		home = "/";
	r->job = job_run(NULL, argc, argv, NULL, NULL, home,
	    remote_update_callback, NULL, remote_free_callback, r,
	    JOB_NOWAIT|JOB_KEEPWRITE|JOB_SHOWSTDERR, -1, -1);
	cmd_free_argv(argc, argv);
	if (r->job == NULL) {
		xasprintf(cause, "failed to run: %s", command);
		free(command);
		free(r->name);
		free(r);
		return (NULL);
	}
	log_debug("%s: %s: %s", __func__, name, command);
	free(command);

	TAILQ_INSERT_TAIL(&remotes, r, entry);

	/* Watch for panes exiting, they stay because of remain-on-exit. */
	remote_send(r, NULL, 0, "refresh-client -B '%s'", REMOTE_DEAD_FORMAT);

	/* Revive panes left from before and adopt unattached panes. */
	remote_send(r, remote_list_callback, 0, "list-panes -a -F '%s'",
	    REMOTE_LIST_FORMAT);
	return (r);
}

/* Connect to a remote and adopt its unattached panes. */
int
remote_connect(struct cmdq_item *item, const char *name, int detached,
    char **cause)
{
	struct remote	*r;

	if ((r = remote_find(name)) != NULL) {
		if (r->item != NULL) {
			xasprintf(cause, "already connecting to %s", name);
			return (-1);
		}
		remote_send(r, remote_list_callback, 0, "list-panes -a -F '%s'",
		    REMOTE_LIST_FORMAT);
	} else if ((r = remote_start(name, cause)) == NULL)
		return (-1);
	r->item = item;
	r->detached = detached;
	return (0);
}

/* Disconnect from a remote. It stays down until connected again. */
int
remote_disconnect(const char *name, char **cause)
{
	struct remote		*r;
	struct remote_pane	*rp;
	int			 found = 0;

	if ((r = remote_find(name)) != NULL) {
		r->flags |= REMOTE_DISCONNECT;
		job_free(r->job);
		found = 1;
	}
	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (rp->remote == NULL &&
		    (rp->flags & REMOTE_PANE_INACTIVE) &&
		    strcmp(rp->name, name) == 0) {
			rp->flags &= ~REMOTE_PANE_INACTIVE;
			remote_pane_marker(rp);
			found = 1;
		}
	}
	if (!found) {
		xasprintf(cause, "not connected: %s", name);
		return (-1);
	}
	return (0);
}

/* Kill local panes of a remote which are not connected. */
int
remote_cleanup(const char *name, char **cause)
{
	struct remote_config	*rc;
	struct remote_pane	*rp;

	if ((rc = remote_find_config(name, cause)) == NULL)
		return (-1);
	remote_free_config(rc);
again:
	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (rp->remote == NULL &&
		    (~rp->flags & REMOTE_PANE_CLOSING) &&
		    strcmp(rp->name, name) == 0) {
			server_kill_pane(rp->wp);
			goto again;
		}
	}
	return (0);
}

/* Connect again to the remote of an inactive pane. */
static void
remote_reconnect(struct remote_pane *rp)
{
	char	*cause;

	if (rp->remote != NULL || (~rp->flags & REMOTE_PANE_INACTIVE) ||
	    (rp->flags & REMOTE_PANE_CLOSING) || remote_find(rp->name) != NULL)
		return;
	log_debug("%s: %s", __func__, rp->name);
	if (remote_start(rp->name, &cause) == NULL) {
		log_debug("%s: %s", __func__, cause);
		free(cause);
	}
}

/* Is a window current for any attached client? */
static int
remote_window_visible(struct window *w)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || c->session->curw == NULL ||
		    (c->flags & (CLIENT_CONTROL|CLIENT_UNATTACHEDFLAGS)))
			continue;
		if (c->session->curw->window == w)
			return (1);
	}
	return (0);
}

/*
 * Check remotes every loop: reconnect inactive panes when their window
 * becomes current and start the idle timer when no window with panes of a
 * remote is current.
 */
void
remote_check(void)
{
	struct remote		*r;
	struct remote_pane	*rp;
	struct timeval		 tv;
	int			 visible;

	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (!remote_window_visible(rp->wp->window)) {
			rp->flags &= ~REMOTE_PANE_VISIBLE;
			continue;
		}
		if (~rp->flags & REMOTE_PANE_VISIBLE) {
			rp->flags |= REMOTE_PANE_VISIBLE;
			remote_reconnect(rp);
		}
	}

	TAILQ_FOREACH(r, &remotes, entry) {
		visible = 0;
		TAILQ_FOREACH(rp, &remote_panes, entry) {
			if (rp->remote == r && (rp->flags & REMOTE_PANE_VISIBLE))
				visible = 1;
		}
		if (visible || r->item != NULL || r->idle == 0)
			evtimer_del(&r->idle_timer);
		else if (!evtimer_pending(&r->idle_timer, NULL)) {
			timerclear(&tv);
			tv.tv_sec = r->idle;
			evtimer_add(&r->idle_timer, &tv);
		}
	}
}

/* Key pressed in a client: reconnect inactive panes in its window. */
void
remote_key(struct client *c)
{
	struct remote_pane	*rp;

	if (c->session == NULL || c->session->curw == NULL)
		return;
	TAILQ_FOREACH(rp, &remote_panes, entry) {
		if (rp->wp->window == c->session->curw->window)
			remote_reconnect(rp);
	}
}

/* Disconnect all remotes on exit, leaving remote panes running. */
void
remote_disconnect_all(void)
{
	struct remote	*r, *r1;

	TAILQ_FOREACH_SAFE(r, &remotes, entry, r1)
		job_free(r->job);
}

/* List configured remotes. */
void
remote_list(struct cmdq_item *item)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	union options_value		*ov;
	struct remote_config		*rc;
	struct remote_pane		*rp;
	struct remote			*r;
	char				*cause, *ssh, *tmux, *user;
	const char			*state;
	u_int				 n;

	o = options_get_only(global_options, "remote-servers");
	if (o == NULL)
		return;
	a = options_array_first(o);
	while (a != NULL) {
		ov = options_array_item_value(a);
		if ((rc = remote_parse_config(ov->string, &cause)) == NULL) {
			cmdq_print(item, "%s", cause);
			free(cause);
			a = options_array_next(a);
			continue;
		}
		r = remote_find(rc->name);
		state = (r != NULL) ? "connected" : "disconnected";
		n = 0;
		TAILQ_FOREACH(rp, &remote_panes, entry) {
			if (strcmp(rp->name, rc->name) != 0 ||
			    (rp->flags & REMOTE_PANE_CLOSING))
				continue;
			n++;
			if (r == NULL && (rp->flags & REMOTE_PANE_INACTIVE))
				state = "inactive";
		}
		ssh = cmd_stringify_argv(rc->sshargc, rc->sshargv);
		tmux = cmd_stringify_argv(rc->tmuxargc, rc->tmuxargv);
		if (rc->user != NULL)
			xasprintf(&user, "%s@", rc->user);
		else
			user = xstrdup("");
		cmdq_print(item, "%s: %s (%u panes) [%s%s ssh=%s%s%s "
		    "tmux=%s%s%s]", rc->name,
		    state, n, user,
		    rc->host, rc->ssh, *ssh == '\0' ? "" : " ", ssh, rc->tmux,
		    *tmux == '\0' ? "" : " ", tmux);
		free(user);
		free(ssh);
		free(tmux);
		remote_free_config(rc);
		a = options_array_next(a);
	}
}

/* Create a new remote window for an empty local pane. */
int
remote_pane_start(struct window_pane *wp, const char *name, int argc,
    char **argv, const char *cwd, char **cause)
{
	struct remote		*r;
	struct remote_pane	*rp;
	char			*start, *new_start, *arg;
	int			 i;

	if ((r = remote_start(name, cause)) == NULL)
		return (-1);
	rp = remote_pane_create(r, wp);

	start = xstrdup("");
	if (cwd != NULL) {
		arg = args_escape(cwd);
		xasprintf(&new_start, "%s -c %s", start, arg);
		free(arg);
		free(start);
		start = new_start;
	}
	if (argc == 0) {
		remote_pane_new_window(rp, start, 1);
		return (0);
	}
	for (i = 0; i < argc; i++) {
		arg = args_escape(argv[i]);
		xasprintf(&new_start, "%s %s", start, arg);
		free(arg);
		free(start);
		start = new_start;
	}

	arg = cmd_stringify_argv(argc, argv);
	free(rp->command);
	rp->command = parse_window_name(arg);
	free(arg);
	remote_pane_new_window(rp, start, 0);
	return (0);
}

/* Local pane resized. */
void
remote_pane_resize(struct window_pane *wp, u_int sx, u_int sy)
{
	struct remote_pane	*rp = wp->remote;

	if (rp->sx == sx && rp->sy == sy)
		return;
	rp->sx = sx;
	rp->sy = sy;
	remote_pane_send_size(rp);
}

/* Local pane destroyed: kill the remote pane too. */
void
remote_pane_destroy(struct window_pane *wp)
{
	struct remote_pane	*rp = wp->remote;
	struct remote		*r;

	if (rp == NULL)
		return;
	r = rp->remote;
	if (r != NULL &&
	    (rp->flags & (REMOTE_PANE_ID|REMOTE_PANE_STARTING)) &&
	    (~rp->flags & REMOTE_PANE_CLOSING))
		remote_send(r, NULL, 0, "kill-pane -t %%%u", rp->id);
	remote_pane_free(rp);
}

/* Is the remote of a pane connected? */
int
remote_pane_connected(struct window_pane *wp)
{
	return (wp->remote->remote != NULL);
}

/* State of the remote of a pane. */
const char *
remote_pane_state(struct window_pane *wp)
{
	struct remote_pane	*rp = wp->remote;

	if (rp->remote != NULL)
		return ("connected");
	if (rp->flags & REMOTE_PANE_INACTIVE)
		return ("inactive");
	return ("disconnected");
}

/* Name of the remote of a pane. */
const char *
remote_pane_name(struct window_pane *wp)
{
	return (wp->remote->name);
}

/* Current command of a remote pane. */
const char *
remote_pane_command(struct window_pane *wp)
{
	return (wp->remote->command);
}
