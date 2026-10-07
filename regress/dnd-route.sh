#!/bin/sh

# Drag and drop protocol (OSC 72): routing between the terminal and panes. An
# inner tmux runs in a pane of an outer tmux and the test plays the terminal:
# messages from the terminal are sent to the inner tmux with send-keys and
# what the inner tmux sends to the terminal is collected with pipe-pane. Each
# inner pane runs a program that keeps what it is sent in a file and sends
# what is appended to another file.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
INNER="$TEST_TMUX -Ltest2$$"
$OUTER kill-server 2>/dev/null
$INNER kill-server 2>/dev/null

DIR=$(mktemp -d)
trap "rm -rf $DIR; $OUTER kill-server 2>/dev/null; $INNER kill-server 2>/dev/null" 0 1 15

# The program in each inner pane: $DIR/send.N is sent, $DIR/recv.N is received.
for i in 0 1; do
	: >$DIR/send.$i
	: >$DIR/recv.$i
	printf 'stty raw -echo\ntail -f %s &\nexec cat >%s\n' \
	    $DIR/send.$i $DIR/recv.$i >$DIR/pane.$i
done
: >$DIR/term

cat <<EOF >$DIR/conf
set -g status off
set -as terminal-features '*:dnd'
EOF

$OUTER new -d -x40 -y10 "sleep 1; exec $INNER -f$DIR/conf new 'sh $DIR/pane.0'" ||
    exit 1
$OUTER pipe-pane -O "cat >>$DIR/term"
sleep 2
$INNER splitw -h -l20 "sh $DIR/pane.1" || exit 1
sleep 1
CW=$($INNER display -p "#{client_cell_width}")

# Show escapes so they can be compared as text.
show()
{
	tr '\033' '^' <$1
}

# send pane message: the program in a pane sends a message.
send()
{
	printf "$2" >>$DIR/send.$1
	sleep 0.5
}

# term message: the terminal sends a message to tmux.
term()
{
	$OUTER send-keys -H $(printf "$1" | od -An -tx1) || exit 1
	sleep 0.5
}

# expect file text: the file must have text, given with ^ for escape.
expect()
{
	show $1 | grep -qF "$2" || {
		echo "missing: $2"
		echo "have: $(show $1)"
		exit 1
	}
}

# reject file text: the file must not have text.
reject()
{
	show $1 | grep -qF "$2" && {
		echo "unexpected: $2"
		echo "have: $(show $1)"
		exit 1
	}
	return 0
}

# Both panes accept drops; the terminal is told the union of their types.
send 0 '\033]72;t=a:i=3;text/plain text/uri-list\033\\'
expect $DIR/term '^]72;t=a;text/plain text/uri-list^\'
send 1 '\033]72;t=a:i=4;image/png text/plain\033\\'
expect $DIR/term '^]72;t=a;text/plain text/uri-list image/png^\'

# A move over the left pane goes to it with the same position and its id,
# with the MIME list only the first time.
term '\033]72;t=m:x=2:y=1:X=20:Y=40:o=3;text/plain image/png \033\\'
expect $DIR/recv.0 '^]72;t=m:x=2:y=1:X=20:Y=40:o=3:i=3;text/plain image/png ^\'
term '\033]72;t=m:x=3:y=1:X=30:Y=40:o=3;text/plain image/png \033\\'
expect $DIR/recv.0 '^]72;t=m:x=3:y=1:X=30:Y=40:o=3:i=3^\'

# The pane's answer goes to the terminal without the id.
send 0 '\033]72;t=m:o=1:i=3;text/plain\033\\'
expect $DIR/term '^]72;t=m:o=1;text/plain^\'

# Moving to the right pane: the left pane is told the drag left, the
# terminal is told to reject until the right pane answers, and the right pane
# gets a position inside it.
: >$DIR/term
term '\033]72;t=m:x=25:y=2:X=2000:Y=80:o=3;text/plain image/png \033\\'
expect $DIR/recv.0 '^]72;t=m:x=-1:y=-1:i=3^\'
expect $DIR/term '^]72;t=m^\'
expect $DIR/recv.1 "^]72;t=m:x=5:y=2:X=$((2000 - 20 * CW)):Y=80:o=3:i=4;text/plain image/png ^\\"

# Asking for data before a drop is refused by tmux.
send 1 '\033]72;t=r:x=1:i=4\033\\'
expect $DIR/recv.1 '^]72;t=R:x=1:i=4;EPERM:'

# Drop on the right pane, then its requests and the data are passed through.
term '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/plain image/png \033\\'
expect $DIR/recv.1 "^]72;t=M:x=5:y=2:X=$((2000 - 20 * CW)):Y=80:o=1:i=4;text/plain image/png ^\\"
send 1 '\033]72;t=r:x=1:i=4\033\\'
expect $DIR/term '^]72;t=r:x=1^\'
term '\033]72;t=r:x=1:m=0;aGVsbG8=\033\\\033]72;t=r:x=1\033\\'
expect $DIR/recv.1 '^]72;t=r:x=1:i=4;aGVsbG8=^\^]72;t=r:x=1:i=4^\'
send 1 '\033]72;t=r:o=1:i=4\033\\'
expect $DIR/term '^]72;t=r:o=1^\'

# After the drop is done, data is not passed on.
send 1 '\033]72;t=r:x=1:i=4\033\\'
expect $DIR/recv.1 '^]72;t=R:x=1:i=4;EPERM:'

# The left pane stops accepting: it no longer gets moves.
send 0 '\033]72;t=A\033\\'
expect $DIR/term '^]72;t=a;image/png text/plain^\'
: >$DIR/recv.0
term '\033]72;t=m:x=2:y=1:X=20:Y=40:o=3;text/plain \033\\'
reject $DIR/recv.0 '72;t=m'

# A long MIME list from the terminal in chunks is put back together.
term '\033]72;t=m:x=25:y=1:o=3:m=1;text/plain \033\\\033]72;t=m:x=25:y=1:o=3;image/png \033\\'
expect $DIR/recv.1 ':i=4;text/plain image/png ^\'

# The left pane offers drags; the terminal is told tmux does.
: >$DIR/term
: >$DIR/recv.0
: >$DIR/recv.1
send 0 '\033]72;t=o:x=1:i=7\033\\'
expect $DIR/term '^]72;t=o:x=1^\'

# A gesture over the right pane, which does not offer, goes nowhere.
term '\033]72;t=o:x=25:y=1:X=2000:Y=40\033\\'
reject $DIR/recv.1 '72;t=o'

# A gesture over the left pane goes to it, then its offer, data and start
# go to the terminal.
term '\033]72;t=o:x=2:y=1:X=20:Y=40\033\\'
expect $DIR/recv.0 '^]72;t=o:x=2:y=1:X=20:Y=40:i=7^\'
send 0 '\033]72;t=o:o=1:i=7;text/plain\033\\'
expect $DIR/term '^]72;t=o:o=1;text/plain^\'
send 0 '\033]72;t=p:x=0:i=7;aGk=\033\\\033]72;t=p:x=0:i=7\033\\'
expect $DIR/term '^]72;t=p;aGk=^\^]72;t=p^\'
send 0 '\033]72;t=P:x=-1:i=7\033\\'
expect $DIR/term '^]72;t=P:x=-1^\'

# The terminal's replies and requests go to the pane, and its data back.
term '\033]72;t=E;OK\033\\'
expect $DIR/recv.0 '^]72;t=E:i=7;OK^\'
term '\033]72;t=k:x=1:Y=4294967295\033\\'
expect $DIR/recv.0 '^]72;t=k:x=1:Y=4294967295:i=7^\'
send 0 '\033]72;t=k:X=4294967295:i=7;aGk=\033\\'
expect $DIR/term '^]72;t=k:X=4294967295;aGk=^\'
term '\033]72;t=e:x=5:y=0\033\\'
expect $DIR/recv.0 '^]72;t=e:x=5:i=7^\'
send 0 '\033]72;t=e:y=0:i=7;aGk=\033\\\033]72;t=e:y=0:i=7\033\\'
expect $DIR/term '^]72;t=e;aGk=^\^]72;t=e^\'

# Once the drag finishes, the pane cannot start another without a gesture.
term '\033]72;t=e:x=4:y=0\033\\'
expect $DIR/recv.0 '^]72;t=e:x=4:i=7^\'
send 0 '\033]72;t=P:x=-1:i=7\033\\'
expect $DIR/recv.0 '^]72;t=E:x=-1:i=7;EPERM:'

# A machine id for remote drags is passed on to the terminal.
: >$DIR/term
send 0 '\033]72;t=o:x=1:i=7;1:abcd\033\\'
expect $DIR/term '^]72;t=o:x=1;1:abcd^\'

# While a pane sends a chunked message, nothing else goes to the terminal.
: >$DIR/recv.0
term '\033]72;t=o:x=2:y=1:X=20:Y=40\033\\'
expect $DIR/recv.0 '^]72;t=o:x=2:y=1:X=20:Y=40:i=7^\'
: >$DIR/term
send 0 '\033]72;t=p:x=0:m=1:i=7;aGVs\033\\'
send 1 '\033]72;t=A\033\\'
reject $DIR/term '72;t=A'
send 0 '\033]72;t=p:x=0:i=7;bG8=\033\\'
expect $DIR/term '^]72;t=p:m=1;aGVs^\^]72;t=p;bG8=^\^]72;t=m^\^]72;t=A^\'

# A chunked error from the terminal is passed on whole before it ends the drag.
: >$DIR/recv.0
term '\033]72;t=E:m=1;EPERM:\033\\\033]72;t=E;denied\033\\'
expect $DIR/recv.0 '^]72;t=E:i=7:m=1;EPERM:^\^]72;t=E:i=7;denied^\'
send 0 '\033]72;t=P:x=-1:i=7\033\\'
expect $DIR/recv.0 '^]72;t=E:x=-1:i=7;EPERM:'

# A read-only client is told tmux no longer takes part and what it sends does
# not reach the panes.
send 1 '\033]72;t=a:i=4;image/png text/plain\033\\'
IC=$($INNER list-clients -F '#{client_name}')
: >$DIR/term
: >$DIR/recv.1
$INNER switch-client -r -c "$IC" || exit 1
sleep 0.5
expect $DIR/term '^]72;t=A^\'
expect $DIR/term '^]72;t=o:x=2^\'
term '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/plain \033\\'
reject $DIR/recv.1 '72;t=M'
$INNER switch-client -r -c "$IC" || exit 1
sleep 0.5
expect $DIR/term '^]72;t=a;image/png text/plain^\'
expect $DIR/term '^]72;t=o:x=1;1:abcd^\'

# A drag cancelled while the pane sends a chunked message ends the message,
# so the terminal is not left waiting for the rest.
: >$DIR/recv.0
term '\033]72;t=o:x=2:y=1:X=20:Y=40\033\\'
expect $DIR/recv.0 '^]72;t=o:x=2:y=1:X=20:Y=40:i=7^\'
: >$DIR/term
send 0 '\033]72;t=p:x=0:m=1:i=7;aGVs\033\\'
term '\033]72;t=E;EFBIG\033\\'
expect $DIR/recv.0 '^]72;t=E:i=7;EFBIG^\'
expect $DIR/term '^]72;t=p:m=1;aGVs^\^]72;t=p^\'
send 1 '\033]72;t=A\033\\'
expect $DIR/term '^]72;t=A^\'
send 0 '\033]72;t=p:x=0:i=7;bG8=\033\\'
reject $DIR/term 'bG8='
send 1 '\033]72;t=a:i=4;image/png text/plain\033\\'
expect $DIR/term '^]72;t=a;image/png text/plain^\'

# An error from the terminal ends the drop.
: >$DIR/recv.1
term '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/plain \033\\'
expect $DIR/recv.1 '^]72;t=M:'
term '\033]72;t=R:x=1;EIO\033\\'
expect $DIR/recv.1 '^]72;t=R:x=1:i=4;EIO^\'
: >$DIR/term
send 1 '\033]72;t=r:x=1:i=4\033\\'
expect $DIR/recv.1 '^]72;t=R:x=1:i=4;EPERM:'
reject $DIR/term '72;t=r'

# Ids are unsigned 32-bit.
: >$DIR/recv.0
send 0 '\033]72;t=q:i=4294967295\033\\'
expect $DIR/recv.0 '^]72;t=q:i=4294967295^\'

# Offering again with no id forgets the old one.
send 0 '\033]72;t=o:x=2:i=7\033\\'
send 0 '\033]72;t=o:x=1\033\\'
: >$DIR/recv.0
term '\033]72;t=o:x=2:y=1:X=20:Y=40\033\\'
expect $DIR/recv.0 '^]72;t=o:x=2:y=1:X=20:Y=40^\'

# A drop finished while another pane sends a chunked message reaches the
# terminal once the chunked message is done.
term '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/plain \033\\'
: >$DIR/term
send 0 '\033]72;t=p:x=0:m=1;aGVs\033\\'
send 1 '\033]72;t=r:o=1:i=4\033\\'
send 0 '\033]72;t=p:x=0;bG8=\033\\'
expect $DIR/term '^]72;t=p:m=1;aGVs^\^]72;t=p;bG8=^\^]72;t=r:o=1^\'
term '\033]72;t=e:x=4:y=0\033\\'

# term2 message: the second client's terminal sends a message to tmux.
term2()
{
	$OUTER send-keys -t:1 -H $(printf "$1" | od -An -tx1) || exit 1
	sleep 0.5
}

# A second client cannot drop on a pane the first client has dropped on, and
# the pane's requests still go to the first client.
: >$DIR/term2
$OUTER neww -d "exec $INNER attach" || exit 1
$OUTER pipe-pane -t:1 -O "cat >>$DIR/term2"
sleep 1
term '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/plain \033\\'
: >$DIR/recv.1
: >$DIR/term
term2 '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/uri-list \033\\'
reject $DIR/recv.1 '72;t=M'
expect $DIR/term2 '^]72;t=r^\'
send 1 '\033]72;t=r:x=1:i=4\033\\'
expect $DIR/term '^]72;t=r:x=1^\'
reject $DIR/term2 '72;t=r:x=1'

# If the pane finishes the drop while the terminal sends chunked data, the
# data is ended and the rest is not passed on.
: >$DIR/recv.1
term '\033]72;t=r:x=1:m=1;aGVs\033\\'
send 1 '\033]72;t=r:o=1:i=4\033\\'
term '\033]72;t=r:x=1;bG8=\033\\'
expect $DIR/recv.1 '^]72;t=r:x=1:i=4:m=1;aGVs^\^]72;t=r:i=4^\'
reject $DIR/recv.1 'bG8='

# Once the drop is finished the second client can drop on the pane.
term2 '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/uri-list \033\\'
expect $DIR/recv.1 ';text/uri-list ^\'
send 1 '\033]72;t=r:o=1:i=4\033\\'

# A pane that stops accepting is no longer under a drag, so another client
# can drop on it once it accepts again.
term '\033]72;t=m:x=25:y=2:o=1;text/plain \033\\'
: >$DIR/term
send 1 '\033]72;t=A\033\\'
expect $DIR/term '^]72;t=m^\'
send 1 '\033]72;t=a:i=4;image/png text/plain\033\\'
: >$DIR/recv.1
term2 '\033]72;t=M:x=25:y=2:X=2000:Y=80:o=1;text/uri-list \033\\'
expect $DIR/recv.1 '^]72;t=M:'

# A long MIME list is merged without holding up the server.
awk 'BEGIN {
	for (i = 0; i < 30000; i++)
		s = s sprintf("application/x-type-%d ", i % 29999)
	for (i = 1; i <= length(s); i += 4096) {
		m = (i + 4096 <= length(s)) ? ":m=1" : ""
		printf "\033]72;t=a:i=4%s;%s\033\\", m, substr(s, i, 4096)
	}
}' >>$DIR/send.1
sleep 0.2
: >$DIR/ok
($INNER display -p ok >$DIR/ok &)
sleep 2
grep -q ok $DIR/ok || { echo "server blocked"; exit 1; }
sleep 1

# Holding a drag over a window in the status line switches to it, but only
# if it stays there long enough.
term '\033]72;t=m:x=-1:y=-1\033\\'
$INNER set -g status on
$INNER set -g status-left ''
$INNER set -g window-status-separator ''
$INNER set -g window-status-format 'XXXXXXXXXX'
$INNER set -g window-status-current-format 'XXXXXXXXXX'
$INNER neww -d 'sleep 100'
sleep 0.5
SY=$(($($INNER display -p '#{client_height}') - 1))
term "\\033]72;t=m:x=12:y=$SY;text/plain\\033\\\\\\033]72;t=m:x=2:y=$SY\\033\\\\"
sleep 1
[ "$($INNER display -p '#{window_index}')" = 0 ] || {
	echo "switched window too early"
	exit 1
}
$OUTER send-keys -H $(printf "\\033]72;t=m:x=12:y=$SY\\033\\\\" | od -An -tx1)
sleep 0.2
[ "$($INNER display -p '#{window_index}')" = 0 ] || {
	echo "switched window too early"
	exit 1
}
sleep 1
[ "$($INNER display -p '#{window_index}')" = 1 ] || {
	echo "did not switch window"
	exit 1
}

# It can be turned off, and the time changed.
$INNER selectw -t0
$INNER set -g drag-select-window off
term "\\033]72;t=m:x=12:y=$SY\\033\\\\"
sleep 1
[ "$($INNER display -p '#{window_index}')" = 0 ] || {
	echo "switched window when turned off"
	exit 1
}
term '\033]72;t=m:x=2:y=0\033\\'
$INNER set -g drag-select-window on
$INNER set -g drag-select-window-time 2000
term "\\033]72;t=m:x=12:y=$SY\\033\\\\"
sleep 1
[ "$($INNER display -p '#{window_index}')" = 0 ] || {
	echo "switched window before drag-select-window-time"
	exit 1
}
sleep 1.5
[ "$($INNER display -p '#{window_index}')" = 1 ] || {
	echo "did not switch window after drag-select-window-time"
	exit 1
}

# Once no pane accepts drops a pending switch is cancelled.
$INNER selectw -t0
term "\\033]72;t=m:x=12:y=$SY\\033\\\\"
send 1 '\033]72;t=A\033\\'
sleep 2.5
[ "$($INNER display -p '#{window_index}')" = 0 ] || {
	echo "switched window after drops stopped"
	exit 1
}
send 1 '\033]72;t=a:i=4;image/png text/plain\033\\'
$INNER set -gu drag-select-window-time
term '\033]72;t=m:x=-1:y=-1\033\\'
$INNER set -g status off
$INNER selectw -t0
send 1 '\033]72;t=r:o=1:i=4\033\\'

# big type file: write a chunked message of type with a MIME list over the
# limit, starting with text/plain.
big()
{
	awk -v t="$1" 'BEGIN {
		s = "text/plain "
		for (i = 0; i < 55000; i++)
			s = s sprintf("application/x-big-%d ", i)
		for (i = 1; i <= length(s); i += 4096) {
			m = (i + 4096 <= length(s)) ? ":m=1" : ""
			printf "\033]72;%s%s;%s\033\\", t, m, substr(s, i, 4096)
		}
	}' >$2
}

# termfile file: the terminal sends what is in a file to tmux.
termfile()
{
	$INNER set -s escape-time 2000
	$OUTER load-buffer -b big $1 || exit 1
	$OUTER paste-buffer -dSr -b big || exit 1
	sleep 3
	$INNER set -su escape-time
}

# A MIME list over the limit from a pane is ignored, not cut short, and the
# pane can still accept drops.
send 1 '\033]72;t=A\033\\'
: >$DIR/term
big t=a:i=4 $DIR/big
cat $DIR/big >>$DIR/send.1
sleep 3
reject $DIR/term 'x-big-'
send 1 '\033]72;t=a:i=4;image/png text/plain\033\\'
expect $DIR/term '^]72;t=a;image/png text/plain^\'

# A MIME list over the limit from the terminal is not passed on and the move
# is rejected, but the next move works.
: >$DIR/term
: >$DIR/recv.1
big t=m:x=25:y=2:o=1 $DIR/big
termfile $DIR/big
reject $DIR/recv.1 'x-big-'
expect $DIR/term '^]72;t=m^\'
term '\033]72;t=m:x=25:y=2:o=1;text/plain image/png \033\\'
expect $DIR/recv.1 ':i=4;text/plain image/png ^\'

# The same for a drop, which is cancelled.
: >$DIR/term
: >$DIR/recv.1
big t=M:x=25:y=2:o=1 $DIR/big
termfile $DIR/big
reject $DIR/recv.1 'x-big-'
reject $DIR/recv.1 '72;t=M'
expect $DIR/recv.1 '^]72;t=m:x=-1:y=-1:i=4^\'
expect $DIR/term '^]72;t=r^\'
term '\033]72;t=M:x=25:y=2:o=1;text/plain \033\\'
expect $DIR/recv.1 '^]72;t=M:x=5:y=2:o=1:i=4;text/plain ^\'
send 1 '\033]72;t=r:o=1:i=4\033\\'

# When the inner tmux exits the terminal is told it no longer takes drops
# or offers drags.
: >$DIR/term
$INNER kill-server
sleep 1
expect $DIR/term '^]72;t=A^\'
expect $DIR/term '^]72;t=o:x=2^\'

exit 0
