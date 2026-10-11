#!/bin/sh

# Kitty desktop notifications protocol (OSC 99). An inner tmux client runs in
# a pane of an outer tmux. What the inner tmux sends to its terminal is copied
# from the outer pane with pipe-pane and terminal replies are typed into it
# with send-keys. A program in the inner pane sends messages and the replies
# it gets are checked.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
INNER="$TEST_TMUX -Ltest2$$ -f/dev/null"
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
OUT=$(mktemp)
PAT=$(mktemp)
SCRIPT=$(mktemp)
trap "rm -f $TMP $OUT $PAT $SCRIPT; kill_all" 0 1 15

S='\033]99;'
E='\033\\'
CAPS='a=focus,report:c=1:o=always,unfocused,invisible:p=title,body,?,alive'

length()
{
	printf "$1" | wc -c | tr -d ' '
}

# Type a terminal reply into the inner tmux.
reply()
{
	$OUTER send-keys -t:0 -H $(printf "$1" | od -An -v -tx1)
	sleep 1
}

# start send [expected [delay]]
#
# Start the servers. The program in the inner pane sends a message, after a
# delay if given, and reads the reply if one is expected. The terminal gives
# its capabilities first.
start()
{
	kill_all
	: >$TMP
	: >$OUT

	{
		printf 'stty raw -echo\nsleep 2\n'
		[ -n "$3" ] && printf 'sleep %s\n' "$3"
		printf 'printf %s\n' "'$1'"
		[ -n "$2" ] && printf 'dd bs=1 count=%s of=%s 2>/dev/null\n' \
		    "$(length "$2")" $TMP
		printf 'sleep 60\n'
	} >$SCRIPT

	$OUTER new -d -x40 -y10 \
	    "$INNER new 'sh $SCRIPT' \\; set -g status off" || exit 1
	$OUTER pipe-pane -o "cat >>$OUT"
	sleep 1
	reply "${S}i=tmux:p=?;$CAPS$E"
	P="tmux-$($INNER display -p '#{pid}')-"
	sleep 2
}

# sent expected name
sent()
{
	printf "$1" >$PAT
	grep -aqF -f $PAT $OUT || {
		echo "$2: not sent to terminal:"
		od -c $OUT | tail -20
		exit 1
	}
}

# unsent expected name
unsent()
{
	printf "$1" >$PAT
	if grep -aqF -f $PAT $OUT; then
		echo "$2: sent to terminal"
		exit 1
	fi
}

# check expected name
check()
{
	printf "$1" | cmp -s - $TMP || {
		echo "$2: reply does not match:"
		od -c $TMP
		exit 1
	}
}

# Base64 of hello, world and some icon data.
HELLO=aGVsbG8=
WORLD=d29ybGQ=
ICON=aWNvbg==

# The capabilities of the terminal are given to the pane.
start "${S}i=q:p=?;$E" "${S}i=q:p=?;$CAPS$E"
check "${S}i=q:p=?;$CAPS$E" 'query'

# A notification is sent under a tmux id.
start "${S};hello$E"
sent "${S}i=${P}1:e=1:a=focus,report:c=1:p=title;$HELLO$E" 'title'

# Title and body in chunks, the base64 split anywhere.
start "${S}i=n:d=0:e=1;aGVs$E${S}i=n:d=0:e=1;bG8$E${S}i=n:p=body;world$E"
sent "${S}i=${P}1:e=1:a=focus,report:c=1:p=title:d=0;$HELLO$E" 'chunks'
sent "${S}i=${P}1:e=1:a=focus,report:c=1:p=body;$WORLD$E" 'chunks body'

# Empty chunks, as sent by kitten notify.
start "${S}i=k:d=0:f=a2l0dGVu;$E${S}i=k:d=0:e=1;$HELLO$E${S}i=k;$E"
sent "${S}i=${P}1:e=1:a=focus,report:c=1:f=a2l0dGVu:p=title;$HELLO$E" 'empty'

# Legacy OSC 9 text is a notification, other numbered commands are not.
start '\033]9;1;2\033\\\033]9;4;1;50\033\\\033]9;hello\033\\'
sent "${S}i=${P}1:e=1:a=focus,report:c=1:p=title;$HELLO$E" 'legacy'

# Other keys are passed on and the occasion kept for the active pane.
start "${S}i=n:u=2:o=unfocused:a=-focus:n=aW5mbw==:n=d2Fybg==;hello$E"
sent "${S}i=${P}1:e=1:a=-focus,report:c=1:o=unfocused:n=aW5mbw==:n=d2Fybg==:u=2:p=title;$HELLO$E" \
    'keys'

# An icon is kept for its cache id.
start "${S}i=a:g=x:p=icon:e=1;$ICON$E${S}i=b:g=x;hello$E"
sent "${S}i=${P}2:e=1:a=focus,report:c=1:g=x:p=title:d=0;$HELLO$E" 'cache'
sent "${S}i=${P}2:e=1:a=focus,report:c=1:g=x:p=icon;$ICON$E" 'cache icon'

# Activation is reported to the pane with buttons.
start "${S}i=n:a=report;hello$E" "${S}i=n;2$E"
reply "${S}i=${P}1;2$E"
check "${S}i=n;2$E" 'button'

# Close events go to the pane.
start "${S}i=n:c=1;hello$E" "${S}i=n:p=close;$E"
reply "${S}i=${P}1:p=close;$E"
check "${S}i=n:p=close;$E" 'close'

# An explicit c=0 in a later chunk cancels close events.
start "${S}i=n:d=0:c=1;hello$E${S}i=n:c=0;world$E" "${S}i=n:p=close;$E"
reply "${S}i=${P}1:p=close;$E"
check '' 'close cancelled'

# Closing a notification from the pane.
start "${S}i=n;hello$E${S}i=n:p=close;$E"
sent "${S}i=${P}1:p=close;$E" 'close request'

# Alive queries go to the terminal and the ids are changed back. The outer tmux
# is the terminal and has nothing live.
start "${S}i=n;hello$E${S}i=a:p=alive;$E" "${S}i=a:p=alive;$E"
sent "${S}i=${P}2:p=alive;$E" 'alive'
check "${S}i=a:p=alive;$E" 'alive reply'

# A notification sent after an alive query is not forgotten by its reply.
start "${S}i=o;hello$E${S}i=a:p=alive;$E${S}i=n:a=report;world$E" \
    "${S}i=a:p=alive;$E${S}i=n;$E"
reply "${S}i=${P}3;$E"
check "${S}i=a:p=alive;$E${S}i=n;$E" 'alive newer'

# Icon names and types from every chunk are kept.
start "${S}i=c:d=0:n=aW5mbw==:t=YQ==;$E${S}i=c:n=d2Fybg==:t=Yg==;hello$E"
sent "${S}i=${P}1:e=1:a=focus,report:c=1:n=aW5mbw==:n=d2Fybg==:t=YQ==:t=Yg==:p=title;$HELLO$E" \
    'repeated keys'

# Base64 is decoded before plain text is added.
start "${S}i=m:d=0:e=1;aGk$E${S}i=m;!$E"
sent "${S}i=${P}1:e=1:a=focus,report:c=1:p=title;aGkh$E" 'mixed'

# Metadata counts toward the size limit.
N=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
N=$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N$N
C="${S}i=big:d=0:n=$N:t=$N;$E"
C=$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C
C=$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C$C
start "$C${S}i=big;hello$E${S}i=b;world$E"
unsent "${S}i=${P}1:" 'metadata limit'
sent "${S}i=${P}2:e=1:a=focus,report:c=1:p=title;$WORLD$E" 'metadata limit next'

# An unfinished update keeps the actions of the notification shown.
start "${S}i=n:a=report,-focus;hello$E${S}i=n:d=0;world$E" "${S}i=n;$E"
reply "${S}i=${P}1;$E"
check "${S}i=n;$E" 'update actions'

# Activation selects the pane, and o= is always if the pane is not visible.
start "${S}i=n:o=invisible;hello$E" '' 3
$INNER new-window
sleep 3
sent "${S}i=${P}1:e=1:a=focus,report:c=1:o=always:p=title;$HELLO$E" 'hidden'
reply "${S}i=${P}1;$E"
[ "$($INNER display -p '#{window_index}')" = 0 ] || {
	echo 'focus: window not selected'
	exit 1
}

exit 0
