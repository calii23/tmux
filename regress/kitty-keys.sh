#!/bin/sh

# Kitty keyboard protocol. Applications in panes push flags and get keys
# encoded as asked. With nested tmux, the inner tmux turns the protocol on in
# the outer pane, parses the keys and passes them on to its own pane.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest$$ -f/dev/null"
OUTER="$TEST_TMUX -Ltest1$$ -f/dev/null"
INNER="$TEST_TMUX -Ltest2$$ -f/dev/null"
$TMUX kill-server 2>/dev/null
$OUTER kill-server 2>/dev/null
$INNER kill-server 2>/dev/null

TMP=$(mktemp)
trap "rm -f $TMP; $TMUX kill-server 2>/dev/null; $OUTER kill-server 2>/dev/null; $INNER kill-server 2>/dev/null" 0 1 15

# Command to run in a pane: push the flags and show input as it arrives.
reader()
{
	echo "stty raw -echo; printf '\\033[>$1u'; cat -v"
}

# check name tmux expected
check()
{
	$2 capturep -p | head -1 >$TMP
	echo "$3" | cmp -s - $TMP || {
		echo "$1: got: $(cat $TMP)"
		echo "$1: expected: $3"
		exit 1
	}
}

# keys flags expected keys...
keys()
{
	flags=$1
	expected=$2
	shift 2

	$TMUX kill-server 2>/dev/null
	$TMUX new -d -x80 -y4 "$(reader $flags)" || exit 1
	sleep 1
	$TMUX send-keys "$@" || exit 1
	sleep 1
	check "flags $flags keys $*" "$TMUX" "$expected"
}

# Without flags, keys are as normal.
keys 0 '^A^[x^[OP' C-a M-x F1

# Disambiguate: only ambiguous keys are changed.
keys 1 'a^[[97;5u^[[27u^M^[[A^[[120;3u^[[P^?^[[97;6u^[[A^[[1;5A' \
    a C-a Escape Enter Up M-x F1 BSpace C-S-a Up C-Up
keys 1 'A^[[9;2u^[[3~^[[1;2P^[[57399u' A BTab DC S-F1 KP0

# Event types: send-keys has only presses.
keys 3 '^[[97;5u' C-a

# Alternate keys.
keys 5 'A^[[97:65;6u' A C-S-a

# Alternate keys alone do not change normal keys.
keys 4 '^C^[^[x' C-c Escape M-x

# All keys as escape codes.
keys 8 '^[[97u^[[97;2u^[[13u^[[9u^[[127u^[[27u' a A Enter Tab BSpace Escape
keys 12 '^[[97:65;2u^[[49u' A 1

# All keys and associated text.
keys 24 '^[[97;;97u^[[97;2;65u^[[97;5u' a A C-a

# Query and pop.
$TMUX kill-server 2>/dev/null
$TMUX new -d -x80 -y4 \
    "stty raw -echo; printf '\\033[>5u\\033[>3u\\033[?u\\033[<u\\033[?u\\033[=2;2u\\033[?u\\033[<9u\\033[?u'; cat -v" ||
    exit 1
sleep 1
check "query" "$TMUX" '^[[?3u^[[?5u^[[?7u^[[?0u'

# Separate flags for the alternate screen.
$TMUX kill-server 2>/dev/null
$TMUX new -d -x80 -y4 \
    "stty raw -echo; printf '\\033[>1u\\033[?1049h\\033[?u\\033[>8u\\033[?1049l\\033[?u'; cat -v" ||
    exit 1
sleep 1
check "alternate" "$TMUX" '^[[?0u^[[?1u'

# Option off ignores the protocol.
$TMUX kill-server 2>/dev/null
$TMUX -f/dev/null new -d -x80 -y4 "sleep 1; $(reader 1)" || exit 1
$TMUX set -g kitty-keys off
sleep 2
$TMUX send-keys C-a
sleep 1
check "off" "$TMUX" '^A'

# Nested: start an inner tmux in a pane of an outer tmux, with a reader in
# the inner pane.
nested()
{
	$OUTER kill-server 2>/dev/null
	$INNER kill-server 2>/dev/null

	$OUTER new -d -x80 -y10 \
	    "$INNER new -x80 -y10 \"$(reader $1)\" \\; set status off" ||
	    exit 1
	sleep 2
	mode=$($OUTER display -p '#{pane_key_mode}')
	[ "$mode" = "$2" ] || {
		echo "nested $1: outer pane mode $mode, expected $2"
		exit 1
	}
}

# Send literal sequences to the inner tmux as a kitty terminal would.
inject()
{
	$OUTER send-keys -l "$(printf "$1")" || exit 1
	sleep 1
}

# The inner tmux turns the protocol on in the outer pane and keys get there.
nested 1 'Kitty 5'
$OUTER send-keys C-a Escape a C-Up || exit 1
sleep 1
check "nested 1" "$INNER" '^[[97;5u^[[27ua^[[1;5A'

# Releases go to the pane that had the press.
nested 3 'Kitty 7'
inject '\033[97;5u\033[97;5:2u\033[97;5:3u\033[120;1:3u'
check "nested 3" "$INNER" '^[[97;5u^[[97;5:2u^[[97;5:3u'

# A repeat in another pane leaves the release with the pane that had the
# press.
nested 3 'Kitty 7'
$INNER splitw -d "$(reader 3)" || exit 1
sleep 1
inject '\033[97;5u'
$INNER selectp -t:.1 || exit 1
sleep 1
inject '\033[97;5:2u\033[97;5:3u'
check "nested repeat" "$INNER" '^[[97;5:2u'
$INNER selectp -t:.0 || exit 1
check "nested repeat release" "$INNER" '^[[97;5u^[[97;5:3u'

# Keys which fail to parse in the kitty form are still extended keys.
nested 1 'Kitty 5'
inject '\033[27;5;97~'
check "nested xterm" "$INNER" '^[[97;5u'

# With event types alone, repeats of other than text keys keep the type.
nested 2 'Kitty 6'
inject '\033[A\033[1;1:2A\033[97;1:2u'
check "nested event types" "$INNER" '^[[A^[[1;1:2Aa'

# Modified keys produce no text, so their repeats keep the type, and keys
# which produce text have no releases without all keys.
nested 2 'Kitty 6'
inject '\033[99;5u\033[99;5:2u'
check "nested ctrl repeat" "$INNER" '^C^[[99;5:2u'
nested 3 'Kitty 7'
inject '\033[97u\033[97;1:3u\033[120;5u'
check "nested text release" "$INNER" 'a^[[120;5u'

# Super and Hyper keys do not match bindings for the key without them.
nested 1 'Kitty 5'
$INNER bind -n a new-window || exit 1
inject '\033[97;9u\033[97;17u'
[ "$($INNER display -p '#{session_windows}')" = 1 ] || {
	echo "nested super: binding ran"
	exit 1
}
check "nested super" "$INNER" '^[[97;9u^[[97;17u'

# Super and Hyper keys do not cancel a modal pane.
nested 1 'Kitty 5'
$INNER bind -n F5 new-pane -OKD "$(reader 1)" || exit 1
inject '\033[15~'
inject '\033[99;13u\033[27;9u'
[ "$($INNER display -p '#{window_panes}')" = 2 ] || {
	echo "nested super modal: pane closed"
	exit 1
}
check "nested super modal" "$INNER" '^[[99;13u^[[27;9u'

# Unicode keys beyond the private use area reach panes without the protocol.
nested 1 'Kitty 5'
$INNER set -s extended-keys on || exit 1
$INNER splitw -d "stty raw -echo; printf '\\033[>4;2m'; cat -v" || exit 1
sleep 1
$INNER set -g synchronize-panes on || exit 1
inject '\033[65345;5u'
$INNER set -g synchronize-panes off || exit 1
check "nested unicode" "$INNER" '^[[65345;5u'
$INNER selectp -t:.1 || exit 1
check "nested unicode legacy" "$INNER" '^[[27;5;65345~'

# Long associated text is passed on whole.
nested 25 'Kitty 29'
TEXT=97:98:99:100:101:102:103:104:105:106:107:108:109:110:111:112:113
inject "\\033[97;;${TEXT}u"
check "nested long text" "$INNER" "^[[97;;${TEXT}u"

# A modal pane capturing all keys gets releases while its binding waits.
nested 11 'Kitty 31'
$INNER bind -n F5 new-pane -OKW "$(reader 11)" || exit 1
inject '\033[15~'
inject '\033[97u\033[97;1:3u'
check "nested modal" "$INNER" '^[[97u^[[97;1:3u'

# Releases of a key binding are not seen.
nested 3 'Kitty 7'
inject '\033[98;5u\033[98;5:3u'
inject '\033[99u\033[99;1:3u'
[ "$($INNER display -p '#{session_windows}')" = 2 ] || {
	echo "nested binding: no new window"
	exit 1
}
$INNER selectw -t:0 || exit 1
inject '\033[120u'
check "nested binding" "$INNER" 'x'

# Modifier keys alone go to the pane and do not leave the prefix table, and
# the shifted key is used for bindings. Without event types there are no
# releases.
nested 8 'Kitty 28'
inject '\033[98;5u\033[57441;2u\033[39:34;2;34u'
[ "$($INNER display -p '#{window_panes}')" = 2 ] || {
	echo "nested shift: no split"
	exit 1
}
$INNER selectp -t:.0 || exit 1
inject '\033[57441;2u\033[97:65;2;65u\033[57441;1:3u'
check "nested shift" "$INNER" '^[[57441;2u^[[57441;2u^[[97;2u'

# Text from the terminal goes to panes without the protocol, and send-keys
# -l sends text as it is.
nested 9 'Kitty 29'
$INNER splitw -d "$(reader 0)" || exit 1
sleep 1
$INNER set -g synchronize-panes on || exit 1
inject '\033[97;65;65u\033[50:64;2;64u\033[99;5u'
$INNER set -g synchronize-panes off || exit 1
$INNER send-keys -l 'x;y' || exit 1
sleep 1
check "nested text" "$INNER" '^[[97;65u^[[50;2u^[[99;5ux;y'
$INNER selectp -t:.1 || exit 1
check "nested legacy" "$INNER" 'A@^C'

# The shifted key replaces Shift with other modifiers too.
nested 5 'Kitty 5'
$INNER splitw -d "$(reader 0)" || exit 1
sleep 1
$INNER set -g synchronize-panes on || exit 1
inject '\033[97:65;4u\033[47:63;6u\033[9;6u'
$INNER set -g synchronize-panes off || exit 1
check "nested shifted" "$INNER" '^[[97:65;4u^[[47:63;6u^[[9;6u'
$INNER selectp -t:.1 || exit 1
check "nested shifted legacy" "$INNER" '^[A^?^[[Z'

# Shift with other modifiers is kept for bindings.
nested 1 'Kitty 5'
$INNER bind -n C-Space new-window || exit 1
inject '\033[32:32;6u'
[ "$($INNER display -p '#{session_windows}')" = 1 ] || {
	echo "nested shift binding: binding ran"
	exit 1
}

# Switching panes keeps releases on while keys are held.
nested 3 'Kitty 7'
$INNER splitw -d "$(reader 0)" || exit 1
sleep 1
inject '\033[97;5u'
$INNER selectp -t:.1 || exit 1
sleep 1
mode=$($OUTER display -p '#{pane_key_mode}')
[ "$mode" = 'Kitty 7' ] || {
	echo "nested held: outer pane mode $mode, expected Kitty 7"
	exit 1
}
inject '\033[97;5:3u'
mode=$($OUTER display -p '#{pane_key_mode}')
[ "$mode" = 'VT10x' ] || {
	echo "nested held release: outer pane mode $mode, expected VT10x"
	exit 1
}
$INNER selectp -t:.0 || exit 1
check "nested held" "$INNER" '^[[97;5u^[[97;5:3u'

# Held keys do not stop the flags being upgraded for another pane.
nested 3 'Kitty 7'
$INNER splitw -d "$(reader 11)" || exit 1
sleep 1
inject '\033[97;5u'
$INNER selectp -t:.1 || exit 1
sleep 1
mode=$($OUTER display -p '#{pane_key_mode}')
[ "$mode" = 'Kitty 31' ] || {
	echo "nested held upgrade: outer pane mode $mode, expected Kitty 31"
	exit 1
}
$OUTER send-keys b || exit 1
sleep 1
check "nested held upgrade" "$INNER" '^[[98u'
inject '\033[97;5:3u'
mode=$($OUTER display -p '#{pane_key_mode}')
[ "$mode" = 'Kitty 31' ] || {
	echo "nested held upgrade release: outer pane mode $mode, expected Kitty 31"
	exit 1
}
$INNER selectp -t:.0 || exit 1
check "nested held upgrade release" "$INNER" '^[[97;5u^[[97;5:3u'

exit 0
