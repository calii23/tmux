#!/bin/sh

# Drag and drop protocol (OSC 72): support query. An inner tmux runs in a pane
# of an outer tmux. A program in the inner pane sends the query followed by a
# device attributes request, as the specification says, and the first reply
# tells it whether the protocol is supported.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
MIDDLE="$TEST_TMUX -Ltest2$$"
INNER="$TEST_TMUX -Ltest3$$"
kill_all()
{
	$OUTER kill-server 2>/dev/null
	$MIDDLE kill-server 2>/dev/null
	$INNER kill-server 2>/dev/null
}
kill_all

TMP=$(mktemp)
CONF=$(mktemp)
CONF2=$(mktemp)
SCRIPT=$(mktemp)
trap "rm -f $TMP $CONF $CONF2 $SCRIPT; kill_all" 0 1 15

# query features request count expected [detect]
#
# With detect, a middle tmux with the features runs between the outer and
# inner tmux and the inner tmux has no features forced, so it has to detect
# support from the middle tmux's reply to its own query.
query()
{
	kill_all
	: >$TMP

	printf 'set -g status off\nset -as terminal-features "*:%s"\n' "$1" \
	    >$CONF
	printf 'set -g status off\n' >$CONF2
	printf 'stty raw -echo\nprintf %s\ndd bs=1 count=%s of=%s 2>/dev/null\nsleep 60\n' \
	    "'$2\\033[c'" "$3" "$TMP" >$SCRIPT
	if [ -n "$5" ]; then
		$OUTER new -d -x20 -y4 \
		    "$MIDDLE -f$CONF new \"$INNER -f$CONF2 new 'sh $SCRIPT'\"" ||
		    exit 1
		sleep 3
	else
		$OUTER new -d -x20 -y4 "$INNER -f$CONF new 'sh $SCRIPT'" ||
		    exit 1
		sleep 2
	fi

	printf "$4" | cmp -s - $TMP || {
		echo "$1 $2: reply does not match:"
		od -c $TMP
		exit 1
	}
}

# Supported: the query is answered before device attributes, echoing the id.
query 'dnd' '\033]72;t=q:i=5\033\\' 14 '\033]72;t=q:i=5\033\\'
query 'dnd' '\033]72;t=q\033\\' 10 '\033]72;t=q\033\\'
query 'dnd' '\033]72;t=q:i=7\007' 13 '\033]72;t=q:i=7\007'

# An invalid query is ignored.
query 'dnd' '\033]72;t=Z\033\\' 3 '\033[?'

# Not supported: device attributes come first.
query 'RGB' '\033]72;t=q:i=5\033\\' 3 '\033[?'

# Detected from the terminal's reply to the query tmux sends when it starts.
query 'dnd' '\033]72;t=q:i=5\033\\' 14 '\033]72;t=q:i=5\033\\' detect
query 'RGB' '\033]72;t=q:i=5\033\\' 3 '\033[?' detect

exit 0
