# kitty-tmux

## About this fork

`kitty-tmux` is a fork of [tmux](https://github.com/tmux/tmux) that adds support
for protocols from the [kitty](https://sw.kovidgoyal.net/kitty/) terminal and
other features that are not mainstream enough for upstream tmux.

I'll give my best to maintain this fork as I use `tmux` as my daily-driver myself.
Feel free open issues if you'd like to see any more protocols supported and
I will see what I can do.

Currently supported protocols in this fork:

- [Keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/):
- [Text sizing protocol](https://sw.kovidgoyal.net/kitty/text-sizing-protocol/) (OSC 66)
- [Drag and drop protocol](https://sw.kovidgoyal.net/kitty/dnd-protocol/) (OSC 72)
- [Graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/):
  images in panes, without needing passthrough and without the performance penalty
- [Clipboard protocol](https://sw.kovidgoyal.net/kitty/clipboard/) (OSC 5522):
  clipboard reads and writes with fallback to tmux buffer if your emulator doesn't support it
- [Pointer shapes](https://sw.kovidgoyal.net/kitty/pointer-shapes/) (OSC 22):
  mouse pointer shape of the pane under the mouse
- [Desktop notifications](https://sw.kovidgoyal.net/kitty/desktop-notifications/) (OSC 99):
  send push notifications from the terminal (also support legacy OSC 9 messages)

## Remote panes

Another thing I added to this fork (has nothing to do with Kitty) is
the ability to connect to a remote tmux server and add panes from it.

This works by connecting to the server using SSH and use that secure
connection for all communication.

**Important:** The SSH connection must work without password entry. I would recommend
to authenticate using a public key therefore. And also the remote server must
run the same version (this fork) of tmux for this to work.

### Configuring remote servers

First you need to add the connection to your `.tmux.conf` file:

```
set -g remote-servers[0] "dev-server1 -u ubuntu -H 10.0.2.4"
set -g remote-servers[1] "dev-server2 -u root -H 10.0.2.22 -t /root/.bin/tmux"
set -g remote-servers[2] "dev-server3 -H devbox.example.com -u dev -o '-p 2222' -t /opt/bin/tmux -a '-L work' -i 600"
```

Syntax for a server connection is:

```
server-name [-H ssh-host] [-u ssh-user] [-o 'extra-ssh-args'] [-s ssh-command] [-t remote-path-to-tmux] [-a 'extra-tmux-args'] [-i idle-time]
```

The idle time is the amount of seconds after which the connection is closed and set as inactive.
An inactive connection will be automatically revived when switching to a window with a pane
from that server or when pressing any key into a pane. Default is 300s (5 minutes).

A connection also becomes inactive if the connection breaks. That could happen for various reasons
like server reboot, local internet connection is broken, laptop closed, etc. The connection is
only re-established when actually needed in that case.

### Attaching the remote server

There are several new commands added to handle the remote connections:

- `tmux connect-remote [-d] remote-name` (alias `connect`): connects to the remote server and attaches all currently unattached panes into a new client
- `tmux disconnect-remote remote-name` (alias `disconnect`): disconnects the remote server rendering all active panes as disconnected. These panes don't revive automatically like inactive ones do.
- `tmux list-remotes` (alias `lsr`): lists the configured remote servers
- `tmux cleanup-remote remote-name`: removes all disconnected panes from the local server (the panes keep running on the remote server and will be re-attached when connecting again)

Then you can use `tmux new-window -r remote-name` and `tmux split-window -r remote-name` which creates a new pane on the remote server and opens a window or splits the current window and attaches it there.

Creating a new pane also automatically connects to the server in case it wasn't already.

## Welcome to tmux!

tmux is a terminal multiplexer: it enables a number of terminals to be created,
accessed, and controlled from a single screen. tmux may be detached from a
screen and continue running in the background, then later reattached.

This release runs on OpenBSD, FreeBSD, NetBSD, Linux, macOS and Solaris.

## Dependencies

tmux depends on [libevent](https://libevent.org) 2.x, available from [this
page](https://github.com/libevent/libevent/releases/latest).

It also depends on [ncurses](https://www.gnu.org/software/ncurses/), available
from [this page](https://invisible-mirror.net/archives/ncurses/).

To build tmux, a C compiler (for example gcc or clang), make, pkg-config and a
suitable yacc (yacc or bison) are needed.

## Installation

### Binary packages

Some platforms provide binary packages for tmux, although these are sometimes
out of date. Examples are listed on
[this page](https://github.com/tmux/tmux/wiki/Installing).

### From release tarball

To build and install tmux from a release tarball, use:

```bash
./configure && make
sudo make install
```

tmux can use the utempter library to update utmp(5), if it is installed - run
configure with `--enable-utempter` to enable this.

For more detailed instructions on building and installing tmux, see
[this page](https://github.com/tmux/tmux/wiki/Installing).

### From version control

To get and build the latest from version control - note that this requires
`autoconf`, `automake` and `pkg-config`:

```bash
git clone https://github.com/tmux/tmux.git
cd tmux
sh autogen.sh
./configure && make
```

## Contributing

Bug reports, feature suggestions and especially code contributions are most
welcome. Please send by email to:

tmux-users@googlegroups.com

Or open a GitHub issue or pull request. **Please read [this
document](CONTRIBUTING.md) before opening an issue.**

There is [a list of suggestions for contributions](https://github.com/tmux/tmux/wiki/Contributing).
Please feel free to ask on the mailing list if you're thinking of working on something or need
further information.

## Documentation

For documentation on using tmux, see the tmux.1 manpage. View it from the
source tree with:

```bash
nroff -mdoc tmux.1|less
```

A small example configuration is in `example_tmux.conf`.

And a bash(1) completion file at:

https://github.com/scop/bash-completion/blob/main/completions-core/tmux.bash

For debugging, run tmux with `-v` or `-vv` to generate server and client log
files in the current directory.

## Support

The tmux mailing list for general discussion and bug reports is:

https://groups.google.com/forum/#!forum/tmux-users

Subscribe by sending an email to:

tmux-users+subscribe@googlegroups.com
