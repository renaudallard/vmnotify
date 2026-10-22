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
  `teams|outlook|firefox`) it is sent with notify-send, the first line as
  summary and the rest as body. The area is also read once at start.
  A toast is sent once while it stays on screen. Lines are compared by
  their letters and digits and count as the same when at most a quarter
  of them differs, so stray characters read from an icon or a button
  showing under the mouse do not make it new. When a new toast stacks
  with one already sent, or takes the place of one that went away, only
  the lines of the new one are sent.

  To save work, a change is only read when it covers at least 15% of the
  area or two lines of text, so a line changing in a window underneath
  is ignored, and a toast going away is noticed by the area looking like
  it did before. When the toast paints its own background, changes away
  from it, such as a line changing higher up in a window underneath, are
  not read with it. Only colour differences count, never particular
  colours, so light and dark themes work alike, and light text on a dark
  toast is turned dark on light for tesseract.
- optionally, a taskbar icon (for example Teams). When its pixels change,
  e.g. a badge appears or the icon flashes, a "Taskbar icon changed"
  notification is sent. Further changes are ignored for 30 seconds.

After a first full update, vmnotify only asks the VNC server for changes
in the box covering the toast and taskbar areas, which it sends as they
happen, so notifications show up within about a second and little data
is moved.

vmnotify is a single C file with no library dependencies beyond libc. It
can run in the background by itself with `-d`.

## Requirements

At run time, tesseract and notify-send. On Debian:

    apt install tesseract-ocr libnotify-bin

## Installing

Static Linux amd64 binaries are attached to the
[releases](https://github.com/renaudallard/vmnotify/releases):

    tar xzf vmnotify-*-linux-amd64.tar.gz
    cd vmnotify-*-linux-amd64
    sudo install -m 755 vmnotify /usr/local/bin
    sudo install -D -m 644 vmnotify.1 /usr/local/share/man/man1/vmnotify.1

Or build from source with a C compiler and make:

    make
    sudo make install

A static binary can be built with musl:

    make CC=musl-gcc LDFLAGS=-static

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
   in the Outlook settings.

5. Take a screenshot to check the regions:

       vmnotify -s shot.ppm

   The default toast area is `-420,-430,420,380`, meaning 420x380 pixels
   starting 420 pixels from the right edge and 430 from the bottom. That
   fits 100% display scaling. Negative x and y count from the right and
   bottom edges. Adjust it with `toast` in `~/.vmnotifyrc` if the toasts
   are elsewhere, and pick the taskbar icon coordinates for `badge` from
   the screenshot.

6. Run it in the foreground with `-v` while a notification comes in, to
   see what is read and tune `match` if needed:

       vmnotify -v

7. Start it in the background, for example from your desktop's autostart:

       vmnotify -d

## Usage

    vmnotify [-dhVv] [-b x,y,w,h] [-c sec] [-i sec] [-m regex] [-s file]
             [-t x,y,w,h] [server]

    -b x,y,w,h  watch this taskbar icon for badge changes
    -c sec      ignore taskbar icon changes for sec after a taskbar icon
                notification (default: 30, at most 86400)
    -d          run in the background, log to syslog and start again 30
                seconds after any error, such as the VM being powered off
    -h          show the version and a summary of the options and of
                ~/.vmnotifyrc
    -i sec      read the toast area once it did not change for sec
                seconds (default: 1, at most 3600)
    -m regex    only forward toasts whose text matches the case insensitive
                extended regex (default: teams|outlook|firefox)
    -s file     save a screenshot of the VM to file as PPM and exit
    -t x,y,w,h  toast area (default: -420,-430,420,380)
    -V          log every step to stderr (syslog with -d), the VNC
                protocol included, but never the password; implies -v
    -v          print what is read, to syslog with -d
    server      host, host:display or host::port, IPv6 addresses in
                brackets (default: 127.0.0.1::5900)

Regions are at most 4096 by 4096 pixels, and the guest screen at most
8192 by 8192.

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
    # like -t, -b, -i and -c
    toast -420,-430,420,380
    badge 640,-40,48,40
    interval 1
    cooldown 30

The password is removed from the environment of tesseract and
notify-send.

## Running under systemd

Instead of `-d`, vmnotify can run as a systemd user service, which
restarts it when it exits:

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
- App icons and close buttons may add a few stray characters to the text.
- A window underneath whose content moves right next to the toast as it
  appears, such as a list of mails getting a new one, may add some of its
  text to the notification.
- Any window in the bottom right corner whose text matches the regex
  triggers a notification, once per distinct text.
- A taskbar icon also changes when the badge goes away or the mouse hovers
  over it.

## License

BSD 2-Clause, see LICENSE.
