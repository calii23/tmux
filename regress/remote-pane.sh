#!/bin/sh

# Remote panes. A second server stands in for the remote: an ssh wrapper logs
# its arguments and runs the remote command locally, which starts the second
# server in control mode.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
L="$TEST_TMUX -Ltest1$$ -f/dev/null"
R="$TEST_TMUX -Ltest2$$ -f/dev/null"
O="$TEST_TMUX -Ltest3$$ -f/dev/null"
S="$TEST_TMUX -Ltest4$$ -f/dev/null"
kill_all()
{
	for s in "$O" "$L" "$R" "$S"; do
		p=$($s display -p '#{pid}' 2>/dev/null)
		$s kill-server 2>/dev/null
		while [ -n "$p" ] && kill -0 $p 2>/dev/null; do
			sleep 0.1
		done
	done
}
TMP=$(mktemp -d)
kill_all
trap 'kill_all; rm -rf $TMP' 0 1 15

cat <<EOF >$TMP/ssh
#!/bin/sh
for i in "\$@"; do
	echo "[\$i]"
done >$TMP/args
for i in "\$@"; do
	last="\$i"
done
exec sh -c "\$last"
EOF
chmod +x $TMP/ssh

fail()
{
	echo "FAIL: $*"
	exit 1
}

# Wait until a command prints something matching a pattern.
wait_for()
{
	n=0
	while [ $n -lt 50 ]; do
		eval "$1" 2>/dev/null | grep -q -- "$2" && return 0
		sleep 0.1
		n=$((n + 1))
	done
	fail "$3"
}

# Find the local pane showing some text.
find_pane()
{
	for p in $($L list-panes -s -t r -F '#{pane_id}'); do
		if $L capture-pane -p -t $p | grep -q -- "$1"; then
			echo $p
			return
		fi
	done
}

# Print all remote panes.
remote_all()
{
	for p in $($R list-panes -a -F '#{pane_id}'); do
		$R capture-pane -p -t $p
	done
}

$R new -d -s work -x 80 -y 24 cat \; split-window -d cat || exit 1
$R send -t work:0.0 before Enter
$R send -t work:0.1 second Enter
wait_for "$R capture-pane -p -t work:0.0" '^before$' "remote setup"
wait_for "$R capture-pane -p -t work:0.1" '^second$' "remote setup"
RP=$($R display -p -t work:0.0 '#{pane_id}')
RP2=$($R display -p -t work:0.1 '#{pane_id}')

$L new -d -s main -x 100 -y 30 cat || exit 1
$R set -g default-command cat
ENTRY="r -H host -u user -s $TMP/ssh -o '-p 2222 -o \"A=b c\"' -t $TEST_TMUX -a '-Ltest2$$ -f/dev/null'"
$L set -s 'remote-servers[0]' "$ENTRY"
$L set -s 'remote-servers[1]' "bad -x"
$L set -s 'remote-servers[2]' "bad2 -o 'x"

# Bad entries give errors.
$L connect-remote bad 2>/dev/null && fail "bad remote"
$L connect-remote nothere 2>/dev/null && fail "unknown remote"
[ "$($L list-remotes | grep -c '^bad remote')" = "2" ] ||
	fail "list bad remotes"

# Connecting adopts the unattached panes and the control session pane.
$L connect-remote r || fail "connect-remote"
[ "$($L list-panes -s -t r -F '#{pane_remote}' | sort | uniq -c | awk '{print $1 $2}')" = "3r" ] ||
	fail "adopted panes"
$L list-remotes | grep -q '^r: connected (3 panes)' || fail "list-remotes"

# The ssh command line is built from the entry.
H=$(hostname | cut -d. -f1)
cat <<EOF | cmp -s - $TMP/args || fail "ssh arguments"
[-T]
[-p]
[2222]
[-o]
[A=b c]
[-l]
[user]
[--]
[host]
['$TEST_TMUX' '-Ltest2$$' '-f/dev/null' -C new-session -A -s '$H']
EOF

# Each remote pane is alone in its own window.
[ "$($R list-panes -a -F '#{window_panes}' | sort -u)" = "1" ] ||
	fail "remote panes not split"

# Existing contents are copied.
wait_for "find_pane before" '%' "seeded contents"
LP=$(find_pane before)

# Output and keys.
$R send -t $RP fromremote Enter
wait_for "$L capture-pane -p -t $LP" '^fromremote$' "remote output"
$L send -t $LP fromlocal Enter
wait_for "$R capture-pane -p -t $RP" '^fromlocal$' "local keys"

# The local pane size is the remote pane size.
$L join-pane -h -s $LP -t main:0 || fail "join-pane"
W=$($L display -p -t $LP '#{pane_width}x#{pane_height}')
wait_for "$R display -p -t $RP '#{pane_width}x#{pane_height}'" "^$W\$" \
	"remote size"

# New remote panes from split-window.
NP=$($L split-window -P -F '#{pane_id}' -r r -t $LP cat) || fail "split-window -r"
[ "$($L display -p -t $NP '#{pane_remote}')" = "r" ] || fail "new pane remote"
wait_for "$R list-panes -a -F '#{pane_id}' | sort -u | wc -l" '4' "new remote pane"
$L send -t $NP newpane Enter
wait_for remote_all '^newpane$' "new pane keys"

# Killing the local pane kills the remote pane.
$L kill-pane -t $NP
wait_for "$R list-panes -a -F '#{pane_id}' | sort -u | wc -l" '3' "kill local pane"

# Disconnecting keeps the panes and their contents with a marker.
wait_for "find_pane second" '%' "seeded second pane"
LP2=$(find_pane second)
$L disconnect-remote r || fail "disconnect-remote"
wait_for "$L display -p -t $LP '#{pane_remote_connected}'" '^0$' \
	"disconnected"
wait_for "$L capture-pane -p -t $LP" 'remote r disconnected' \
	"disconnect marker"
[ "$($L display -p -t $LP '#{pane_dead}')" = "0" ] ||
	fail "dead after disconnect"
$L capture-pane -p -t $LP | grep -q '^fromlocal$' ||
	fail "contents after disconnect"
$L list-remotes | grep -q '^r: disconnected' || fail "list disconnected"

# Input is dropped while disconnected.
$L send -t $LP lost Enter

# choose-tree shows the state. It is drawn for a client, so attach one from
# a third server.
$O new -d -s out -x 120 -y 40 "$L attach -t main" || exit 1
$L new-window -t main:9 cat
$L choose-tree -Nw -t main:9 || fail "choose-tree"
wait_for "$O capture-pane -p -t out" '\[r: disconnected\]' \
	"choose-tree disconnected"
$L kill-window -t main:9

# A remote pane killed while disconnected is gone after reconnecting.
$R kill-pane -t $RP2

# Creating a new remote pane reconnects and revives all panes.
NP=$($L split-window -P -F '#{pane_id}' -r r -t $LP cat) ||
	fail "split-window -r reconnect"
wait_for "$L display -p -t $LP '#{pane_remote_connected}'" '^1$' "revived"
wait_for "$L capture-pane -p -t $LP" '^fromlocal$' "revived contents"
$L capture-pane -p -t $LP | grep -q 'disconnected' && fail "marker kept"
$R capture-pane -p -t $RP | grep -q '^lost$' &&
	fail "input while disconnected"
wait_for "$L list-panes -a -F '#{pane_id}' | grep -c $LP2" '^0$' \
	"killed while disconnected"
for p in $($L list-panes -a -F '#{?pane_remote,#{pane_id},}'); do
	wait_for "$L display -p -t $p '#{pane_remote_connected}'" '^1$' \
		"revived $p"
done
$L send -t $LP again Enter
wait_for "$R capture-pane -p -t $RP" '^again$' "keys after reconnect"
$L kill-pane -t $NP

# choose-tree shows connected panes.
$L new-window -t main:9 cat
$L choose-tree -Nw -t main:9 || fail "choose-tree"
wait_for "$O capture-pane -p -t out" '\[r\]' "choose-tree connected"
$L kill-window -t main:9

# connect-remote also revives disconnected panes.
$L disconnect-remote r || fail "disconnect-remote"
wait_for "$L display -p -t $LP '#{pane_remote_connected}'" '^0$' \
	"disconnected again"
$L connect-remote -d r || fail "reconnect"
wait_for "$L display -p -t $LP '#{pane_remote_connected}'" '^1$' \
	"revived again"
wait_for "$L capture-pane -p -t $LP" '^again$' "revived contents again"

# Killing the remote pane kills the local pane.
$R kill-pane -t $RP
wait_for "$L list-panes -t main:0 -F '#{pane_id}' | grep -c $LP" '^0$' \
	"kill remote pane"

# A short idle time: the remote becomes inactive when no window with its
# panes is current.
$L disconnect-remote r || fail "disconnect-remote"
$L set -s 'remote-servers[0]' "$ENTRY -i 1"
$L connect-remote -d r || fail "connect idle"
LP=$($L list-panes -s -t r -F '#{pane_id}' | head -1)
wait_for "$L display -p -t $LP '#{pane_remote_connected}'" '^1$' "idle revived"
$L join-pane -h -s $LP -t main:0 || fail "join-pane idle"
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^connected$' \
	"connected before idle"
$L new-window -t main:8 cat
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^inactive$' \
	"idle inactive"
wait_for "$L capture-pane -p -t $LP" 'remote r inactive' "inactive marker"
[ "$($L display -p -t $LP '#{pane_dead}')" = "0" ] || fail "dead when idle"
$L list-remotes | grep -q '^r: inactive' || fail "list inactive"

# choose-tree shows inactive.
$L choose-tree -Nw -t main:8 || fail "choose-tree"
$L send -t main:8 Up Right
wait_for "$O capture-pane -p -t out" '\[r: inactive\]' "choose-tree inactive"
$L send -t main:8 q

# Switching to the window reconnects.
$L select-window -t main:0
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^connected$' \
	"reconnect on window"
$L capture-pane -p -t $LP | grep -q 'inactive' && fail "inactive marker kept"

# A lost connection makes the remote inactive; a key reconnects it.
for p in $($R list-clients -F '#{client_pid}'); do
	kill $p
done
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^inactive$' \
	"lost inactive"
sleep 1.5
[ "$($L display -p -t $LP '#{pane_remote_state}')" = "inactive" ] ||
	fail "reconnected without trigger"
$O send-keys -t out z
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^connected$' \
	"reconnect on key"

# After disconnect-remote, neither window changes nor keys reconnect.
$L disconnect-remote r || fail "disconnect-remote"
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^disconnected$' \
	"explicit disconnected"
$L select-window -t main:8
$L select-window -t main:0
$O send-keys -t out z
sleep 1.5
[ "$($L display -p -t $LP '#{pane_remote_state}')" = "disconnected" ] ||
	fail "reconnected after disconnect-remote"

# disconnect-remote turns inactive panes into disconnected ones.
$L connect-remote -d r || fail "connect"
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^connected$' \
	"connected again"
$L select-window -t main:8
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^inactive$' \
	"inactive again"
$L disconnect-remote r || fail "disconnect inactive"
[ "$($L display -p -t $LP '#{pane_remote_state}')" = "disconnected" ] ||
	fail "inactive to disconnected"
$L select-window -t main:0
sleep 1.5
[ "$($L display -p -t $LP '#{pane_remote_state}')" = "disconnected" ] ||
	fail "disconnected reconnected"

# cleanup-remote removes the panes locally but not remotely; they are
# adopted again on connect.
N=$($R list-panes -a -F '#{pane_id}' | sort -u | wc -l | tr -d ' ')
$L cleanup-remote r || fail "cleanup-remote"
[ "$($L list-panes -a -F '#{pane_remote}' | grep -c '^r$')" = "0" ] ||
	fail "cleanup left panes"
[ "$($R list-panes -a -F '#{pane_id}' | sort -u | wc -l | tr -d ' ')" = "$N" ] ||
	fail "cleanup killed remote panes"
$L cleanup-remote nothere 2>/dev/null && fail "cleanup unknown remote"
$L connect-remote -d r || fail "connect after cleanup"
wait_for "$L list-panes -a -F '#{pane_remote}' | grep -c '^r$'" "^$N\$" \
	"adopted after cleanup"

# No idle disconnect for the rest.
$L disconnect-remote r || fail "disconnect-remote"
$L set -s 'remote-servers[0]' "$ENTRY"
$L connect-remote -d r || fail "connect"
LP=$($L list-panes -a -F '#{?pane_remote,#{pane_id},}' | grep . | head -1)
wait_for "$L display -p -t $LP '#{pane_remote_state}'" '^connected$' \
	"connected for respawn"

# Remote panes cannot be respawned, neither alone nor with their window.
$L respawn-pane -k -t $LP cat 2>/dev/null && fail "respawn-pane remote"
$L new-window -d -t main:7 cat
$L join-pane -d -h -s $LP -t main:7 || fail "join-pane respawn"
$L respawn-window -k -t main:7 cat 2>/dev/null && fail "respawn-window remote"
[ "$($L display -p -t $LP '#{pane_remote}:#{pane_pid}')" = "r:0" ] ||
	fail "remote pane respawned"
[ "$($L list-panes -t main:7 | wc -l | tr -d ' ')" = "2" ] ||
	fail "respawn-window destroyed panes"

# Exit status of remote panes is passed on, also with remote remain-on-exit.
cat <<EOF >$TMP/exit.sh
read x
[ "\$x" = term ] && kill -TERM \$\$
exit \$x
EOF
$R set -g remain-on-exit on
for c in 7 0 term; do
	NP=$($L split-window -P -F '#{pane_id}' -r r -t main:7 sh $TMP/exit.sh) ||
		fail "split-window -r exit"
	$L set -p -t $NP remain-on-exit failed
	wait_for "$L display -p -t $NP '#{pane_remote_state}'" '^connected$' \
		"exit pane connected"
	sleep 0.5
	$L send -t $NP $c Enter
	case $c in
	0)
		wait_for "$L list-panes -a -F '#{pane_id}' | grep -c $NP" '^0$' \
			"exit 0 removed"
		;;
	term)
		wait_for "$L display -p -t $NP '#{pane_dead}:#{pane_dead_signal}'" \
			'^1:[Tt][Ee][Rr][Mm]$' "exit signal"
		;;
	*)
		wait_for "$L display -p -t $NP '#{pane_dead}:#{pane_dead_status}'" \
			"^1:$c\$" "exit status"
		;;
	esac
done
wait_for "$R list-panes -a -F '#{pane_dead}' | grep -c 1" '^0$' \
	"dead remote panes killed"
$R set -g remain-on-exit off

# Drawing attributes are kept when a pane is adopted.
cat <<'EOF' >$TMP/pen.sh
printf '\033[31mRED\n'
read x
printf 'NEXT\n'
cat
EOF
$L disconnect-remote r || fail "disconnect-remote"
$R new -d -s pen "sh $TMP/pen.sh" || fail "pen session"
wait_for "$R capture-pane -p -t pen" '^RED$' "pen remote"
$L connect-remote -d r || fail "connect pen"
wait_for "find_pane RED" '%' "pen adopted"
PP=$(find_pane RED)
wait_for "$L display -p -t $PP '#{pane_remote_state}'" '^connected$' \
	"pen connected"
sleep 0.5
$R send -t pen Enter
wait_for "$L capture-pane -p -t $PP" '^NEXT$' "pen output"
RL=$($R capture-pane -p -e -t pen | grep NEXT)
LL=$($L capture-pane -p -e -t $PP | grep NEXT)
[ "$RL" = "$LL" ] || fail "pen attributes"

# Character sets are reset when a pane is revived.
cat <<'EOF' >$TMP/charset.sh
printf 'CHARSET\n\033(0'
read x
printf '\033(B'
read x
printf 'qqqq\n'
cat
EOF
$R new -d -s charset "sh $TMP/charset.sh" || fail "charset session"
$L connect-remote -d r || fail "connect charset"
wait_for "find_pane CHARSET" '%' "charset adopted"
CP=$(find_pane CHARSET)
wait_for "$L display -p -t $CP '#{pane_remote_state}'" '^connected$' \
	"charset connected"
$L disconnect-remote r || fail "disconnect charset"
$R send -t charset Enter
sleep 0.3
$L connect-remote -d r || fail "reconnect charset"
wait_for "$L display -p -t $CP '#{pane_remote_state}'" '^connected$' \
	"charset reconnected"
sleep 0.5
$R send -t charset Enter
wait_for "$R capture-pane -p -t charset" '^qqqq$' "charset remote"
wait_for "$L capture-pane -p -t $CP" 'qqqq' "charset output"
RL=$($R capture-pane -p -e -t charset | grep qqqq)
LL=$($L capture-pane -p -e -t $CP | grep qqqq)
[ "$RL" = "$LL" ] || fail "charset reset"

# Dead panes do not count when breaking panes out of a shared window.
$L disconnect-remote r || fail "disconnect isolate"
$R set -g remain-on-exit on
$R new -d -s iso -x 80 -y 24 "exit 3" || fail "iso session"
$R split-window -d -t iso cat || fail "iso split"
wait_for "$R display -p -t iso:0.0 '#{pane_dead}'" '^1$' "iso dead"
$R set -g remain-on-exit off
IP=$($R display -p -t iso:0.1 '#{pane_id}')
$L connect-remote -d r || fail "connect isolate"
wait_for "$R display -p -t $IP '#{window_panes}'" '^1$' "live pane not alone"

# Unfinished escape sequences are kept when a pane is adopted.
cat <<'EOF2' >$TMP/pending.sh
printf 'PENDING\n\033[31'
read x
printf 'mRED\n'
cat
EOF2
$L disconnect-remote r || fail "disconnect pending"
$R new -d -s pending "sh $TMP/pending.sh" || fail "pending session"
wait_for "$R capture-pane -p -t pending" '^PENDING$' "pending remote"
$L connect-remote -d r || fail "connect pending"
wait_for "find_pane PENDING" '%' "pending adopted"
PP=$(find_pane PENDING)
wait_for "$L display -p -t $PP '#{pane_remote_state}'" '^connected$' \
	"pending connected"
sleep 0.5
$R send -t pending Enter
wait_for "$R capture-pane -p -t pending" '^RED$' "pending remote output"
wait_for "$L capture-pane -p -t $PP" 'RED' "pending output"
RL=$($R capture-pane -p -e -t pending | grep RED)
LL=$($L capture-pane -p -e -t $PP | grep RED)
[ "$RL" = "$LL" ] || fail "pending sequence"

# The saved cursor is kept when a pane is adopted.
cat <<'EOF2' >$TMP/saved.sh
stty -echo
printf 'SAVED\n\033[3;5H\033[31m\0337\033[m\033[10;1H'
read x
printf '\0338X'
cat
EOF2
$L disconnect-remote r || fail "disconnect saved"
$R new -d -s saved "sh $TMP/saved.sh" || fail "saved session"
wait_for "$R capture-pane -p -t saved" '^SAVED$' "saved remote"
$L connect-remote -d r || fail "connect saved"
wait_for "find_pane SAVED" '%' "saved adopted"
SP=$(find_pane SAVED)
wait_for "$L display -p -t $SP '#{pane_remote_state}'" '^connected$' \
	"saved connected"
sleep 0.5
$R send -t saved Enter
wait_for "$R capture-pane -p -t saved" '^    X' "saved remote output"
wait_for "$L capture-pane -p -t $SP" 'X' "saved output"
RL=$($R capture-pane -p -e -t saved | sed -n 3p)
LL=$($L capture-pane -p -e -t $SP | sed -n 3p)
[ "$RL" = "$LL" ] || fail "saved cursor"

# Line drawing characters are kept when a pane is adopted.
cat <<'EOF2' >$TMP/acs.sh
printf 'ACS\n\033)0\016qqqq\017\n'
cat
EOF2
$L disconnect-remote r || fail "disconnect acs"
$R new -d -s acs "sh $TMP/acs.sh" || fail "acs session"
wait_for "$R capture-pane -p -t acs" '^ACS$' "acs remote"
$L connect-remote -d r || fail "connect acs"
wait_for "find_pane ACS" '%' "acs adopted"
AP=$(find_pane ACS)
wait_for "$L display -p -t $AP '#{pane_remote_state}'" '^connected$' \
	"acs connected"
RL=$($R capture-pane -p -e -t acs | sed -n 2p)
LL=$($L capture-pane -p -e -t $AP | sed -n 2p)
[ "$RL" = "$LL" ] || fail "acs cells"

# Tab stops are kept when a pane is adopted, for captured tabs and new ones.
cat <<'EOF2' >$TMP/tabs.sh
stty -echo
printf 'TABS\n\033[3g\033[1;6H\033H\033[2;1H\tA\r\n'
read x
printf '\033[2;1H\tB\r\n\tX'
cat
EOF2
$L disconnect-remote r || fail "disconnect tabs"
$R new -d -s tabs "sh $TMP/tabs.sh" || fail "tabs session"
wait_for "$R capture-pane -p -t tabs" 'A$' "tabs remote"
$L connect-remote -d r || fail "connect tabs"
wait_for "find_pane TABS" '%' "tabs adopted"
TP=$(find_pane TABS)
wait_for "$L display -p -t $TP '#{pane_remote_state}'" '^connected$' \
	"tabs connected"
sleep 0.5
$R send -t tabs Enter
wait_for "$R capture-pane -p -t tabs" 'X$' "tabs remote output"
wait_for "$L capture-pane -p -t $TP" 'X$' "tabs output"
RL=$($R display -p -t tabs '#{cursor_x}:#{cursor_y}'; \
    $R capture-pane -p -t tabs | sed -n 2,3p)
LL=$($L display -p -t $TP '#{cursor_x}:#{cursor_y}'; \
    $L capture-pane -p -t $TP | sed -n 2,3p)
[ "$RL" = "$LL" ] || fail "tab stops: $RL / $LL"

# A cursor waiting to wrap at the end of a line still wraps after reconnect.
cat <<'EOF2' >$TMP/wrap.sh
stty -echo
printf 'WRAPSTART\n'
read x
cols=$(stty size | cut -d' ' -f2)
printf "%$((cols - 1))s" '' | tr ' ' x
printf 'E'
read x
printf 'Z\n'
cat
EOF2
$L disconnect-remote r || fail "disconnect wrap"
$R new -d -s wrap "sh $TMP/wrap.sh" || fail "wrap session"
$L connect-remote -d r || fail "connect wrap"
wait_for "find_pane WRAPSTART" '%' "wrap adopted"
WP=$(find_pane WRAPSTART)
wait_for "$L display -p -t $WP '#{pane_remote_state}'" '^connected$' \
	"wrap connected"
$L disconnect-remote r || fail "disconnect wrap"
$R send -t wrap Enter
wait_for "$R capture-pane -p -t wrap" 'xE$' "wrap remote line"
$L connect-remote -d r || fail "reconnect wrap"
wait_for "$L display -p -t $WP '#{pane_remote_state}'" '^connected$' \
	"wrap reconnected"
sleep 0.5
$R send -t wrap Enter
wait_for "$R capture-pane -p -t wrap" '^Z$' "wrap remote output"
wait_for "$L capture-pane -p -t $WP" '^Z' "wrap output"
[ "$($R capture-pane -p -t wrap)" = "$($L capture-pane -p -t $WP)" ] ||
	fail "pending wrap"

# The same with trailing spaces, which capture-pane trims.
cat <<'EOF2' >$TMP/wrap2.sh
stty -echo
printf 'WRAPSPACE\n'
read x
cols=$(stty size | cut -d' ' -f2)
printf "WRAP%$((cols - 4))s" ''
read x
printf 'B\n'
cat
EOF2
$L disconnect-remote r || fail "disconnect wrap2"
$R new -d -s wrap2 "sh $TMP/wrap2.sh" || fail "wrap2 session"
$L connect-remote -d r || fail "connect wrap2"
wait_for "find_pane WRAPSPACE" '%' "wrap2 adopted"
WP=$(find_pane WRAPSPACE)
wait_for "$L display -p -t $WP '#{pane_remote_state}'" '^connected$' \
	"wrap2 connected"
$L disconnect-remote r || fail "disconnect wrap2"
$R send -t wrap2 Enter
wait_for "$R display -p -t wrap2 '#{cursor_x}'" '^80$' "wrap2 remote cursor"
$L connect-remote -d r || fail "reconnect wrap2"
wait_for "$L display -p -t $WP '#{pane_remote_state}'" '^connected$' \
	"wrap2 reconnected"
sleep 0.5
$R send -t wrap2 Enter
wait_for "$R capture-pane -p -t wrap2" '^B$' "wrap2 remote output"
wait_for "$L capture-pane -p -t $WP" '^B' "wrap2 output"
[ "$($R capture-pane -p -t wrap2)" = "$($L capture-pane -p -t $WP)" ] ||
	fail "pending wrap spaces"

# Soft wraps are kept, so the line joins again when the pane is wider.
cat <<'EOF2' >$TMP/soft.sh
printf 'SOFTSTART\n'
printf '%100s' '' | tr ' ' y
printf '\n'
printf '%79s' '' | tr ' ' z
printf '\344\270\255END\n'
cat
EOF2
$L disconnect-remote r || fail "disconnect soft"
$R new -d -s soft -x 80 -y 24 "sh $TMP/soft.sh" || fail "soft session"
wait_for "$R capture-pane -p -t soft" '^yyyy' "soft remote output"
$L connect-remote -d r || fail "connect soft"
wait_for "find_pane SOFTSTART" '%' "soft adopted"
WP=$(find_pane SOFTSTART)
wait_for "$L display -p -t $WP '#{pane_remote_state}'" '^connected$' \
	"soft connected"
wait_for "$R display -p -t soft '#{pane_width}'" "^$($L display -p -t $WP '#{pane_width}')\$" \
	"soft remote width"
sleep 0.5
[ "$($R capture-pane -p -t soft)" = "$($L capture-pane -p -t $WP)" ] ||
	fail "soft adopt: $($L capture-pane -p -t $WP | grep .)"
$L resize-window -t $WP -x 120 || fail "soft resize"
wait_for "$R display -p -t soft '#{pane_width}'" '^120$' "soft remote resize"
sleep 0.5
[ "$($R capture-pane -p -t soft)" = "$($L capture-pane -p -t $WP)" ] ||
	fail "soft wrap: $($L capture-pane -p -t $WP | grep .)"
$L kill-pane -t $WP

# Clipboard reads from a remote pane are answered once, by the remote.
cat <<'EOF2' >$TMP/clip.sh
printf 'CLIPSTART\n'
read x
stty -icanon -echo min 0 time 10
printf '\033]5522;type=read:id=b;dGV4dC9wbGFpbg==\033\\'
cat -v >CLIPOUT
cat
EOF2
sed "s|CLIPOUT|$TMP/clipreply|" $TMP/clip.sh >$TMP/clip2.sh
$L set -g get-clipboard off
$R set -g get-clipboard off
$L disconnect-remote r || fail "disconnect clip"
$R new -d -s clip "sh $TMP/clip2.sh" || fail "clip session"
$L connect-remote -d r || fail "connect clip"
wait_for "find_pane CLIPSTART" '%' "clip adopted"
KP=$(find_pane CLIPSTART)
wait_for "$L display -p -t $KP '#{pane_remote_state}'" '^connected$' \
	"clip connected"
sleep 0.5
$R send -t clip Enter
n=0
while [ $n -lt 50 ] && ! [ -s $TMP/clipreply ]; do
	sleep 0.1
	n=$((n + 1))
done
sleep 1.5
[ "$(grep -o 5522 $TMP/clipreply | wc -l | tr -d ' ')" = 1 ] ||
	fail "clipboard replies: $(cat $TMP/clipreply)"
$L set -gu get-clipboard
$R set -gu get-clipboard

# Existing hooks in the remote session are kept.
$L disconnect-remote r || fail "disconnect hooks"
$R has -t "=$H" 2>/dev/null || $R new -d -s "$H" cat
$R set-hook -t "=$H:" after-new-window 'set -g @hookran 1'
$L connect-remote -d r || fail "connect hooks"
NP=$($L split-window -P -F '#{pane_id}' -r r -t main:7 cat) ||
	fail "split-window -r hooks"
wait_for "$R show -gv @hookran" '^1$' "user hook ran"
$L kill-pane -t $NP
$L disconnect-remote r || fail "disconnect hooks"
[ "$($R show-hooks -t "=$H:" after-new-window)" = \
    "after-new-window[0] set-option -g @hookran 1" ] || fail "user hook changed"
$R set-hook -u -t "=$H:" after-new-window

# A failing hook in the remote session does not upset other commands.
$L disconnect-remote r 2>/dev/null
$R set-hook -t "=$H:" after-new-window 'kill-pane -t %999'
$L connect-remote -d r || fail "connect bad hook"
BP=$($L split-window -d -P -F '#{pane_id}' -r r -t main:7 cat \; \
    split-window -d -P -F '#{pane_id}' -r r -t main:7 cat) ||
	fail "split-window -r bad hook"
for p in $BP; do
	wait_for "$L display -p -t $p '#{pane_dead}:#{pane_remote_state}'" \
	    '^0:connected$' "bad hook pane"
done
sleep 1
for p in $BP; do
	[ "$($L display -p -t $p '#{pane_dead}')" = 0 ] ||
		fail "bad hook: $p dead: $($L capture-pane -p -t $p | grep .)"
	$L kill-pane -t $p
done
$R set-hook -u -t "=$H:" after-new-window

# A hook which waits does not lose the exit status of a fast command.
$R set-hook -t "=$H:" after-new-window 'run-shell "sleep 1"'
$L set -w -t main:7 remain-on-exit failed
WP=$($L split-window -P -F '#{pane_id}' -r r -t main:7 sh -c 'exit 7') ||
	fail "split-window -r waiting hook"
wait_for "$L display -p -t $WP '#{pane_dead}:#{pane_dead_status}'" '^1:7$' \
	"waiting hook exit status"
$L kill-pane -t $WP

# The same for the default command.
$R set -t "=$H:" default-command 'exit 7'
WP=$($L split-window -P -F '#{pane_id}' -r r -t main:7) ||
	fail "split-window -r default waiting hook"
wait_for "$L display -p -t $WP '#{pane_dead}:#{pane_dead_status}'" '^1:7$' \
	"waiting hook default exit status"
$L kill-pane -t $WP
$R set -u -t "=$H:" default-command
$L set -wu -t main:7 remain-on-exit
$R set-hook -u -t "=$H:" after-new-window

# A default command with several lines is kept whole, as by new-window.
ML="printf FIRST >>\$OUT

printf SECOND >>\$OUT"
$R set -t "=$H:" default-command "OUT=$TMP/ml-remote; $ML"
WP=$($L split-window -P -F '#{pane_id}' -r r -t main:7) ||
	fail "split-window -r multiline default"
$R new-session -d -s mlnative 'sleep 30'
$R set -t mlnative default-command "OUT=$TMP/ml-native; $ML"
$R new-window -d -t mlnative:
wait_for "cat $TMP/ml-native" '^FIRSTSECOND$' "native multiline default"
wait_for "cat $TMP/ml-remote" '^FIRSTSECOND$' \
	"multiline default: $(cat $TMP/ml-remote 2>/dev/null)"
$R kill-session -t mlnative
$L kill-pane -t $WP
$R set -u -t "=$H:" default-command

# Fast commands keep their exit status over a slow link. The control client
# gets its own stdout, because the server makes it nonblocking. list-panes is
# held back longer, so subscriptions are reported before it.
cat <<EOF >$TMP/ssh-slow
#!/bin/sh
for i in "\$@"; do
	last="\$i"
done
while IFS= read -r l; do
	sleep 0.12
	case "\$l" in
	list-panes*) sleep 1.5 ;;
	esac
	printf '%s\n' "\$l"
done | sh -c "\$last" | cat
EOF
chmod +x $TMP/ssh-slow
$L set -s 'remote-servers[3]' "slow -s $TMP/ssh-slow -t $TEST_TMUX -a '-Ltest4$$ -f/dev/null'"
$L set -w -t main:7 remain-on-exit failed
FP=$($L split-window -P -F '#{pane_id}' -r slow -t main:7 sh -c 'exit 7') ||
	fail "split-window -r slow"
wait_for "$L display -p -t $FP '#{pane_dead}:#{pane_dead_status}'" '^1:7$' \
	"fast exit status"
$L kill-pane -t $FP
$L set -wu -t main:7 remain-on-exit

# Commands start with the local pane size, even over a slow link.
SP=$($L split-window -P -F '#{pane_id}' -l 5 -r slow -t main:7 \
    sh -c "stty size >$TMP/size.tmp; mv $TMP/size.tmp $TMP/size; cat") ||
	fail "split-window -r slow size"
wait_for "cat $TMP/size" . "slow size"
size=$($L display -p -t $SP '#{pane_height} #{pane_width}')
[ "$(cat $TMP/size)" = "$size" ] ||
	fail "slow size: $(cat $TMP/size) not $size"
$L kill-pane -t $SP

# A pane which died while disconnected is not revived as a live pane, even
# when its death was reported before the pane was known again.
cat <<EOF >$TMP/die.sh
echo DIESTART
while [ ! -f $TMP/die ]; do sleep 0.1; done
exit 9
EOF
DP=$($L new-window -d -P -F '#{pane_id}' -r slow -t main: sh $TMP/die.sh) ||
	fail "split-window -r slow die"
wait_for "$L capture-pane -p -t $DP" '^DIESTART' "die connected"
$L disconnect-remote slow || fail "disconnect slow"
touch $TMP/die
wait_for "$S list-panes -a -F '#{pane_dead}:#{pane_dead_status}'" '^1:9$' \
	"die remote dead"
$L set -w -t $DP remain-on-exit failed
$L connect-remote -d slow || fail "reconnect slow"
wait_for "$L display -p -t $DP '#{pane_dead}:#{pane_dead_status}'" '^1:9$' \
	"revived dead pane"
wait_for "$S list-panes -a -F '#{pane_dead}:#{pane_dead_status}' | grep -c 1:9" \
	'^0$' "revived dead pane killed"

exit 0
