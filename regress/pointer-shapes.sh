#!/bin/sh

# Pointer shapes (OSC 22). First a program in a pane changes the shape stacks
# and the query replies are checked. Then an inner tmux runs in a pane of an
# outer tmux and the sequences it sends to its terminal are checked as the
# mouse moves between panes.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
INNER="$TEST_TMUX -Ltest2$$"
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
CONF=$(mktemp)
SCRIPT=$(mktemp)
trap "rm -f $TMP $OUT $CONF $SCRIPT; kill_all" 0 1 15

S='\033]22;'
E='\033\\'

# send message [reply]
#
# Add a message to the script and read the reply if one is expected.
send()
{
	printf 'printf %s\n' "'$S$1$E'" >>$SCRIPT
	if [ -n "$2" ]; then
		n=$(printf "$S$2$E" | wc -c | tr -d ' ')
		printf 'dd bs=1 count=%s 2>/dev/null >>%s\n' $n $TMP >>$SCRIPT
		printf "$S$2$E" >>$OUT
	fi
}

# Pane side.
: >$TMP
: >$OUT
printf 'stty raw -echo\nsleep 1\n' >$SCRIPT
send '?__current__' '0'
send '>pointer,wait'
send '?__current__' 'wait'
send '<'
send '?__current__' 'pointer'
send '=text'
send '?__current__' 'text'
send 'crosshair'
send '?__current__,__default__' 'crosshair,text'
send '?pointer,no-such-name,zoom-in' '1,0,1'
send '>bad!name'
send '?__current__' 'crosshair'
send '>no-such-name'
send '=no-such-name'
send '?__current__' 'crosshair'
send '>hand2'
send '?__current__,hand2' 'pointer,0'
send '<'
printf "printf '\\\\033[?1049h'\\n" >>$SCRIPT
send '?__current__' '0'
send '>help'
send '?__current__' 'help'
printf "printf '\\\\033[?1049l'\\n" >>$SCRIPT
send '?__current__' 'crosshair'
printf "printf '\\\\033[?1049h'\\n" >>$SCRIPT
send '?__current__' 'help'
printf "printf '\\\\033[?1049l'\\n" >>$SCRIPT
send '>pointer,wait'
send '='
send '?__current__' '0'
send '<'
send '?__current__' 'pointer'
send '>move'
printf "printf '\\\\033c'\\n" >>$SCRIPT
send '?__current__' '0'
printf 'sleep 60\n' >>$SCRIPT

$INNER -f/dev/null new -d -x40 -y10 "sh $SCRIPT" || exit 1
sleep 3
if ! cmp -s $TMP $OUT; then
	echo "pane replies differ:"
	od -c $TMP
	od -c $OUT
	exit 1
fi
kill_all

# Terminal side, support is detected with a query. The left pane pushes a
# shape, the right has none. Zooming the right pane hides the left without the
# mouse moving. Read-only clients show shapes as well.
cat <<EOF >$CONF
set -g status off
set -g mouse on
EOF
printf '%s\nexec cat\n' "printf '$S>pointer$E'" >$SCRIPT

mouse()
{
	$OUTER send-keys -l "$(printf '\033[<35;%s;3M' $1)"
	sleep 1
}
count()
{
	n=$(grep -o "$(printf "\033]22;$1\033")" $TMP | wc -l | tr -d ' ')
	if [ "$n" != 2 ]; then
		echo "expected 2 of $1 ($2), got $n"
		od -c $TMP
		exit 1
	fi
}
for flags in "" "-r"; do
	: >$TMP
	$INNER -f$CONF new -d -x40 -y10 "sh $SCRIPT" \; splitw -h cat ||
	    exit 1
	$OUTER new -d -x40 -y10 "$INNER attach $flags" || exit 1
	$OUTER pipe-pane -O "cat >>$TMP"
	sleep 1

	mouse 5
	mouse 30
	mouse 5
	$INNER resizep -Z -t%1
	sleep 1

	if ! grep -q "$(printf '\033\\[?1003h')" $TMP; then
		echo "missing all motion mouse mode ($flags)"
		od -c $TMP
		exit 1
	fi
	count '>pointer' "$flags"
	count '<' "$flags"
	kill_all
done

# Only a positive reply to the query enables support. script(1) is the
# terminal and answers the query.
if script -q /dev/null true </dev/null >/dev/null 2>&1; then
	term() { script -q /dev/null $INNER attach; }
else
	term() { script -qc "$INNER attach" /dev/null; }
fi
for reply in 0 1; do
	$INNER -f$CONF new -d -x40 -y10 cat || exit 1
	{
		sleep 1
		printf "${S}$reply$E"
		sleep 1
		$INNER display -p '#{client_termfeatures}' >$TMP
		$INNER detach
	} | term >/dev/null 2>&1
	if grep -q pointershapes $TMP; then
		[ $reply = 1 ] || { echo "reply 0 enabled support"; exit 1; }
	else
		[ $reply = 0 ] || { echo "reply 1 did not enable support"; exit 1; }
	fi
	kill_all
done

# SGR pixels mouse mode (1016) reports the middle of the cell.
: >$TMP
R='\033[<35;41;49M'
{
	printf 'stty raw -echo\n'
	printf "printf '\\033[?1003h\\033[?1016h'\n"
	printf 'dd bs=1 count=12 of=%s 2>/dev/null\n' $TMP
	printf 'sleep 60\n'
} >$SCRIPT
$OUTER new -d -x40 -y10 "$INNER -f$CONF new 'sh $SCRIPT'" || exit 1
sleep 1
$OUTER send-keys -l "$(printf '\033[<35;3;2M')"
sleep 1
if [ "$(cat $TMP)" != "$(printf "$R")" ]; then
	echo "SGR pixels mouse event differs:"
	od -c $TMP
	exit 1
fi

exit 0
