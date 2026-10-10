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

#include <stdlib.h>

#include "tmux.h"

/*
 * Connect to, disconnect from, clean up and list remote servers.
 */

static enum cmd_retval	cmd_connect_remote_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_connect_remote_entry = {
	.name = "connect-remote",
	.description = "Connect to a remote server.",
	.alias = "connect",

	.args = { "d", 1, 1, NULL },
	.usage = "[-d] remote-name",

	.flags = 0,
	.exec = cmd_connect_remote_exec
};

const struct cmd_entry cmd_disconnect_remote_entry = {
	.name = "disconnect-remote",
	.description = "Disconnect from a remote server.",
	.alias = "disconnect",

	.args = { "", 1, 1, NULL },
	.usage = "remote-name",

	.flags = 0,
	.exec = cmd_connect_remote_exec
};

const struct cmd_entry cmd_cleanup_remote_entry = {
	.name = "cleanup-remote",
	.description = "Kill panes of a remote server which is not connected.",
	.alias = NULL,

	.args = { "", 1, 1, NULL },
	.usage = "remote-name",

	.flags = 0,
	.exec = cmd_connect_remote_exec
};

const struct cmd_entry cmd_list_remotes_entry = {
	.name = "list-remotes",
	.description = "List remote servers.",
	.alias = "lsr",

	.args = { "", 0, 0, NULL },
	.usage = "",

	.flags = CMD_READONLY|CMD_AFTERHOOK,
	.exec = cmd_connect_remote_exec
};

static enum cmd_retval
cmd_connect_remote_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	char		*cause;

	if (cmd_get_entry(self) == &cmd_list_remotes_entry) {
		remote_list(item);
		return (CMD_RETURN_NORMAL);
	}

	if (cmd_get_entry(self) == &cmd_cleanup_remote_entry) {
		if (remote_cleanup(args_string(args, 0), &cause) != 0) {
			cmdq_error(item, "%s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}

	if (cmd_get_entry(self) == &cmd_disconnect_remote_entry) {
		if (remote_disconnect(args_string(args, 0), &cause) != 0) {
			cmdq_error(item, "%s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}

	if (remote_connect(item, args_string(args, 0), args_has(args, 'd'),
	    &cause) != 0) {
		cmdq_error(item, "%s", cause);
		free(cause);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_WAIT);
}
