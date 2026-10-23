# vmnotify

Shows Windows notifications from a VMware Workstation VM as notifications
on the Linux desktop.

vmnotify reads the guest screen through the VNC server built into VMware
Workstation, so nothing is installed or changed inside Windows. It works
while the VM window is hidden behind other windows or on another
workspace.

Two things are watched:

- the toast area in the bottom right corner. When it changed and then
  stayed the same for one second (toasts slide in), the changed part is
  run through tesseract, and if the text matches a regex (default
  `teams|outlook|firefox`) it is sent as a desktop notification, the first
  line as summary and the rest as body. The area is also read once at
  start. A toast is sent once while it stays on screen. Lines are compared
  by their letters and digits and count as the same when at most a quarter
  of them differs, so stray characters read from an icon or a button
  showing under the mouse do not make it new. When a new toast stacks with
  one already sent, or takes the place of one that went away, only the
  lines of the new one are sent, if they match the regex.

  To save work, a change is only read when it covers at least 15% of the
  area or two lines of text, so a line changing in a window underneath is
  ignored, and a toast going away is noticed by the area looking like it
  did before. When the toast paints its own background, changes away from
  it, such as a line changing higher up in a window underneath, are not
  read with it, unless nothing to forward was read in it. Only colour
  differences count, never particular colours, so light and dark themes
  work alike, and light text on a dark toast is turned dark on light for
  tesseract.
- the taskbar, for badges in the corner of app icons. Teams draws its
  own, green, yellow or grey with your presence and red with a white mark
  or count on new activity, also when Teams shows no toast, as for a
  message in the chat already open. These colours do not depend on the
  theme. Windows draws the badges apps ask it for, such as the envelope
  Outlook shows for new mail, as a disc filled with the accent colour, a
  single colour where icons are shaded. When the red pixels of the
  taskbar grow, a "Red badge on the taskbar icon" notification is sent;
  when those of single colour areas narrower than a badge do, a "New
  badge on the taskbar icon" one. Then none for 30 seconds. Its summary
  names the app when the icon next to the badge looks like Microsoft
  Teams, Outlook, Slack, WhatsApp, Signal or Telegram, "VM" otherwise.
  Badges going away, presence changes, the mouse over an icon and icons
  moving along the taskbar send nothing. `badge` sets another region,
  `badge none` turns this off.

Every notification summary starts with the name of the machine, the
desktop name the VNC server gives, so that notifications from several
machines can be told apart.

Nothing has to be set for the regions: at start, vmnotify finds the
taskbar at the bottom of the screen, by its top edge, and puts the toast
area of 420x380 pixels right above it, larger at a larger display
scaling.

After a first full update, vmnotify only asks the VNC server for changes
in the box covering the toast and taskbar areas, which it sends as they
happen, so notifications show up within about a second and little data
is moved.

vmnotify is a single C file with no library dependencies beyond libc. It
can run in the background by itself with `-d`.

## Requirements

At run time, tesseract. On Debian:

    apt install tesseract-ocr

Notifications are sent over D-Bus to the notification service of the
desktop, which every desktop environment has; notify-send is not
needed.

With `checkupdates yes` in `~/.vmnotifyrc` and curl installed, vmnotify
asks GitHub at start which release is the latest and says so when it is
newer than itself. It asks nothing by default.

## Installing

Static Linux amd64 binaries are attached to the
[releases](https://github.com/renaudallard/vmnotify/releases):

    tar xzf vmnotify-*-linux-amd64.tar.gz
    cd vmnotify-*-linux-amd64
    sudo ./install.sh

install.sh puts the binary and the manual page under `/usr/local`, or
under the directory given as argument, and warns when tesseract is
missing. With another directory, adjust `ExecStart` in
vmnotify.service if you use it.

Or build from source with a C compiler and make:

    make
    sudo make install

A static binary can be built with musl:

    make CC=musl-gcc LDFLAGS=-static

To upgrade, stop a running vmnotify, install the new one the same way
and start it again:

    pkill -x vmnotify
    sudo ./install.sh
    vmnotify -d

When it runs as the systemd user service, restart the service instead,
or it starts again by itself and one of the two finds the other
running:

    sudo ./install.sh
    systemctl --user restart vmnotify

Installing by hand as the README of 0.3.0 and earlier said may have
left the manual page in a file named `/usr/local/share/man/man1`
instead of a directory. install.sh checks that this file is the vmnotify
manual page and replaces it with the directory. When installing by hand,
remove it first:

    sudo rm /usr/local/share/man/man1

## Setup

1. In VMware Workstation, open VM > Settings > Options > VNC Connections,
   enable VNC, keep port 5900 (or pick one from 5901 to 6001) and set a
   password.

2. Check which address the VNC port listens on:

       ss -ltn | grep 5900

   If it is `0.0.0.0` rather than `127.0.0.1`, anyone who can reach the
   host can see and control the VM. Block the port in the host firewall.

3. Put the password in `~/.vmnotifyrc`, which must not be readable by
   anyone else:

       install -m 600 /dev/null ~/.vmnotifyrc
       echo 'password secret' > ~/.vmnotifyrc

4. In Windows, make sure notifications are actually shown: Do not disturb
   off, Teams notifications on, and for Outlook on the web allow
   notifications for the site in Firefox and turn on desktop notifications
   in the Outlook settings. Firefox shows them as Windows toasts, with
   "Firefox" at the top and "via" the site at the bottom, which the
   default regex matches.

5. The toast area and the taskbar are found by themselves; `vmnotify -V`
   logs what was found. When the taskbar is not at the bottom of the
   screen, take a screenshot and set `toast` and `badge` in
   `~/.vmnotifyrc`:

       vmnotify -s shot.ppm

   A region is x,y,w,h, negative x and y count from the right and bottom
   edges: `-420,-430,420,380` is 420x380 pixels starting 420 pixels from
   the right edge and 430 from the bottom.

6. Run it in the foreground with `-v` while a notification comes in, to
   see what is read and tune `match` if needed:

       vmnotify -v

7. Start it in the background, for example from your desktop's autostart:

       vmnotify -d

## Usage

    vmnotify [-dhVv] [-b x,y,w,h] [-c sec] [-i sec] [-m regex] [-s file]
             [-t x,y,w,h] [server]

    -b x,y,w,h  watch these taskbar icons for badges, none for none
                (default: the taskbar)
    -c sec      send no other badge notification for sec after one
                (default: 30, at most 86400)
    -d          run in the background, log to syslog and start again 30
                seconds after any error, such as the VM being powered off
    -h          show the version and a summary of the options and of
                ~/.vmnotifyrc
    -i sec      read the toast area once it did not change for sec
                seconds (default: 1, at most 3600)
    -m regex    only forward toasts whose text matches the case insensitive
                extended regex (default: teams|outlook|firefox)
    -s file     save a screenshot of the VM to file as PPM and exit
    -t x,y,w,h  toast area (default: 420x380 above the taskbar, more at
                a larger display scaling)
    -V          log every step to stderr (syslog with -d), the VNC
                protocol included, but never the password; implies -v
    -v          print what is read, to syslog with -d
    server      host, host:display or host::port, IPv6 addresses in
                brackets (default: 127.0.0.1::5900)

Regions are at most 4096 by 4096 pixels, and the guest screen at most
8192 by 8192.

With `-d`, ps(1) shows three processes: `vmnotify: monitor` followed by
the server, which starts the watching one again after errors,
`vmnotify: watcher` followed by the machine name and the server, and
`vmnotify: helper` followed by the server.

## Security

Everything the VNC server sends, the guest screen included, is read by
the watcher, or by `-s`, under a seccomp filter set right after
connecting: it may only read and write what it has open, poll, handle
its memory, never executable, and tell the time. Opening files, creating
sockets and ioctl fail; any other system call ends it, and `-d` logs
that before starting again. Running tesseract and sending D-Bus
notifications need much more, and tesseract would inherit the filter, so
a helper process forked before it, `vmnotify: helper`, does both for the
watcher over a socket pair and checks every request. The filter works on
amd64 and arm64.

Only one vmnotify watches at a time, it holds a lock on
`$XDG_RUNTIME_DIR/vmnotify.lock` (or `~/.vmnotify.lock`), which also
contains its process ID. A screenshot with `-s` can always be taken.

Without `-d`, vmnotify exits on errors, including when the VNC connection
fails or drops.

When something does not work, `vmnotify -V -s shot.ppm` shows each step
of the connection and where it stops; errors also name what vmnotify
was waiting for.

See vmnotify(1) for details.

## ~/.vmnotifyrc

One `keyword value` per line. Empty lines and lines starting with `#` are
ignored, as are blanks around the value. Command line options take
precedence over the file. It must be owned by you and not be accessible
by anyone else.

    # VNC password, the VNC_PASSWORD environment variable takes precedence
    password secret
    # which toasts to forward, like -m
    match teams|outlook|firefox
    # like the server argument
    server 127.0.0.1::5900
    # like -t, -b, -i and -c; toast and badge are found when not set
    toast -420,-430,420,380
    badge 0,-48,1920,48
    interval 1
    cooldown 30
    # say at start when a newer release is out, asking GitHub
    checkupdates no

The password is removed from the environment of tesseract.

## Running under systemd

Instead of `-d`, vmnotify can run as a systemd user service, which
restarts it when it exits. Unlike `-d`, which restarts the watching
process but not itself, this also covers vmnotify being killed:

    mkdir -p ~/.config/systemd/user
    cp vmnotify.service ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user enable --now vmnotify

Logs:

    journalctl --user -u vmnotify -f

## Limitations

- Only the VNC security types None and VNC authentication are supported,
  which is what VMware Workstation offers.
- A toast is only read once the toast area stopped changing for the time
  set with `-i`. If something keeps changing behind a translucent toast, such
  as a video, it is not read.
- When the VM window is in front and you are working in Windows, the
  notifications show up on both sides.
- A toast that is not forwarded and then replaced by one that is, with
  only its first line differing, is not read.
- When a toast below others is dismissed while a queued one appears, the
  toasts still on screen are sent again with the new one.
- App icons and close buttons may add a few stray characters to the text.
- A window underneath whose content moves right next to the toast as it
  appears, such as a list of mails getting a new one, may add some of its
  text to the notification.
- Any window in the bottom right corner whose text matches the regex
  triggers a notification, once per distinct text.
- Teams also makes its badge red when your presence is busy or do not
  disturb, as at the start of a meeting, which sends a red badge
  notification.
- An app starting, or an icon changing, with areas of a single colour may
  send a new badge notification.

## License

BSD 2-Clause, see LICENSE.
