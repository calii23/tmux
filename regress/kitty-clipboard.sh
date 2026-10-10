#!/bin/sh

# Kitty clipboard protocol (OSC 5522). An inner tmux runs in a pane of an outer
# tmux, which is the terminal for the inner tmux and supports the protocol
# itself. A program in the inner pane sends messages and the replies it gets
# are checked, as well as the paste buffers of both servers.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
INNER="$TEST_TMUX -Ltest2$$"
# Wait for each server to exit, a big one may still be freeing memory and
# accepting connections.
kill_all()
{
	for s in "$OUTER" "$INNER"; do
		p=$($s display -p '#{pid}' 2>/dev/null)
		$s kill-server 2>/dev/null
		while [ -n "$p" ] && kill -0 $p 2>/dev/null; do
			sleep 0.1
		done
	done
}
kill_all

TMP=$(mktemp)
TMP2=$(mktemp)
CONF=$(mktemp)
SCRIPT=$(mktemp)
trap "rm -f $TMP $TMP2 $CONF $SCRIPT; kill_all" 0 1 15

# Base64 of text/plain, ., hello, world and inner.
PLAIN=dGV4dC9wbGFpbg==
DOT=Lg==
HELLO=aGVsbG8=
WORLD=d29ybGQ=
INNERB=aW5uZXI=

S='\033]5522;'
E='\033\\'

length()
{
	printf "$1" | wc -c | tr -d ' '
}

# start options send expected [send2 expected2 [detached]]
#
# Start the servers with the inner server's options. The program in the inner
# pane sends the first message and reads the reply, then the same for the
# second if given. With detached, the inner tmux has no client. $OUTER_CMD is
# run in the outer server after it starts.
start()
{
	kill_all
	: >$TMP
	: >$TMP2

	printf 'set -g status off\n%s\n' "$1" >$CONF
	{
		printf 'stty raw -echo\nsleep 1\n'
		printf 'printf %s\n' "'$2'"
		printf 'dd bs=1 count=%s of=%s 2>/dev/null\n' "$(length "$3")" \
		    $TMP
		if [ -n "$4" ]; then
			printf 'printf %s\n' "'$4'"
			printf 'dd bs=1 count=%s of=%s 2>/dev/null\n' \
			    "$(length "$5")" $TMP2
		fi
		printf 'sleep 60\n'
	} >$SCRIPT

	if [ -n "$6" ]; then
		$INNER -f$CONF new -d -x40 -y10 "sh $SCRIPT" || exit 1
	else
		$OUTER new -d -x40 -y10 "$INNER -f$CONF new 'sh $SCRIPT'" ||
		    exit 1
		$OUTER set -g set-clipboard on
		$OUTER set-buffer outer
		[ -n "$OUTER_CMD" ] && $OUTER $OUTER_CMD
	fi
	sleep 3
}

# check file expected name
check()
{
	printf "$2" | cmp -s - $1 || {
		echo "$3: reply does not match:"
		od -c $1
		exit 1
	}
}

# buffer server expected name
buffer()
{
	b=$($1 show-buffer 2>/dev/null)
	[ "$b" = "$2" ] || {
		echo "$3: buffer is '$b' not '$2'"
		exit 1
	}
}

# Support is reported with DECRPM.
start '' '\033[?5522$p' '\033[?5522;2$y'
check $TMP '\033[?5522;2$y' 'DECRQM'
start '' '\033[?5522h\033[?5522$p' '\033[?5522;1$y'
check $TMP '\033[?5522;1$y' 'DECRQM set'

# A write creates a buffer in both servers.
W="${S}type=write:id=a$E${S}type=wdata:mime=$PLAIN;aGVs${E}"
W="$W${S}type=wdata:mime=$PLAIN;bG8=$E${S}type=wdata$E"
start 'set -g set-clipboard on' "$W" "${S}type=write:status=DONE:id=a$E"
check $TMP "${S}type=write:status=DONE:id=a$E" 'write'
buffer "$INNER" hello 'write inner'
buffer "$OUTER" hello 'write outer'

# Programs may pad every message of a write, and binary data is passed on.
PNG=$(printf image/png | base64)
W="${S}type=write:id=a$E${S}type=wdata:mime=$PLAIN;aGU=$E"
W="$W${S}type=wdata:mime=$PLAIN;bGxv$E"
W="$W${S}type=wdata:mime=$PNG;$(head -c 3000 /dev/zero | base64)$E"
W="$W${S}type=wdata:mime=$PNG;$(head -c 3001 /dev/zero | base64)$E"
W="$W${S}type=wdata$E"
start 'set -g set-clipboard on' "$W" "${S}type=write:status=DONE:id=a$E"
check $TMP "${S}type=write:status=DONE:id=a$E" 'write padded'
buffer "$INNER" hello 'write padded inner'
buffer "$OUTER" hello 'write padded outer'

# Each of two writes sent together gets the terminal's reply.
W="${S}type=write:id=a$E${S}type=wdata:mime=$PLAIN;$HELLO$E${S}type=wdata$E"
W="$W${S}type=write:id=b$E${S}type=wdata:mime=$PLAIN;$WORLD$E${S}type=wdata$E"
X="${S}type=write:status=DONE:id=a$E${S}type=write:status=DONE:id=b$E"
start 'set -g set-clipboard on' "$W" "$X"
check $TMP "$X" 'write twice'
buffer "$INNER" world 'write twice inner'
buffer "$OUTER" world 'write twice outer'

# MIME types may have parameters.
M=$(printf 'text/plain; charset=utf-8' | base64)
W="${S}type=write:id=a$E${S}type=wdata:mime=$M;$HELLO$E${S}type=wdata$E"
start 'set -g set-clipboard on' "$W" "${S}type=write:status=DONE:id=a$E"
check $TMP "${S}type=write:status=DONE:id=a$E" 'write parameters'
buffer "$INNER" hello 'write parameters inner'
buffer "$OUTER" hello 'write parameters outer'

# The terminal's reply to a write goes to the pane, the buffer is still
# created.
W="${S}type=write:id=a$E${S}type=wdata:mime=$PLAIN;$HELLO$E${S}type=wdata$E"
OUTER_CMD='set -g set-clipboard external'
start 'set -g set-clipboard on' "$W" "${S}type=write:status=EPERM:id=a$E"
OUTER_CMD=
check $TMP "${S}type=write:status=EPERM:id=a$E" 'write refused'
buffer "$INNER" hello 'write refused inner'
buffer "$OUTER" outer 'write refused outer'

# An alias to plain text creates a buffer.
HTML=$(printf text/html | base64)
A="${S}type=write$E${S}type=wdata:mime=$HTML;$HELLO$E"
A="$A${S}type=walias:mime=$HTML;$PLAIN$E${S}type=wdata$E"
start 'set -g set-clipboard on' "$A" "${S}type=write:status=DONE$E"
check $TMP "${S}type=write:status=DONE$E" 'write alias'
buffer "$INNER" hello 'write alias inner'

# Too many MIME types are refused.
A="${S}type=write$E"
i=0
while [ $i -le 1024 ]; do
	A="$A${S}type=wdata:mime=$(printf x/$i | base64);$E"
	i=$((i + 1))
done
A="$A${S}type=wdata$E"
start 'set -g set-clipboard on' "$A" "${S}type=write:status=EFBIG$E"
check $TMP "${S}type=write:status=EFBIG$E" 'write too many types'

# Writes need set-clipboard on.
start 'set -g set-clipboard external' "$W" "${S}type=write:status=EPERM:id=a$E"
check $TMP "${S}type=write:status=EPERM:id=a$E" 'write external'
buffer "$OUTER" outer 'write external outer'

# Bad base64 is rejected, as is data without a MIME type.
W="${S}type=write$E${S}type=wdata:mime=$PLAIN;aGVsbG8$E${S}type=wdata$E"
start 'set -g set-clipboard on' "$W" "${S}type=write:status=EINVAL$E"
check $TMP "${S}type=write:status=EINVAL$E" 'write bad base64'
W="${S}type=write$E${S}type=wdata;$HELLO$E${S}type=wdata$E"
start 'set -g set-clipboard on' "$W" "${S}type=write:status=EINVAL$E"
check $TMP "${S}type=write:status=EINVAL$E" 'write no MIME type'

# big extra expected
#
# Write 64 MiB less one byte in 4096 byte messages, then extra padded messages
# of one byte each.
big()
{
	cat <<-EOF >$SCRIPT
	stty raw -echo
	sleep 1
	printf '${S}type=write:id=a$E'
	head -c 67108863 /dev/zero | base64 | tr -d '\n' | fold -w 4096 |
	    awk '{ printf "\\033]5522;type=wdata:mime=$PLAIN;%s\\033\\\\", \$0 }'
	i=0
	while [ \$i -lt $1 ]; do
		printf '${S}type=wdata:mime=$PLAIN;AA==$E'
		i=\$((i + 1))
	done
	printf '${S}type=wdata$E'
	dd bs=1 count=$(length "$2") of=$TMP 2>/dev/null
	sleep 60
	EOF
	kill_all
	: >$TMP
	printf 'set -g status off\nset -g set-clipboard on\n' >$CONF
	$INNER -f$CONF new -d -x40 -y10 "sh $SCRIPT" || exit 1
	i=0
	while [ $i -lt 60 ] && [ ! -s $TMP ]; do
		sleep 1
		i=$((i + 1))
	done
	sleep 1
}

# A write of exactly the largest size is taken even with padded messages.
big 1 "${S}type=write:status=DONE:id=a$E"
check $TMP "${S}type=write:status=DONE:id=a$E" 'write largest'
[ "$($INNER show-buffer | wc -c | tr -d ' ')" = 67108864 ] || {
	echo "write largest: wrong buffer size"
	exit 1
}
big 2 "${S}type=write:status=EFBIG:id=a$E"
check $TMP "${S}type=write:status=EFBIG:id=a$E" 'write too big'

# Without a terminal with the protocol, reads get the newest buffer.
R="${S}type=read:id=b;$PLAIN$E"
X="${S}type=read:status=OK:id=b$E"
X="$X${S}type=read:status=DATA:mime=$PLAIN:id=b;$INNERB$E"
X="$X${S}type=read:status=DONE:id=b$E"
start 'set-buffer inner' "$R" "$X" '' '' detached
check $TMP "$X" 'read buffer'

# Listing the MIME types.
R="${S}type=read;$DOT$E"
X="${S}type=read:status=OK$E"
X="$X${S}type=read:status=DATA:mime=$DOT;$PLAIN$E"
X="$X${S}type=read:status=DONE$E"
start 'set-buffer inner' "$R" "$X" '' '' detached
check $TMP "$X" 'read list'

# An OSC 52 read without a buffer gets an empty reply.
start 'set -g set-clipboard on' '\033]52;c;?\033\\' '\033]52;c;\033\\' '' '' detached
check $TMP '\033]52;c;\033\\' 'OSC 52 read no buffer'

# With a terminal with the protocol, OSC 52 reads go to it even with
# get-clipboard buffer.
X="\033]52;c;$(printf outer | base64)\033\\"
start 'set -g set-clipboard on; set -g get-clipboard buffer; set-buffer inner' \
    '\033]52;c;?\033\\' "$X"
check $TMP "$X" 'OSC 52 read terminal'

# Reads with get-clipboard off are refused.
R="${S}type=read:id=b;$PLAIN$E"
start 'set -g get-clipboard off' "$R" "${S}type=read:status=EPERM:id=b$E"
check $TMP "${S}type=read:status=EPERM:id=b$E" 'read off'

# Otherwise reads go to the terminal, here the outer tmux, even with
# get-clipboard buffer.
X="${S}type=read:status=OK:id=b$E"
X="$X${S}type=read:status=DATA:mime=$PLAIN:id=b;$(printf outer | base64)$E"
X="$X${S}type=read:status=DONE:id=b$E"
start 'set -g get-clipboard buffer; set-buffer inner' "$R" "$X"
check $TMP "$X" 'read request'

# With get-clipboard both the reply also creates a buffer.
start 'set -g get-clipboard both; set-buffer inner' "$R" "$X"
check $TMP "$X" 'read both'
buffer "$INNER" outer 'read both inner'

# A paste event goes to the active pane if it has mode 5522, and a read with
# its password goes to the terminal even with get-clipboard off.
P="${S}type=read:status=OK:pw=c2VjcmV0$E"
P="$P${S}type=read:status=DATA:mime=$DOT;$PLAIN$E"
P="$P${S}type=read:status=DONE$E"
R="${S}type=read:pw=c2VjcmV0:name=UGFzdGUgZXZlbnQ=;$PLAIN$E"
X="${S}type=read:status=OK$E"
X="$X${S}type=read:status=DATA:mime=$PLAIN;$(printf outer | base64)$E"
X="$X${S}type=read:status=DONE$E"
start 'set -g get-clipboard off' '\033[?5522h' "$P" \
    "$R" "$X" &
sleep 2
printf "$P" | $OUTER load-buffer -b event -
$OUTER paste-buffer -S -b event
wait
sleep 1
check $TMP "$P" 'paste event'
check $TMP2 "$X" 'paste read'

exit 0
