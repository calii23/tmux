#!/bin/sh

# Kitty graphics protocol. An inner tmux runs in a pane of an outer tmux and
# the bytes it writes to its terminal are saved with pipe-pane on the outer
# pane. Programs in the inner pane send graphics commands; tmux keeps the
# images and sends them to its client terminal itself.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
INNER="$TEST_TMUX -Ltest2$$"
kill_all()
{
	$OUTER kill-server 2>/dev/null
	$INNER kill-server 2>/dev/null
}
kill_all

TMP=$(mktemp)
RAW=$(mktemp)
CONF=$(mktemp)
SCRIPT=$(mktemp)
FIFO=$(mktemp -u)
trap "rm -f $TMP $RAW $CONF $SCRIPT $FIFO; kill_all" 0 1 15

# A 1x1 RGB image.
DATA=AAAA

# Placeholder with row and column 0, then one inferred from it.
PH='\364\216\273\256\314\205\314\205\364\216\273\256'

# start features script [width height]
start()
{
	kill_all
	: >$TMP
	: >$RAW

	printf 'set -g status off\nset -as terminal-features "*:%s"\n' "$1" \
	    >$CONF
	printf '%s\nsleep 60\n' "$2" >$SCRIPT
	$INNER -f$CONF new -d -x${3:-20} -y${4:-6} 'sleep 60' || exit 1
	$OUTER new -d -x${3:-20} -y${4:-6} \
	    "${TTYTERM:+TERM=$TTYTERM }$INNER attach" || exit 1
	$OUTER pipe-pane -o "cat >>$RAW" || exit 1
	sleep 1
	$INNER respawn-pane -k "sh $SCRIPT" || exit 1
	sleep 2
}

# attach_pixels seconds [session cellwidth cellheight file]: attach a 20x20
# client (default with 8x8 cells) to the inner server and save its output.
attach_pixels()
{
	python3 - "$INNER" "${5:-$RAW}" "$1" "${2:-}" "${3:-8}" "${4:-8}" \
	    <<'EOF2'
import fcntl, os, pty, select, struct, sys, termios, time
pid, fd = pty.fork()
if pid == 0:
	fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 20,
	    20 * int(sys.argv[5]), 20 * int(sys.argv[6])))
	os.environ['TERM'] = 'xterm-256color'
	target = (' -t' + sys.argv[4]) if sys.argv[4] else ''
	os.execvp('sh', ['sh', '-c', sys.argv[1] + ' attach' + target])
out = b''
end = time.time() + int(sys.argv[3])
while time.time() < end:
	if select.select([fd], [], [], 0.1)[0]:
		try:
			out += os.read(fd, 65536)
		except OSError:
			break
open(sys.argv[2], 'wb').write(out)
os.kill(pid, 9)
EOF2
}

# check name pattern: the terminal output must contain pattern.
check()
{
	grep -q -- "$2" $RAW || {
		echo "$1: output does not contain $2:"
		od -c $RAW | head -20
		exit 1
	}
}

# check_not name pattern: the terminal output must not contain pattern.
check_not()
{
	! grep -q -- "$2" $RAW || {
		echo "$1: output contains $2"
		exit 1
	}
}

# Query is answered before device attributes when supported, otherwise only
# device attributes are.
start kittygraphics "stty raw -echo
printf '\\033_Ga=q,i=31,s=1,v=1,f=24;$DATA\\033\\\\\\033[c'
dd bs=1 count=12 of=$TMP 2>/dev/null"
printf '\033_Gi=31;OK\033\\' | cmp -s - $TMP || {
	echo "query: reply does not match:"
	od -c $TMP
	exit 1
}
start RGB "stty raw -echo
printf '\\033_Ga=q,i=31,s=1,v=1,f=24;$DATA\\033\\\\\\033[c'
dd bs=1 count=3 of=$TMP 2>/dev/null"
printf '\033[?' | cmp -s - $TMP || {
	echo "query unsupported: reply does not match:"
	od -c $TMP
	exit 1
}

# A virtual placement is uploaded with the client image id and the placeholder
# colour is changed to match. The command does not set the title.
start kittygraphics "printf '\\033_Ga=T,U=1,i=5,q=2,f=24,s=1,v=1,c=2,r=1;$DATA\\033\\\\'
printf '\\033[38;5;5m$PH\\033[0m'"
check virtual 'a=t,i=1,f=24,t=d,s=1,v=1,q=2,m=0;AAAA'
check virtual 'a=p,U=1,i=1,p=8,c=2,r=1,q=2'
$OUTER capture-pane -ep | grep -q '38;2;0;0;1m' || {
	echo "virtual: placeholder colour not changed"
	$OUTER capture-pane -ep | od -c | head
	exit 1
}
[ "$($INNER display -p '#{pane_title}')" = "$(hostname)" ] || {
	echo "virtual: title changed"
	exit 1
}

# The window chooser preview shows images from the pane it copies.
start kittygraphics "printf '\\033_Ga=T,U=1,i=5,q=2,f=24,s=1,v=1,c=2,r=1;$DATA\\033\\\\'
printf '\\033[38;5;5m$PH\\033[0m'" 40 20
$INNER new-window -d \; next-window
sleep 1
$OUTER capture-pane -ep | grep -q '38;2;0;0;1m' && {
	echo "preview: placeholder still shown after window change"
	exit 1
}
$INNER choose-tree -w -O index \; send-keys Up
sleep 1
$OUTER capture-pane -ep | grep -q '38;2;0;0;1m' || {
	echo "preview: placeholder not shown"
	$OUTER capture-pane -ep | od -c | head
	exit 1
}

# Row and high byte diacritics are read: image 16777221 (high byte 1) on row
# 1 is drawn as the client image on row 1.
start kittygraphics "printf '\\033_Ga=T,U=1,i=16777221,q=2,f=24,s=1,v=1,c=1,r=2;$DATA\\033\\\\'
printf '\\033[38;5;5m\\364\\216\\273\\256\\314\\215\\314\\205\\314\\215\\033[0m'"
check diacritics 'a=p,U=1,i=1,p=8,c=1,r=2,q=2'
LC_ALL=C grep -q "$(printf '38;2;0;0;1m\364\216\273\256\314\215\314\205')" $RAW || {
	echo "diacritics: placeholder not drawn on row 1 of image 1"
	od -c $RAW | tail -20
	exit 1
}

# A placement is put at its position, removed when the window is not shown and
# put back when it is.
start kittygraphics "printf '\\033[2;3H\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'"
check put 'a=t,i=1,f=24'
check put '\[2;3H.*a=p,i=1,p=8,C=1,x=0,y=0,w=1,h=1,q=2'
: >$RAW
$INNER new-window -d \; next-window
sleep 1
check hidden 'a=d,d=i,i=1,p=8,q=2'
: >$RAW
$INNER previous-window
sleep 1
check shown 'a=p,i=1,p=8,C=1'
check_not shown 'a=t,'

# Scrolling moves the placement with the text, so it is put again.
start kittygraphics "printf '\\033[6;1H\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'
sleep 3
printf '\\n\\n'"
check scroll 'a=p,i=1,p=8,C=1'
: >$RAW
sleep 3
check scroll 'a=p,i=1,p=8,C=1'

# Commands wrapped for passthrough are handled even if it is off.
start kittygraphics "printf '\\033Ptmux;\\033\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\033\\\\\\033\\\\'"
check passthrough 'a=t,i=1,f=24'
check passthrough 'a=p,i=1,p=8,C=1'

# Huge placements are refused or cut down and do not hang the server.
start kittygraphics "printf '\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,r=4294967295,C=1;$DATA\\033\\\\'
printf '\\033[H\\033_Ga=p,i=7,p=2,q=2,r=10000;\\033\\\\'
printf '\\033[H\\033_Ga=p,i=7,p=4,q=2,r=4294967295;\\033\\\\'
printf '\\033[H\\033_Ga=p,i=7,p=3,q=2,c=10000,r=10000,C=1;\\033\\\\'"
($INNER display -p ok >$TMP 2>&1) & PID=$!
sleep 3
kill $PID 2>/dev/null && {
	echo "huge: server not responding"
	exit 1
}
grep -q ok $TMP || {
	echo "huge: server not responding"
	exit 1
}
check huge 'a=p,i=1,p=[0-9]*,C=1,.*c=20,r=6'
check_not huge 'r=4294967295'

# Repeated animation controls are merged and frames are limited.
start kittygraphics "stty raw -echo
printf '\\033_Ga=t,i=5,q=1,f=24,s=1,v=1;$DATA\\033\\\\'
i=0
while [ \$i -lt 5000 ]; do
	printf '\\033_Ga=a,i=5,q=1,s=3;\\033\\\\'
	i=\$((i + 1))
done
i=0
while [ \$i -lt 4100 ]; do
	printf '\\033_Ga=f,i=5,q=1,s=1,v=1;$DATA\\033\\\\'
	i=\$((i + 1))
done
dd bs=1 count=31 of=$TMP 2>/dev/null"
sleep 5
printf '\033_Gi=5;ENOSPC:Too many frames\033\\' | cmp -s - $TMP || {
	echo "frames: reply does not match:"
	od -c $TMP
	exit 1
}

# Placements stay with their text when it is reflowed.
start kittygraphics "printf 'xxxxxxxxxxxxxxxxxxxxxxxxx\\033[2;4H'
printf '\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'"
check reflow 'a=p,i=1,p=8,C=1'
: >$RAW
$OUTER resize-window -x 40
sleep 1
check reflow 'a=p,i=1,p=8,C=1'
: >$RAW
$OUTER resize-window -x 20
sleep 1
check reflow 'a=p,i=1,p=8,C=1'

# Images on the alternate screen are kept when it is left and entered again
# (like a program running an editor), only placements are removed.
start kittygraphics "printf '\\033[?1049h\\033_Ga=T,U=1,i=5,q=2,f=24,s=1,v=1,c=2,r=1;$DATA\\033\\\\'
printf '\\033[38;5;5m$PH\\033[0m'
sleep 3
printf '\\033[?1049l\\033[?1049h\\033[2J\\033[?1049l\\033[?1049h\\033[2J\\033[H'
printf '\\033[38;5;5m$PH\\033[0m'"
: >$RAW
sleep 3
check_not alternate 'a=d,d=I,i=1'
$OUTER capture-pane -ep | grep -q '38;2;0;0;1m' || {
	echo "alternate: placeholder not drawn with image"
	$OUTER capture-pane -ep | od -c | head
	exit 1
}

# With only columns given, rows keep the aspect ratio so a square image does
# not go past the bottom of the pane.
start kittygraphics "printf '\\033[H\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,c=20,C=1;$DATA\\033\\\\'"
check aspect 'a=p,i=1,p=8,C=1,.*r=6,q=2'
check_not aspect 'a=p,i=1,p=8,C=1,x=0,y=0,w=1,h=1,c=20,q=2'

# The high byte is inferred for a placeholder with only row and column.
start kittygraphics "printf '\\033_Ga=T,U=1,i=16777221,q=2,f=24,s=1,v=1,c=2,r=1;$DATA\\033\\\\'
printf '\\033[38;5;5m\\364\\216\\273\\256\\314\\205\\314\\205\\314\\215\\364\\216\\273\\256\\314\\205\\314\\215\\033[0m'"
check_not inferred '38;2;255;255;255m'
check inferred '38;2;0;0;1m'
LC_ALL=C grep -q "$(printf '\364\216\273\256\314\205\314\215')" $RAW || {
	echo "inferred: second placeholder not drawn"
	od -c $RAW | tail -20
	exit 1
}

# Relative placements move with their parent and go when it is deleted.
start kittygraphics "printf '\\033[2;3H\\033_Ga=T,i=7,p=1,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'
printf '\\033_Ga=p,i=7,p=2,P=7,Q=1,H=2,V=1,q=2;\\033\\\\'
sleep 3
printf '\\033[4;5H\\033_Ga=p,i=7,p=1,q=2,C=1;\\033\\\\'
sleep 3
printf '\\033_Ga=d,d=i,i=7,p=1,q=2;\\033\\\\'"
check relative 'a=p,i=1,p=16,'
: >$RAW
sleep 3
check relative 'a=p,i=1,p=8,'
check relative 'a=p,i=1,p=16,'
: >$RAW
sleep 3
check relative 'a=d,d=i,i=1,p=8,'
check relative 'a=d,d=i,i=1,p=16,'

# A file which is not a regular file is refused without blocking.
mkfifo $FIFO || exit 1
start kittygraphics "printf '\\033_Ga=T,t=f,i=7,q=2,f=24,s=1,v=1;$(printf %s $FIFO | base64)\\033\\\\'"
($INNER display -p ok >$TMP 2>&1) & PID=$!
sleep 3
kill $PID 2>/dev/null && {
	echo "fifo: server not responding"
	exit 1
}
grep -q ok $TMP || {
	echo "fifo: server not responding"
	exit 1
}
check_not fifo 'a=t,'

# A delete while an upload is unfinished cancels it.
start kittygraphics "stty raw -echo
printf '\\033_Ga=t,i=9,f=24,s=1,v=1,m=1;$DATA\\033\\\\'
printf '\\033_Ga=d,d=I,i=9\\033\\\\'
printf '\\033_Ga=p,i=9\\033\\\\\\033[c'
dd bs=1 count=34 of=$TMP 2>/dev/null"
printf '\033_Gi=9;ENOENT:No image with id 9\033\\' | cmp -s - $TMP || {
	echo "cancel: reply does not match:"
	od -c $TMP
	exit 1
}

# Deleting all placements leaves those in the history.
start kittygraphics "printf '\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'
printf '\\n\\n\\n\\n\\n\\n\\n\\n'
printf '\\033_Ga=d,d=A,q=2\\033\\\\'"
: >$RAW
$INNER copy-mode \; send -X history-top
sleep 1
check history 'a=p,i=1,p=8,'

# The size of a compressed PNG is found so it can be cropped: 32x320 PNG.
TALL=eJzrDPBz5+WS4mJgYOD19HAJAtIKDAyMDhxMQFbQ7Je+QKrD08UxpGLO27MbORkYeA5v+PO/pNpVofnA5qneq0rfM9AJsDWyPzlyyytka4o6iOvp6ueyzimhCQBWTxww
start kittygraphics "printf '\\033[H\\033_Ga=T,i=7,q=2,f=100,o=z,C=1;$TALL\\033\\\\'"
check compressed 'a=t,i=1,f=100,t=d,o=z'
check compressed 'a=p,i=1,p=8,C=1,x=0,y=0,w=32,h=[0-9]*,.*r=6,q=2'

# Like kitty, an image given both columns and rows is stretched over them,
# so half the columns show half the image: 64x64 PNG.
SQUARE=iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAIAAAAlC+aJAAAAb0lEQVR4nO3PAQkAAAyEwO9feoshgnABdLep8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3IPanc8OLDQitxAAAAAElFTkSuQmCC
start kittygraphics "printf '\\033[1;11H\\033_Ga=T,i=7,q=2,f=100,c=20,r=6,C=1;$SQUARE\\033\\\\'"
check stretched 'a=p,i=1,p=8,C=1,x=0,y=0,w=32,h=64,.*c=10,r=6,q=2'

# Clearing by scrolling moves images on the terminal, so they are put again.
TTYTERM=xterm-256color start kittygraphics "printf '\\033[2;1H\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'
sleep 3
printf '\\033[6;2H\\033[1J'"
: >$RAW
sleep 3
check scrollclear '\[[0-9]*S'
check scrollclear 'a=p,i=1,p=8,C=1'

# Natural placement size is worked out for each client's cell size: a 32x320
# PNG put while detached is cropped for a client with 8x8 cells.
kill_all
printf 'set -g status off\nset -as terminal-features "*:kittygraphics"\n' >$CONF
$INNER -f$CONF new -d -x20 -y20 "printf '\\033[H\\033_Ga=T,i=7,q=2,f=100,o=z,C=1;$TALL\\033\\\\'; sleep 60" ||
    exit 1
sleep 1
attach_pixels 3
check cellsize 'a=p,i=1,p=8,C=1,x=0,y=0,w=32,h=160,.*r=20,q=2'

# Deleting by position uses the size on the client.
kill_all
printf 'set -g status off\nset -as terminal-features "*:kittygraphics"\n' >$CONF
$INNER -f$CONF new -d -x20 -y20 "printf '\\033[H\\033_Ga=T,i=7,q=2,f=100,o=z,C=1;$TALL\\033\\\\'; sleep 3; printf '\\033_Ga=d,d=p,x=1,y=20,q=2\\033\\\\'; sleep 60" ||
    exit 1
sleep 1
attach_pixels 6
check deletesize 'a=d,d=i,i=1,p=8,q=2'

# Only clients showing the pane are used for delete sizes: a client with 4x4
# cells on another session does not make the image cover row 15.
kill_all
printf 'set -g status off\nset -as terminal-features "*:kittygraphics"\n' >$CONF
$INNER -f$CONF new -d -sA -x20 -y20 "printf '\\033[H\\033_Ga=T,i=7,q=2,f=100,o=z,C=1;$TALL\\033\\\\'; sleep 3; printf '\\033_Ga=d,d=p,x=1,y=15,q=2\\033\\\\'; sleep 60" ||
    exit 1
$INNER new -d -sB -x20 -y20 'sleep 60' || exit 1
sleep 1
attach_pixels 6 B 4 4 $TMP &
attach_pixels 6 A 16 32
wait
check otherclient 'a=p,i=1,p=8,'
check_not otherclient 'a=d,d=i,i=1,p=8'

# A delete only matches if the placement covers the cell on one client: a
# 64x64 PNG is 8x2 cells with 8x32 cells and 2x8 with 32x8 cells, so neither
# covers cell 6,6.
kill_all
printf 'set -g status off\nset -as terminal-features "*:kittygraphics"\n' >$CONF
$INNER -f$CONF new -d -sA -x20 -y20 "sleep 2; printf '\\033[H\\033_Ga=T,i=7,q=2,f=100,C=1;$SQUARE\\033\\\\'; sleep 2; printf '\\033_Ga=d,d=p,x=6,y=6,q=2\\033\\\\'; sleep 60" ||
    exit 1
attach_pixels 6 A 8 32 $TMP &
attach_pixels 6 A 32 8
wait
check eachclient 'a=p,i=1,p=8,'
check_not eachclient 'a=d,d=i,i=1,p=8'
grep -q 'a=d,d=i,i=1,p=8' $TMP && {
	echo "eachclient: deleted on the other client"
	exit 1
}

# Rows from columns are rounded up at the end: a 31x63 PNG with c=1 and 16x32
# cells takes two rows.
ODD=iVBORw0KGgoAAAANSUhEUgAAAB8AAAA/CAIAAACXYIRYAAAAMUlEQVR4nO3MsQ0AAAwCIP9/uj3CuJEwk0t2hrXdbrfb7Xa73W632+12u91ut9vtlQfLb5nJHOGaQgAAAABJRU5ErkJggg==
start kittygraphics "printf '\\033[H\\033_Ga=T,i=7,q=2,f=100,c=1;$ODD\\033\\\\'"
[ "$($INNER display -p '#{cursor_x},#{cursor_y}')" = "1,1" ] || {
	echo "rounding: cursor at $($INNER display -p '#{cursor_x},#{cursor_y}')"
	exit 1
}

# Clearing the screen deletes placements on it, even if they start in the
# history.
start kittygraphics "printf '\\033[H\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,c=1,r=4,C=1;$DATA\\033\\\\'
printf '\\033[6;1H\\n'
sleep 3
printf '\\033[2J'"
$INNER set -g scroll-on-clear off
check clear 'a=p,i=1,p=8,'
: >$RAW
sleep 3
check clear 'a=d,d=i,i=1,p=8'

# Virtual placements are sent with only the size given, so the terminal
# works out the rest for its cell size.
start kittygraphics "printf '\\033_Ga=T,U=1,i=5,q=2,f=100,c=2;$SQUARE\\033\\\\'
printf '\\033[38;5;5m$PH\\033[0m'"
check virtualsize 'a=p,U=1,i=1,p=8,c=2,q=2'

# Changing a placement between normal and virtual replaces it without a
# delete after the new one.
start kittygraphics "printf '\\033[H\\033_Ga=T,i=7,p=1,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'
sleep 3
printf '\\033_Ga=p,i=7,p=1,U=1,c=2,r=1,q=2\\033\\\\\\033[38;5;7m\\033[58;5;1m$PH\\033[0m'
sleep 3
printf '\\033[H\\033_Ga=p,i=7,p=1,q=2,C=1\\033\\\\'"
check replace 'a=p,i=1,p=8,C=1'
: >$RAW
sleep 2
$INNER refresh-client
sleep 1
check replace 'a=p,U=1,i=1,p=8,c=2,r=1'
check_not replace 'a=d,d=i,i=1,p=8'
: >$RAW
sleep 3
check replace 'a=p,i=1,p=8,C=1'
[ "$(LC_ALL=C grep -ao '_Ga=[pd][^;\]*' $RAW | tr -d '\033' | tail -1)" = "_Ga=p,i=1,p=8,C=1,x=0,y=0,w=1,h=1,q=2" ] || {
	echo "replace: last command is not the new placement:"
	LC_ALL=C grep -ao '_Ga=[pd][^;\]*' $RAW
	exit 1
}

# A placement put again keeps nothing from before: a child made virtual is
# not deleted with its old parent.
start kittygraphics "printf '\\033[H\\033_Ga=T,i=7,p=1,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'
printf '\\033_Ga=T,i=8,p=2,P=7,Q=1,H=2,q=2,f=24,s=1,v=1;$DATA\\033\\\\'
sleep 2
printf '\\033_Ga=p,i=8,p=2,U=1,c=1,r=1,q=2\\033\\\\'
sleep 2
printf '\\033_Ga=d,d=i,i=7,p=1,q=2\\033\\\\'"
check reput 'a=p,i=2,p=16,'
: >$RAW
sleep 3
check reput 'a=d,d=i,i=1,p=8'
check_not reput 'a=d,d=i,i=2,p=16'

# A placeholder with only a row, followed by an abbreviated one, is drawn.
start kittygraphics "printf '\\033_Ga=T,U=1,i=5,q=2,f=24,s=1,v=1,c=2,r=1;$DATA\\033\\\\'
printf '\\033[38;5;5m\\364\\216\\273\\256\\314\\205\\364\\216\\273\\256\\033[0m'"
check abbreviated '38;2;0;0;1m'
$OUTER capture-pane -ep | grep -q '38;2;0;0;1m' || {
	echo "abbreviated: placeholder not shown"
	exit 1
}

# Graphics anywhere in a passthrough string are handled by tmux and the rest
# is passed through in order, only if allowed.
MARK='\\033\\033]0;pt-mark\\007'
GFX="\\033\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\033\\\\"
for order in before after; do
	for allow in on off; do
		if [ $order = before ]; then
			WRAP="$MARK$GFX"
		else
			WRAP="$GFX$MARK"
		fi
		start kittygraphics "sleep 3
printf '\\033Ptmux;$WRAP\\033\\\\'"
		$INNER set -g allow-passthrough $allow
		sleep 3
		check "passthrough $order $allow" 'a=t,i=1,f=24'
		check_not "passthrough $order $allow" 'i=7'
		if [ $allow = on ]; then
			check "passthrough $order $allow" 'pt-mark'
		else
			check_not "passthrough $order $allow" 'pt-mark'
		fi
	done
done

# Nothing is sent to a terminal without the feature.
start RGB "printf '\\033_Ga=T,i=7,q=2,f=24,s=1,v=1,C=1;$DATA\\033\\\\'"
check_not unsupported '_G'

exit 0
