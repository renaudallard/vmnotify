/*
 * Copyright (c) 2026, Renaud Allard <renaud@allard.it>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Forward Windows notifications from a VMware VM to the Linux desktop.
 *
 * The guest screen is read through the VNC server built into VMware
 * Workstation, so nothing has to run inside the guest. The toast area is
 * OCRed with tesseract when it changes and an optional taskbar icon is
 * watched for badge changes. Both end up as notify-send notifications.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define VERSION		"0.2.2"

#define MAXDIM		8192	/* largest framebuffer side accepted */
#define MAXREGION	4096	/* largest region side accepted */
#define MAXOCR		65536	/* OCR output kept, the rest is dropped */
#define TIMEOUT		30	/* seconds without data from the server */
#define RETRY		30	/* seconds between restarts with -d */
#define TICK		250	/* milliseconds between update requests */
#define KEEPIDLE	60	/* seconds idle before TCP keepalive probes */
#define KEEPINTVL	10	/* seconds between probes */
#define KEEPCNT		6	/* unanswered probes before giving up */
#define MINCHANGE	15	/* percent of the toast area to change */
#define MINHEIGHT	40	/* or rows it spans, about two text lines */
#define NOISE		32	/* grey levels a pixel may move unseen */
#define AROUND		20	/* pixels on each side judging the background */
#define BLOCK		8	/* side of the blocks a toast fills */
#define REFRESHES	10	/* requests to get a full update covered */

extern char **environ;

struct region {
	int	x, y, w, h;
};

struct vnc {
	int		 fd;
	int		 minor;
	int		 w, h;
	unsigned int	 gen;		/* bumped on screen size changes */
	uint8_t		*fb;		/* w * h pixels, B G R X */
	uint8_t		*got;		/* w * h bits, pixels received */
};

/* Settings from the command line or ~/.vmnotifyrc, kept as strings. */
enum {
	K_BADGE,
	K_COOLDOWN,
	K_INTERVAL,
	K_MATCH,
	K_PASSWORD,
	K_SERVER,
	K_TOAST,
	NKEYS
};

static const char *keywords[NKEYS] = {
	"badge", "cooldown", "interval", "match", "password", "server", "toast"
};

static int verbose;
static int debugging;
static int logsyslog;
/* what we are waiting for, for error messages */
static const char *stage = "connecting";

/* Logging, to stderr or with -d to syslog */

static void __attribute__((__format__(__printf__, 3, 0)))
vlog(int pri, int errnum, const char *fmt, va_list ap)
{
	char msg[1024];

	vsnprintf(msg, sizeof(msg), fmt, ap);
	if (logsyslog) {
		if (errnum != 0)
			syslog(pri, "%s: %s", msg, strerror(errnum));
		else
			syslog(pri, "%s", msg);
	} else {
		if (errnum != 0)
			fprintf(stderr, "vmnotify: %s: %s\n", msg,
			    strerror(errnum));
		else
			fprintf(stderr, "vmnotify: %s\n", msg);
	}
}

static void __attribute__((__noreturn__, __format__(__printf__, 1, 2)))
fatal(const char *fmt, ...)
{
	va_list ap;
	int e = errno;

	va_start(ap, fmt);
	vlog(LOG_ERR, e, fmt, ap);
	va_end(ap);
	exit(1);
}

static void __attribute__((__noreturn__, __format__(__printf__, 1, 2)))
fatalx(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(LOG_ERR, 0, fmt, ap);
	va_end(ap);
	exit(1);
}

static void __attribute__((__format__(__printf__, 1, 2)))
log_warn(const char *fmt, ...)
{
	va_list ap;
	int e = errno;

	va_start(ap, fmt);
	vlog(LOG_WARNING, e, fmt, ap);
	va_end(ap);
}

static void __attribute__((__format__(__printf__, 1, 2)))
log_warnx(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(LOG_WARNING, 0, fmt, ap);
	va_end(ap);
}

/* Monotonic milliseconds */
static long long
now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
		fatal("clock_gettime");
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* -V, everything that happens, with a timestamp */
static void __attribute__((__format__(__printf__, 1, 2)))
debug(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;
	char msg[1024];

	if (!debugging)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (logsyslog) {
		syslog(LOG_DEBUG, "%s", msg);
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
		ts.tv_sec = ts.tv_nsec = 0;
	fprintf(stderr, "vmnotify: %lld.%03ld %s\n", (long long)ts.tv_sec,
	    ts.tv_nsec / 1000000, msg);
}

static void __attribute__((__noreturn__))
usage(void)
{
	fprintf(stderr, "usage: vmnotify [-dhVv] [-b x,y,w,h] [-c sec] "
	    "[-i sec] [-m regex] [-s file]\n"
	    "                [-t x,y,w,h] [server]\n");
	exit(1);
}

static void __attribute__((__noreturn__))
help(void)
{
	printf("vmnotify %s\n"
	    "usage: vmnotify [-dhVv] [-b x,y,w,h] [-c sec] [-i sec] "
	    "[-m regex] [-s file]\n"
	    "                [-t x,y,w,h] [server]\n"
	    "\n"
	    "Forward Windows notifications from a VMware Workstation VM "
	    "to the desktop.\n"
	    "\n"
	    "  -b x,y,w,h  watch this taskbar icon for badge changes\n"
	    "  -c sec      ignore taskbar icon changes for sec after one was "
	    "sent (30)\n"
	    "  -d          run in the background and restart after errors\n"
	    "  -h          show this help\n"
	    "  -i sec      read the toast area once it did not change for sec "
	    "seconds (1)\n"
	    "  -m regex    only forward toasts matching the case insensitive "
	    "extended\n"
	    "              regex (teams|outlook|firefox)\n"
	    "  -s file     save a screenshot of the VM as PPM to file and "
	    "exit\n"
	    "  -t x,y,w,h  toast area (-420,-430,420,380)\n"
	    "  -V          log every step, the VNC protocol included, implies "
	    "-v\n"
	    "  -v          print what is read\n"
	    "  server      host, host:display, host::port or [address]::port\n"
	    "              (127.0.0.1::5900)\n"
	    "\n"
	    "In regions, negative x and y count from the right and bottom "
	    "edges.\n"
	    "\n"
	    "~/.vmnotifyrc must be owned by you and not be accessible by "
	    "others. It\n"
	    "holds lines of the form \"keyword value\", lines starting with "
	    "# are\n"
	    "comments. Command line options take precedence over it.\n"
	    "\n"
	    "  password secret      VNC password, VNC_PASSWORD takes "
	    "precedence\n"
	    "  match regex          like -m\n"
	    "  server host::port    like server\n"
	    "  toast x,y,w,h        like -t\n"
	    "  badge x,y,w,h        like -b\n"
	    "  interval sec         like -i\n"
	    "  cooldown sec         like -c\n", VERSION);
	exit(0);
}

/* DES, only what VNC authentication needs: encrypting one block. */

static const uint8_t des_ip[64] = {
	58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
	62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
	57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};

static const uint8_t des_fp[64] = {
	40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
	38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
	36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
	34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25
};

static const uint8_t des_e[48] = {
	32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9,
	8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
	16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
	24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1
};

static const uint8_t des_p[32] = {
	16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
	2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25
};

static const uint8_t des_pc1[56] = {
	57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
	10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
	63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
	14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4
};

static const uint8_t des_pc2[48] = {
	14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
	23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
	41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};

static const uint8_t des_shift[16] = {
	1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

static const uint8_t des_s[8][64] = {
	{ 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
	  0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
	  4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
	  15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
	{ 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
	  3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
	  0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
	  13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
	{ 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
	  13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
	  13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
	  1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
	{ 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
	  13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
	  10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
	  3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
	{ 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
	  14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
	  4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
	  11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
	{ 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
	  10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
	  9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
	  4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
	{ 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
	  13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
	  1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
	  6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
	{ 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
	  1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
	  7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
	  2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 }
};

/* Bit 1 of a table is the most significant of the nbits wide input. */
static uint64_t
permute(uint64_t in, int nbits, const uint8_t *table, int n)
{
	uint64_t out = 0;
	int i;

	for (i = 0; i < n; i++)
		out = (out << 1) | ((in >> (nbits - table[i])) & 1);
	return out;
}

static void
des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8])
{
	uint64_t k = 0, b = 0, sub[16], cd, e;
	uint32_t c, d, l, r, s, t;
	int i, j, six;

	for (i = 0; i < 8; i++) {
		k = (k << 8) | key[i];
		b = (b << 8) | in[i];
	}

	cd = permute(k, 64, des_pc1, 56);
	c = (cd >> 28) & 0xfffffff;
	d = cd & 0xfffffff;
	for (i = 0; i < 16; i++) {
		c = ((c << des_shift[i]) | (c >> (28 - des_shift[i]))) &
		    0xfffffff;
		d = ((d << des_shift[i]) | (d >> (28 - des_shift[i]))) &
		    0xfffffff;
		sub[i] = permute(((uint64_t)c << 28) | d, 56, des_pc2, 48);
	}

	b = permute(b, 64, des_ip, 64);
	l = b >> 32;
	r = b & 0xffffffff;
	for (i = 0; i < 16; i++) {
		e = permute(r, 32, des_e, 48) ^ sub[i];
		s = 0;
		for (j = 0; j < 8; j++) {
			six = (e >> (42 - 6 * j)) & 0x3f;
			s = (s << 4) |
			    des_s[j][(six & 0x20) | ((six & 1) << 4) |
			    ((six >> 1) & 0xf)];
		}
		t = r;
		r = l ^ (uint32_t)permute(s, 32, des_p, 32);
		l = t;
	}
	b = permute(((uint64_t)r << 32) | l, 64, des_fp, 64);

	for (i = 7; i >= 0; i--) {
		out[i] = b & 0xff;
		b >>= 8;
	}
	explicit_bzero(sub, sizeof(sub));
}

/* I/O helpers */

static int
readall(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = read(fd, p, len)) == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = 0;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static int
writeall(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = write(fd, p, len)) == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static void
vread(struct vnc *v, void *buf, size_t len)
{
	if (readall(v->fd, buf, len) == -1) {
		if (errno == 0)
			fatalx("connection closed by server, waiting for %s",
			    stage);
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			fatalx("server timed out, waiting for %s", stage);
		fatal("read, waiting for %s", stage);
	}
}

static void
vwrite(struct vnc *v, const void *buf, size_t len)
{
	if (writeall(v->fd, buf, len) == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			fatalx("server timed out, it stopped reading");
		if (errno == EPIPE || errno == ECONNRESET)
			fatalx("connection closed by server");
		fatal("write");
	}
}

static void
vskip(struct vnc *v, size_t len)
{
	uint8_t buf[4096];
	size_t n;

	while (len > 0) {
		n = len < sizeof(buf) ? len : sizeof(buf);
		vread(v, buf, n);
		len -= n;
	}
}

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v & 0xff;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = (v >> 16) & 0xff;
	p[2] = (v >> 8) & 0xff;
	p[3] = v & 0xff;
}

/* RFB client */

static void __attribute__((__noreturn__))
vnc_reason(struct vnc *v, const char *what)
{
	char buf[256];
	uint8_t len[4];
	uint32_t i, n;

	vread(v, len, sizeof(len));
	n = get32(len);
	if (n >= sizeof(buf)) {
		vread(v, buf, sizeof(buf) - 1);
		vskip(v, n - (sizeof(buf) - 1));
		n = sizeof(buf) - 1;
	} else
		vread(v, buf, n);
	buf[n] = '\0';

	/* the reason comes from the server, keep escape sequences out */
	for (i = 0; i < n; i++)
		if (!isprint((unsigned char)buf[i]))
			buf[i] = '?';
	fatalx("%s: %s", what, buf);
}

/* A new size leaves the screen blank and nothing of it received. */
static void
vnc_resize(struct vnc *v, int w, int h)
{
	if (w <= 0 || h <= 0 || w > MAXDIM || h > MAXDIM)
		fatalx("unsupported screen size %dx%d", w, h);
	if (v->fb != NULL && w == v->w && h == v->h)
		return;
	free(v->fb);
	free(v->got);
	if ((v->fb = calloc((size_t)w * h, 4)) == NULL ||
	    (v->got = calloc(((size_t)w * h + 7) / 8, 1)) == NULL)
		fatal("calloc");
	v->w = w;
	v->h = h;
	v->gen++;
	debug("screen size is now %dx%d", w, h);
}

/* Note the pixels of a rectangle as received, clipped to the screen. */
static void
mark(struct vnc *v, int x, int y, int w, int h)
{
	size_t i;
	int x1, y1, j, k;

	x1 = x + w < v->w ? x + w : v->w;
	y1 = y + h < v->h ? y + h : v->h;
	for (j = y; j < y1; j++) {
		for (k = x; k < x1; k++) {
			i = (size_t)j * v->w + k;
			v->got[i / 8] |= 1 << (i % 8);
		}
	}
}

/* Return 1 if every pixel of box was received. */
static int
covered(const struct vnc *v, const int box[4])
{
	size_t i;
	int j, k;

	for (j = box[1]; j < box[1] + box[3]; j++) {
		for (k = box[0]; k < box[0] + box[2]; k++) {
			i = (size_t)j * v->w + k;
			if (!(v->got[i / 8] & (1 << (i % 8))))
				return 0;
		}
	}
	return 1;
}

static void
vnc_connect(struct vnc *v, const char *host, const char *port)
{
	struct addrinfo hints, *res, *ai;
	struct timeval tv = { TIMEOUT, 0 };
	int error, fd = -1, save = 0, on = 1, idle = KEEPIDLE;
	int intvl = KEEPINTVL, cnt = KEEPCNT;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	debug("connecting to %s port %s", host, port);
	if ((error = getaddrinfo(host, port, &hints, &res)) != 0)
		fatalx("%s: %s", host, gai_strerror(error));
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
		    ai->ai_protocol);
		if (fd == -1) {
			save = errno;
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		save = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd == -1) {
		errno = save;
		fatal("connect to %s port %s", host, port);
	}
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1 ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1)
		fatal("setsockopt");

	/*
	 * Waiting for an update has no timeout, keepalive notices a server
	 * that went away without closing, such as a host that rebooted.
	 * It does not touch the RFB stream.
	 */
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == -1 ||
	    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle,
	    sizeof(idle)) == -1 ||
	    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl,
	    sizeof(intvl)) == -1 ||
	    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) == -1)
		fatal("keepalive");
	debug("connected");
	v->fd = fd;
}

/* Read the desktop name of ServerInit, for -V only. */
static void
vnc_name(struct vnc *v, uint32_t len)
{
	char name[256];
	uint32_t i, n;

	n = len < sizeof(name) ? len : sizeof(name) - 1;
	vread(v, name, n);
	vskip(v, len - n);
	for (i = 0; i < n; i++)
		if (!isprint((unsigned char)name[i]))
			name[i] = '?';
	name[n] = '\0';
	debug("server name \"%s\"%s", name, n < len ? ", truncated" : "");
}

static void
vnc_auth(struct vnc *v, const char *password)
{
	uint8_t key[8], challenge[16], response[16];
	int i, j;

	if (password == NULL)
		fatalx("server requires a password, see ~/.vmnotifyrc");

	/* VNC uses each password byte with its bits mirrored as DES key. */
	memset(key, 0, sizeof(key));
	for (i = 0; i < 8 && password[i] != '\0'; i++)
		for (j = 0; j < 8; j++)
			if ((unsigned char)password[i] & (1 << j))
				key[i] |= 0x80 >> j;

	stage = "the authentication challenge";
	vread(v, challenge, sizeof(challenge));
	des_encrypt(key, challenge, response);
	des_encrypt(key, challenge + 8, response + 8);
	/* neither is logged, they would allow guessing the password */
	debug("got the challenge, sending the response");
	vwrite(v, response, sizeof(response));
	explicit_bzero(key, sizeof(key));
}

static void
vnc_handshake(struct vnc *v, const char *password)
{
	uint8_t buf[24], types[255], type = 0, n;
	int i, major, minor, none = 0, vncauth = 0;

	stage = "the protocol version";
	vread(v, buf, 12);
	if (memcmp(buf, "RFB ", 4) != 0 || buf[7] != '.' || buf[11] != '\n')
		fatalx("not a VNC server");
	buf[7] = buf[11] = '\0';
	major = atoi((char *)buf + 4);
	minor = atoi((char *)buf + 8);
	if (major < 3)
		fatalx("unsupported protocol version %d.%d", major, minor);
	if (major > 3 || minor >= 8)
		v->minor = 8;
	else if (minor == 7)
		v->minor = 7;
	else
		v->minor = 3;
	debug("server speaks RFB %d.%d, using 3.%d", major, minor, v->minor);
	snprintf((char *)buf, sizeof(buf), "RFB 003.%03d\n", v->minor);
	vwrite(v, buf, 12);

	stage = "the security types";
	if (v->minor >= 7) {
		vread(v, &n, 1);
		if (n == 0)
			vnc_reason(v, "connection refused");
		vread(v, types, n);
		for (i = 0; i < n; i++) {
			debug("server offers security type %u", types[i]);
			if (types[i] == 1)
				none = 1;
			else if (types[i] == 2)
				vncauth = 1;
		}
		if (vncauth && (password != NULL || !none))
			type = 2;
		else if (none)
			type = 1;
		else
			fatalx("no supported security type");
		debug("using security type %u", type);
		vwrite(v, &type, 1);
	} else {
		vread(v, buf, 4);
		if (get32(buf) == 0)
			vnc_reason(v, "connection refused");
		if (get32(buf) != 1 && get32(buf) != 2)
			fatalx("unsupported security type %u", get32(buf));
		type = get32(buf);
		debug("server chose security type %u", type);
	}

	if (type == 2)
		vnc_auth(v, password);
	if (type == 2 || v->minor == 8) {
		stage = "the authentication result";
		vread(v, buf, 4);
		debug("security result %u", get32(buf));
		if (get32(buf) != 0) {
			if (v->minor == 8)
				vnc_reason(v, "authentication failed");
			fatalx("authentication failed");
		}
	}

	/* ClientInit, shared so other viewers are not disconnected */
	debug("sending ClientInit, shared");
	buf[0] = 1;
	vwrite(v, buf, 1);

	/* ServerInit */
	stage = "the screen size";
	vread(v, buf, 24);
	debug("server screen %ux%u, native pixel format: %u bpp, depth %u, "
	    "%s endian, %s, max %u %u %u, shift %u %u %u", get16(buf),
	    get16(buf + 2), buf[4], buf[5], buf[6] ? "big" : "little",
	    buf[7] ? "true colour" : "colour map", get16(buf + 8),
	    get16(buf + 10), get16(buf + 12), buf[14], buf[15], buf[16]);
	vnc_resize(v, get16(buf), get16(buf + 2));
	vnc_name(v, get32(buf + 20));

	/* SetPixelFormat: 32 bits, little endian B G R X */
	memset(buf, 0, 20);
	buf[4] = 32;
	buf[5] = 24;
	buf[6] = 0;
	buf[7] = 1;
	put16(buf + 8, 255);
	put16(buf + 10, 255);
	put16(buf + 12, 255);
	buf[14] = 16;
	buf[15] = 8;
	buf[16] = 0;
	debug("sending SetPixelFormat: 32 bpp, depth 24, little endian, "
	    "true colour, max 255 255 255, shift 16 8 0");
	vwrite(v, buf, 20);

	/* SetEncodings: Raw and DesktopSize */
	memset(buf, 0, 12);
	buf[0] = 2;
	put16(buf + 2, 2);
	put32(buf + 4, 0);
	put32(buf + 8, (uint32_t)-223);
	debug("sending SetEncodings: raw, desktop size");
	vwrite(v, buf, 12);
}

static void
vnc_raw(struct vnc *v, int x, int y, int w, int h)
{
	static uint8_t row[65535 * 4];
	size_t ncol;
	int i;

	mark(v, x, y, w, h);

	/* Rectangles may stick out of the screen, keep what is inside. */
	ncol = x < v->w ? (size_t)(w < v->w - x ? w : v->w - x) : 0;
	for (i = 0; i < h; i++) {
		vread(v, row, (size_t)w * 4);
		if (y + i < v->h && ncol > 0)
			memcpy(v->fb + ((size_t)(y + i) * v->w + x) * 4, row,
			    ncol * 4);
	}
}

/* Read one server message, return 1 if it was a framebuffer update. */
static int
vnc_message(struct vnc *v)
{
	uint8_t type, buf[12];
	int32_t enc;
	int i, n;

	vread(v, &type, 1);
	switch (type) {
	case 0:		/* FramebufferUpdate */
		vread(v, buf, 3);
		n = get16(buf + 1);
		debug("update with %d rectangles", n);
		for (i = 0; i < n; i++) {
			vread(v, buf, 12);
			enc = (int32_t)get32(buf + 8);
			debug("rectangle %ux%u at %u,%u, encoding %d",
			    get16(buf + 4), get16(buf + 6), get16(buf),
			    get16(buf + 2), enc);
			if (enc == 0)
				vnc_raw(v, get16(buf), get16(buf + 2),
				    get16(buf + 4), get16(buf + 6));
			else if (enc == -223)
				vnc_resize(v, get16(buf + 4), get16(buf + 6));
			else
				fatalx("unexpected encoding %d", enc);
		}
		return 1;
	case 1:		/* SetColourMapEntries */
		vread(v, buf, 5);
		debug("colour map with %u entries", get16(buf + 3));
		vskip(v, (size_t)get16(buf + 3) * 6);
		break;
	case 2:		/* Bell */
		debug("bell");
		break;
	case 3:		/* ServerCutText */
		vread(v, buf, 7);
		debug("clipboard text of %u bytes", get32(buf + 3));
		vskip(v, get32(buf + 3));
		break;
	default:
		fatalx("unexpected message type %u", type);
	}
	return 0;
}

/* Top left corner of r, negative coordinates count from the far edges. */
static void
origin(const struct vnc *v, const struct region *r, int *x, int *y)
{
	*x = r->x < 0 ? v->w + r->x : r->x;
	*y = r->y < 0 ? v->h + r->y : r->y;
}

static int
clamp(int n, int max)
{
	return n < 0 ? 0 : n > max ? max : n;
}

/* The on screen part of r as x0, y0, x1, y1, return 0 if there is none. */
static int
clip(const struct vnc *v, const struct region *r, int c[4])
{
	origin(v, r, &c[0], &c[1]);
	c[2] = clamp(c[0] + r->w, v->w);
	c[3] = clamp(c[1] + r->h, v->h);
	c[0] = clamp(c[0], v->w);
	c[1] = clamp(c[1], v->h);
	return c[0] < c[2] && c[1] < c[3];
}

/*
 * The part of the screen covering regions a and b, either may be NULL.
 * The whole screen when none is given or they are all off screen.
 */
static void
area(const struct vnc *v, const struct region *a, const struct region *b,
    int box[4])
{
	const struct region *r[2] = { a, b };
	int i, c[4], bx0 = v->w, by0 = v->h, bx1 = 0, by1 = 0;

	for (i = 0; i < 2; i++) {
		if (r[i] == NULL || !clip(v, r[i], c))
			continue;
		bx0 = c[0] < bx0 ? c[0] : bx0;
		by0 = c[1] < by0 ? c[1] : by0;
		bx1 = c[2] > bx1 ? c[2] : bx1;
		by1 = c[3] > by1 ? c[3] : by1;
	}
	if (bx0 >= bx1 || by0 >= by1) {
		bx0 = by0 = 0;
		bx1 = v->w;
		by1 = v->h;
	}
	box[0] = bx0;
	box[1] = by0;
	box[2] = bx1 - bx0;
	box[3] = by1 - by0;
}

static void
vnc_request(struct vnc *v, int incremental, const int box[4])
{
	uint8_t req[10];

	debug("requesting %s update of %dx%d at %d,%d",
	    incremental ? "an incremental" : "a full", box[2], box[3], box[0],
	    box[1]);
	req[0] = 3;
	req[1] = incremental;
	put16(req + 2, box[0]);
	put16(req + 4, box[1]);
	put16(req + 6, box[2]);
	put16(req + 8, box[3]);
	vwrite(v, req, sizeof(req));
}

/* Fetch the part of the screen covering a and b, see area(). */
static void
vnc_refresh(struct vnc *v, const struct region *a, const struct region *b)
{
	struct pollfd pfd;
	int box[4], tries;

	pfd.fd = v->fd;
	pfd.events = POLLIN;
	memset(v->got, 0, ((size_t)v->w * v->h + 7) / 8);

	/*
	 * Some servers answer with a part of the area only, ask again until
	 * all of it came or REFRESHES requests were made. A resize starts
	 * over by itself, it empties v->got.
	 */
	for (tries = 0; tries < REFRESHES; tries++) {
		area(v, a, b, box);
		if (covered(v, box))
			return;
		vnc_request(v, 0, box);
		stage = "a screen update";
		while (vnc_message(v) == 0)
			;

		/* Catch up with anything else already sent. */
		while (poll(&pfd, 1, 0) > 0)
			vnc_message(v);
	}
	area(v, a, b, box);
	if (!covered(v, box))
		log_warnx("screen incomplete after %d requests", REFRESHES);
}

/* Screen handling */

static void
crop(const struct vnc *v, const struct region *r, uint8_t *out)
{
	const uint8_t *p;
	int i, j, x0, y0, x, y;

	origin(v, r, &x0, &y0);
	for (j = 0; j < r->h; j++) {
		for (i = 0; i < r->w; i++) {
			x = x0 + i;
			y = y0 + j;
			if (x < 0 || y < 0 || x >= v->w || y >= v->h) {
				*out++ = 0;
				continue;
			}
			p = v->fb + ((size_t)y * v->w + x) * 4;
			*out++ = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
		}
	}
}

static void
screenshot(const struct vnc *v, const char *path)
{
	FILE *fp;
	const uint8_t *p;
	size_t i;
	int fd;

	/* it shows the VM, keep it to the user */
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
	    0600)) == -1 || (fp = fdopen(fd, "w")) == NULL)
		fatal("%s", path);
	fprintf(fp, "P6\n%d %d\n255\n", v->w, v->h);
	for (i = 0, p = v->fb; i < (size_t)v->w * v->h; i++, p += 4) {
		putc(p[2], fp);
		putc(p[1], fp);
		putc(p[0], fp);
	}
	if (ferror(fp) | fclose(fp))
		fatal("%s", path);
}

/* External commands */

static pid_t
spawn(char *const argv[], int in, int out)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	sigset_t sigs;
	pid_t pid;
	int error;

	/* SIGPIPE is ignored here, give the children the default back */
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGPIPE);
	if ((error = posix_spawnattr_init(&attr)) != 0 ||
	    (error = posix_spawnattr_setsigdefault(&attr, &sigs)) != 0 ||
	    (error = posix_spawnattr_setflags(&attr,
	    POSIX_SPAWN_SETSIGDEF)) != 0 ||
	    (error = posix_spawn_file_actions_init(&fa)) != 0)
		goto fail;
	if (in != -1 &&
	    (error = posix_spawn_file_actions_adddup2(&fa, in,
	    STDIN_FILENO)) != 0)
		goto fail;
	if (out != -1 &&
	    ((error = posix_spawn_file_actions_adddup2(&fa, out,
	    STDOUT_FILENO)) != 0 ||
	    (error = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO,
	    "/dev/null", O_WRONLY, 0)) != 0))
		goto fail;
	if ((error = posix_spawnp(&pid, argv[0], &fa, &attr, argv,
	    environ)) != 0)
		goto fail;
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	return pid;

fail:
	errno = error;
	fatal("%s", argv[0]);
}

/* Wait for a command, return 0 if it succeeded. */
static int
reap(pid_t pid, const char *name)
{
	int status;

	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			fatal("waitpid");
	if (WIFSIGNALED(status)) {
		log_warnx("%s killed by signal %d", name, WTERMSIG(status));
		return -1;
	}
	if (WEXITSTATUS(status) != 0) {
		log_warnx("%s exited with status %d", name,
		    WEXITSTATUS(status));
		return -1;
	}
	return 0;
}

/*
 * Turn light text on a dark background, a dark toast or theme, into dark
 * on light, in place. Tesseract no longer looks for inverted text and
 * misses it next to dark on light text. The background of a pixel is
 * the mean of the square of AROUND pixels on each side, from a summed
 * area table. 32 bits are enough: 255 * MAXREGION * MAXREGION < 2^32.
 */
static void
polarity(uint8_t *img, int w, int h)
{
	uint32_t *sum, s, n;
	size_t sw = (size_t)w + 1;
	int x, y, x0, y0, x1, y1;

	if ((sum = calloc(sw * ((size_t)h + 1), sizeof(*sum))) == NULL)
		fatal("calloc");
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			sum[(y + 1) * sw + x + 1] = img[(size_t)y * w + x] +
			    sum[y * sw + x + 1] + sum[(y + 1) * sw + x] -
			    sum[y * sw + x];
	for (y = 0; y < h; y++) {
		y0 = y - AROUND > 0 ? y - AROUND : 0;
		y1 = y + AROUND + 1 < h ? y + AROUND + 1 : h;
		for (x = 0; x < w; x++) {
			x0 = x - AROUND > 0 ? x - AROUND : 0;
			x1 = x + AROUND + 1 < w ? x + AROUND + 1 : w;
			/* unsigned wrap cancels out, the result fits */
			s = sum[y1 * sw + x1] - sum[y0 * sw + x1] -
			    sum[y1 * sw + x0] + sum[y0 * sw + x0];
			n = (uint32_t)(x1 - x0) * (y1 - y0);
			if (s < 128 * n)
				img[(size_t)y * w + x] = 255 -
				    img[(size_t)y * w + x];
		}
	}
	free(sum);
}

/* 2x bilinear upscale, small UI fonts OCR much better that way. */
static void
upscale(const uint8_t *in, int w, int h, uint8_t *out)
{
	int i, j, i1, j1, a, b, c, d;
	size_t ow = (size_t)w * 2;

	for (j = 0; j < h; j++) {
		j1 = j + 1 < h ? j + 1 : j;
		for (i = 0; i < w; i++) {
			i1 = i + 1 < w ? i + 1 : i;
			a = in[(size_t)j * w + i];
			b = in[(size_t)j * w + i1];
			c = in[(size_t)j1 * w + i];
			d = in[(size_t)j1 * w + i1];
			out[(size_t)j * 2 * ow + i * 2] = a;
			out[(size_t)j * 2 * ow + i * 2 + 1] = (a + b) / 2;
			out[((size_t)j * 2 + 1) * ow + i * 2] = (a + c) / 2;
			out[((size_t)j * 2 + 1) * ow + i * 2 + 1] =
			    (a + b + c + d) / 4;
		}
	}
}

/* Join the non empty, trimmed lines of s with newlines, in place. */
static void
tidy(char *s)
{
	char *p = s, *line, *end, *o = s;

	while ((line = strsep(&p, "\n")) != NULL) {
		while (isspace((unsigned char)*line))
			line++;
		end = line + strlen(line);
		while (end > line && isspace((unsigned char)end[-1]))
			end--;
		if (end == line)
			continue;
		if (o != s)
			*o++ = '\n';
		memmove(o, line, end - line);
		o += end - line;
	}
	*o = '\0';
}

static char *
ocr(const uint8_t *img, int stride, const int box[4])
{
	char *argv[] = { "tesseract", "stdin", "stdout", NULL };
	char hdr[64], *text, buf[4096];
	uint8_t *flat, *big;
	size_t len = 0, bigsz;
	ssize_t n;
	long long start;
	pid_t pid;
	int in[2], out[2], hlen, y, w = box[2], h = box[3];

	bigsz = (size_t)w * h * 4;
	if ((flat = malloc((size_t)w * h)) == NULL ||
	    (big = malloc(bigsz)) == NULL || (text = malloc(MAXOCR)) == NULL)
		fatal("malloc");
	for (y = 0; y < h; y++)
		memcpy(flat + (size_t)y * w,
		    img + (size_t)(box[1] + y) * stride + box[0], w);
	polarity(flat, w, h);
	upscale(flat, w, h, big);
	free(flat);
	hlen = snprintf(hdr, sizeof(hdr), "P5\n%d %d\n255\n", w * 2, h * 2);

	if (pipe2(in, O_CLOEXEC) == -1 || pipe2(out, O_CLOEXEC) == -1)
		fatal("pipe");
	debug("running tesseract on %dx%d", w * 2, h * 2);
	start = now();
	pid = spawn(argv, in[0], out[1]);
	close(in[0]);
	close(out[1]);

	/* tesseract reads all of its input before writing anything. */
	if (writeall(in[1], hdr, hlen) == -1 ||
	    writeall(in[1], big, bigsz) == -1)
		log_warn("write to tesseract");
	close(in[1]);
	free(big);

	for (;;) {
		if ((n = read(out[0], buf, sizeof(buf))) == -1) {
			if (errno == EINTR)
				continue;
			fatal("read from tesseract");
		}
		if (n == 0)
			break;
		if ((size_t)n > MAXOCR - 1 - len)
			n = MAXOCR - 1 - len;
		memcpy(text + len, buf, n);
		len += n;
	}
	close(out[0]);
	text[len] = '\0';

	if (reap(pid, "tesseract") != 0)
		text[0] = '\0';
	tidy(text);
	debug("tesseract took %lld ms, %zu bytes of text", now() - start,
	    strlen(text));
	return text;
}

/* notify-send bodies may contain markup, escape it. */
static char *
escape(const char *s)
{
	const char *p;
	char *e, *o;
	size_t len = 1;

	for (p = s; *p != '\0'; p++)
		len += *p == '&' ? 5 : *p == '<' || *p == '>' ? 4 : 1;
	if ((e = malloc(len)) == NULL)
		fatal("malloc");
	for (o = e, p = s; *p != '\0'; p++) {
		if (*p == '&')
			o = stpcpy(o, "&amp;");
		else if (*p == '<')
			o = stpcpy(o, "&lt;");
		else if (*p == '>')
			o = stpcpy(o, "&gt;");
		else
			*o++ = *p;
	}
	*o = '\0';
	return e;
}

static void
notify(char *summary, const char *body)
{
	char *argv[] = { "notify-send", "-a", "vmnotify", "--", NULL, NULL,
	    NULL };
	char *e;

	/* OCR text may start with a dash, end the options first */
	e = escape(body);
	argv[4] = summary;
	argv[5] = e;
	debug("running notify-send, summary \"%s\"", summary);
	reap(spawn(argv, -1, -1), "notify-send");
	free(e);
}

/* Main loop */

static void
show(const char *what, const char *text)
{
	char buf[1024];
	size_t i;

	if (!verbose)
		return;
	for (i = 0; i < sizeof(buf) - 1 && text[i] != '\0'; i++)
		buf[i] = text[i] == '\n' ? '|' : text[i];
	buf[i] = '\0';
	if (logsyslog)
		syslog(LOG_INFO, "%s: %s", what, buf);
	else
		printf("%s: %s\n", what, buf);
}

/* Return 1 if text is a toast to forward, sent or already sent. */
static int
toast_seen(char *text, const regex_t *re, char **last)
{
	char *body;

	show("toast area", text);
	if (*text == '\0' || regexec(re, text, 0, NULL, 0) != 0) {
		free(*last);
		*last = NULL;
		free(text);
		return 0;
	}
	if (*last != NULL && strcmp(text, *last) == 0) {
		free(text);
		return 1;
	}
	free(*last);
	*last = text;

	/* first line is the summary, the rest the body */
	if ((body = strchr(text, '\n')) == NULL) {
		notify(text, "");
		return 1;
	}
	*body = '\0';
	notify(text, body + 1);
	*body = '\n';
	return 1;
}

/*
 * Compare two w by h images of the toast area. Return the percentage of
 * pixels differing by more than NOISE, and in span the height of the
 * box around them. Put in box the part to read.
 *
 * A toast whose background differs from what is underneath repaints a
 * solid rectangle. When the blocks of BLOCK pixels that mostly changed
 * cover three quarters of the changes, the box is around them, which
 * leaves out a text line changing underneath at the same time, and solid
 * is set. A toast on a window of the same colour only changes its text,
 * the box is then around all changes. Growing it would take in the cut
 * off bottom of text above the toast. It is the whole area when nothing
 * changed.
 */
static int
compare(const uint8_t *a, const uint8_t *b, int w, int h, int box[4],
    int *solid, int *span)
{
	size_t i, n = 0, nb;
	int *cnt, x, y, bw = w / BLOCK, bh = h / BLOCK;
	int x0 = w, y0 = h, x1 = 0, y1 = 0;
	int dx0 = w, dy0 = h, dx1 = 0, dy1 = 0;

	/* regions are at least 1x1, see parse_region() */
	if (w <= 0 || h <= 0)
		fatalx("empty toast area");
	nb = (size_t)bw * bh;
	if ((cnt = calloc(nb > 0 ? nb : 1, sizeof(*cnt))) == NULL)
		fatal("calloc");
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			i = (size_t)y * w + x;
			if (a[i] <= b[i] + NOISE && b[i] <= a[i] + NOISE)
				continue;
			n++;
			x0 = x < x0 ? x : x0;
			y0 = y < y0 ? y : y0;
			x1 = x + 1 > x1 ? x + 1 : x1;
			y1 = y + 1 > y1 ? y + 1 : y1;
			if (x / BLOCK < bw && y / BLOCK < bh)
				cnt[(size_t)(y / BLOCK) * bw + x / BLOCK]++;
		}
	}
	for (y = 0; y < bh; y++) {
		for (x = 0; x < bw; x++) {
			if (cnt[(size_t)y * bw + x] <= BLOCK * BLOCK / 2)
				continue;
			dx0 = x * BLOCK < dx0 ? x * BLOCK : dx0;
			dy0 = y * BLOCK < dy0 ? y * BLOCK : dy0;
			dx1 = (x + 1) * BLOCK > dx1 ? (x + 1) * BLOCK : dx1;
			dy1 = (y + 1) * BLOCK > dy1 ? (y + 1) * BLOCK : dy1;
		}
	}
	free(cnt);

	*span = n > 0 ? y1 - y0 : 0;
	*solid = dx0 < dx1 && 4 * (size_t)(dx1 - dx0) * (dy1 - dy0) >=
	    3 * (size_t)(x1 - x0) * (y1 - y0);
	if (*solid) {
		box[0] = dx0;
		box[1] = dy0;
		box[2] = dx1 - dx0;
		box[3] = dy1 - dy0;
	} else if (n > 0) {
		box[0] = x0;
		box[1] = y0;
		box[2] = x1 - x0;
		box[3] = y1 - y0;
	} else {
		box[0] = box[1] = 0;
		box[2] = w;
		box[3] = h;
	}
	return (int)(n * 100 / ((size_t)w * h));
}

/* Grow box a to also hold box b. */
static void
unite(int a[4], const int b[4])
{
	int x1, y1;

	x1 = a[0] + a[2] > b[0] + b[2] ? a[0] + a[2] : b[0] + b[2];
	y1 = a[1] + a[3] > b[1] + b[3] ? a[1] + a[3] : b[1] + b[3];
	a[0] = a[0] < b[0] ? a[0] : b[0];
	a[1] = a[1] < b[1] ? a[1] : b[1];
	a[2] = x1 - a[0];
	a[3] = y1 - a[1];
}

/* Nothing would ever be seen there, say so. */
static void
offscreen(const struct vnc *v, const struct region *r, const char *name)
{
	int c[4];

	if (!clip(v, r, c))
		log_warnx("the %s area %d,%d,%d,%d is off the %dx%d screen",
		    name, r->x, r->y, r->w, r->h, v->w, v->h);
}

static void __attribute__((__noreturn__))
watch(struct vnc *v, const struct region *toast, const struct region *badge,
    const regex_t *re, int interval, int cooldown)
{
	struct pollfd pfd;
	uint8_t *cur, *prev, *seen, *base, *bcur = NULL, *bprev = NULL, *tmp;
	size_t len, blen = 0;
	char *lasttext = NULL;
	long long t, changed = 0, nextreq = 0, quiet = 0;
	unsigned int gen;
	int box[4], fresh = 1, dirty, filling = 0, fills = 0, pending = 0;
	int unread = 0, read = 0, showing = 0, doread, pct = 0, solid = 0;
	int span = 0;
	int part[4], last[4];

	len = (size_t)toast->w * toast->h;
	if ((cur = malloc(len)) == NULL || (prev = malloc(len)) == NULL ||
	    (seen = malloc(len)) == NULL || (base = malloc(len)) == NULL)
		fatal("malloc");
	if (badge != NULL) {
		blen = (size_t)badge->w * badge->h;
		if ((bcur = malloc(blen)) == NULL ||
		    (bprev = malloc(blen)) == NULL)
			fatal("malloc");
	}
	pfd.fd = v->fd;
	pfd.events = POLLIN;

	/* nothing is pending yet, servers answer this one at once */
	vnc_refresh(v, toast, badge);
	gen = v->gen;
	dirty = 1;

	for (;;) {
		/*
		 * The server answers an incremental request once something
		 * changed in the area, wait TICK after each answer so that a
		 * server answering at once cannot make us spin. While the
		 * area is being filled after a resize, ask for all of it.
		 *
		 * Waiting for an answer never times out: a VMware server was
		 * seen holding back even a full update until something
		 * changed while another request was pending.
		 */
		t = now();
		if (!pending && t >= nextreq) {
			area(v, toast, badge, box);
			vnc_request(v, !filling, box);
			pending = 1;
			if (filling)
				fills++;
		}

		if (poll(&pfd, 1, TICK) == -1 && errno != EINTR)
			fatal("poll");
		while (poll(&pfd, 1, 0) > 0) {
			if (vnc_message(v) == 0)
				continue;
			pending = 0;
			nextreq = now() + TICK;
			dirty = 1;
		}

		/* after a resize the screen is blank until filled again */
		if (v->gen != gen) {
			gen = v->gen;
			filling = 1;
			fills = 0;
		}
		if (filling) {
			area(v, toast, badge, box);
			if (!covered(v, box)) {
				if (fills < REFRESHES)
					continue;
				log_warnx("screen incomplete after %d "
				    "requests, going on with what came",
				    REFRESHES);
			}
			filling = 0;
			fresh = 1;
		}

		t = now();
		if (dirty) {
			dirty = 0;
			if (fresh) {
				offscreen(v, toast, "toast");
				if (badge != NULL)
					offscreen(v, badge, "badge");
			}
			crop(v, toast, cur);
			if (fresh || memcmp(cur, prev, len) != 0) {
				debug("toast area changed");
				tmp = prev;
				prev = cur;
				cur = tmp;
				changed = t;
				unread = 1;
			}
			if (badge != NULL) {
				crop(v, badge, bcur);
				if (!fresh && memcmp(bcur, bprev, blen) != 0 &&
				    t < quiet)
					debug("taskbar icon changed during the "
					    "cooldown");
				if (!fresh && memcmp(bcur, bprev, blen) != 0 &&
				    t >= quiet) {
					show("taskbar icon", "changed");
					notify("VM", "Taskbar icon changed");
					quiet = t + cooldown * 1000LL;
				}
				tmp = bprev;
				bprev = bcur;
				bcur = tmp;
			}
			fresh = 0;
		}

		/*
		 * Toasts slide in, only look at the area once it stayed the
		 * same for interval seconds. base is the area without a
		 * toast, seen the area at the last look.
		 *
		 * - The first time, read all of it.
		 * - A toast on screen and the area back to base: it is gone.
		 * - No toast on screen: a new one covers much of the area or at
		 *   least two text lines. A smaller change, such as a line in
		 *   a window underneath, is not read; such changes add up
		 *   against base until they are.
		 * - Otherwise read the part that changed, with the toast read
		 *   before so that a replacement keeps the unchanged header.
		 *
		 * When no toast to forward was read, the area becomes base.
		 */
		if (unread && t - changed >= interval * 1000LL) {
			unread = 0;
			doread = 0;
			if (!read) {
				part[0] = part[1] = 0;
				part[2] = toast->w;
				part[3] = toast->h;
				pct = 100;
				memcpy(base, prev, len);
				doread = 1;
			} else if (showing && compare(prev, base, toast->w,
			    toast->h, part, &solid, &span) < MINCHANGE &&
			    span < MINHEIGHT) {
				debug("toast gone");
				showing = 0;
				/* the same text again is a new toast */
				free(lasttext);
				lasttext = NULL;
			} else {
				pct = compare(prev, showing ? seen : base,
				    toast->w, toast->h, part, &solid, &span);
				if (showing) {
					unite(part, last);
					doread = 1;
				} else if (pct >= MINCHANGE ||
				    span >= MINHEIGHT)
					doread = 1;
				else
					debug("toast area %d%% different over "
					    "%d rows, not read", pct, span);
			}
			if (doread) {
				debug("toast area %d%% different, reading "
				    "%dx%d at %d,%d", pct, part[2], part[3],
				    part[0], part[1]);
				read = 1;
				showing = toast_seen(ocr(prev, toast->w, part),
				    re, &lasttext);
				if (showing)
					memcpy(last, part, sizeof(last));
				else
					memcpy(base, prev, len);
			}
			memcpy(seen, prev, len);
		}
	}
}

/* Configuration */

/*
 * ~/.vmnotifyrc holds "keyword value" lines, lines starting with # are
 * comments. A later line for the same keyword replaces an earlier one.
 */
static void
readrc(char *rc[NKEYS])
{
	struct stat st;
	FILE *fp;
	char path[PATH_MAX], *line = NULL, *p, *end;
	const char *home;
	size_t size = 0, klen;
	int fd, k, lineno = 0;

	if ((home = getenv("HOME")) == NULL || *home == '\0')
		return;
	if ((size_t)snprintf(path, sizeof(path), "%s/.vmnotifyrc", home) >=
	    sizeof(path))
		fatalx("HOME is too long");

	/* O_NONBLOCK so that a FIFO does not hang us, it is refused below */
	if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) == -1) {
		if (errno == ENOENT)
			return;
		fatal("%s", path);
	}
	if (fstat(fd, &st) == -1)
		fatal("%s", path);
	if (!S_ISREG(st.st_mode))
		fatalx("%s: not a regular file", path);
	if (st.st_uid != getuid() || (st.st_mode & 077) != 0)
		fatalx("%s: must be owned by you and not accessible by others",
		    path);
	if ((fp = fdopen(fd, "r")) == NULL)
		fatal("%s", path);
	/* no stdio buffer, it would keep a copy of the password */
	setvbuf(fp, NULL, _IONBF, 0);

	while (getline(&line, &size, fp) != -1) {
		lineno++;
		p = line + strspn(line, " \t");
		if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r')
			continue;
		klen = strcspn(p, " \t\r\n");
		for (k = 0; k < NKEYS; k++)
			if (strlen(keywords[k]) == klen &&
			    strncmp(p, keywords[k], klen) == 0)
				break;
		/* the line may hold the password, do not show it */
		if (k == NKEYS)
			fatalx("%s:%d: unknown keyword", path, lineno);
		p += klen;
		p += strspn(p, " \t");
		end = p + strlen(p);
		while (end > p && isspace((unsigned char)end[-1]))
			end--;
		if (end == p)
			fatalx("%s:%d: missing value for %s", path, lineno,
			    keywords[k]);
		if (rc[k] != NULL) {
			explicit_bzero(rc[k], strlen(rc[k]));
			free(rc[k]);
		}
		if ((rc[k] = strndup(p, end - p)) == NULL)
			fatal("strndup");
		debug("%s:%d sets %s", path, lineno, keywords[k]);
	}
	if (ferror(fp))
		fatal("%s", path);
	if (line != NULL) {
		explicit_bzero(line, size);
		free(line);
	}
	fclose(fp);
}

/* Keep a second instance from watching too, hold the lock until exit. */
static int
lock(void)
{
	char path[PATH_MAX];
	const char *dir;
	int fd, n;

	if ((dir = getenv("XDG_RUNTIME_DIR")) != NULL && *dir != '\0')
		n = snprintf(path, sizeof(path), "%s/vmnotify.lock", dir);
	else if ((dir = getenv("HOME")) != NULL && *dir != '\0')
		n = snprintf(path, sizeof(path), "%s/.vmnotify.lock", dir);
	else
		fatalx("neither XDG_RUNTIME_DIR nor HOME is set");
	if (n < 0 || (size_t)n >= sizeof(path))
		fatalx("lock file path is too long");
	debug("lock file %s", path);
	if ((fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) == -1)
		fatal("%s", path);
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		if (errno == EWOULDBLOCK)
			fatalx("already running, see %s", path);
		fatal("%s", path);
	}
	return fd;
}

static void
writepid(int fd)
{
	char buf[32];
	int n;

	n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
	if (ftruncate(fd, 0) == -1 || pwrite(fd, buf, n, 0) != n)
		log_warn("cannot write the pid to the lock file");
}

static void
onsignal(int sig)
{
	(void)sig;
	/* daemon() gave us our own group, the watcher and its commands */
	kill(0, SIGTERM);
	_exit(0);
}

/*
 * With -d, the first process stays as supervisor and runs the watcher in
 * a child, restarted RETRY seconds after it died. Only children return.
 */
static void
supervise(void)
{
	struct sigaction sa;
	pid_t pid;
	int status;

	/*
	 * Stopping the supervisor stops the whole process group. A signal
	 * reaching the watcher before it reset its handlers does the same.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onsignal;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGTERM);
	sigaddset(&sa.sa_mask, SIGINT);
	sigaddset(&sa.sa_mask, SIGHUP);
	if (sigaction(SIGTERM, &sa, NULL) == -1 ||
	    sigaction(SIGINT, &sa, NULL) == -1 ||
	    sigaction(SIGHUP, &sa, NULL) == -1)
		fatal("sigaction");

	for (;;) {
		if ((pid = fork()) == -1)
			fatal("fork");
		if (pid == 0) {
			signal(SIGTERM, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGHUP, SIG_DFL);
			return;
		}
		debug("started watcher %ld", (long)pid);

		while (waitpid(pid, &status, 0) == -1)
			if (errno != EINTR)
				fatal("waitpid");
		if (WIFSIGNALED(status))
			log_warnx("watcher killed by signal %d, restarting "
			    "in %d seconds", WTERMSIG(status), RETRY);
		else
			log_warnx("watcher exited with status %d, restarting "
			    "in %d seconds", WEXITSTATUS(status), RETRY);
		sleep(RETRY);
	}
}

/* Argument parsing */

static long
number(const char *s, long min, long max, const char *what)
{
	char *end;
	long n;

	/* strtol would also take leading blanks and a plus sign */
	if (!isdigit((unsigned char)*s) && *s != '-')
		fatalx("invalid %s: %s", what, s);
	errno = 0;
	n = strtol(s, &end, 10);
	if (*end != '\0' || errno != 0 || n < min || n > max)
		fatalx("invalid %s: %s", what, s);
	return n;
}

static void
parse_region(const char *s, struct region *r)
{
	char buf[64], *p, *f[4];
	int i;

	if (strlen(s) >= sizeof(buf))
		fatalx("invalid region: %s", s);
	strcpy(buf, s);
	p = buf;
	for (i = 0; i < 4; i++)
		if ((f[i] = strsep(&p, ",")) == NULL)
			fatalx("invalid region: %s", s);
	if (p != NULL)
		fatalx("invalid region: %s", s);
	r->x = number(f[0], -MAXDIM, MAXDIM, "region x");
	r->y = number(f[1], -MAXDIM, MAXDIM, "region y");
	r->w = number(f[2], 1, MAXREGION, "region width");
	r->h = number(f[3], 1, MAXREGION, "region height");
}

/* host, host:display or host::port, host may be [address] */
static void
parse_server(char *s, const char **host, char *port, size_t portlen)
{
	char *p, *end;
	long n = 5900;

	if (*s == '[') {
		if ((end = strchr(s, ']')) == NULL)
			fatalx("invalid server: %s", s);
		*host = s + 1;
		p = end + 1;
	} else {
		*host = s;
		end = p = s + strcspn(s, ":");
	}
	if (strncmp(p, "::", 2) == 0)
		n = number(p + 2, 1, 65535, "port");
	else if (*p == ':')
		n = 5900 + number(p + 1, 0, 65535 - 5900, "display");
	else if (*p != '\0')
		fatalx("invalid server: %s", s);
	*end = '\0';
	if (**host == '\0')
		*host = "127.0.0.1";
	snprintf(port, portlen, "%ld", n);
}

int
main(int argc, char *argv[])
{
	struct vnc v = { .fd = -1 };
	struct region toast = { -420, -430, 420, 380 }, badge, *bp = NULL;
	regex_t re;
	const char *host, *shot = NULL;
	char *opt[NKEYS] = { NULL }, *rc[NKEYS] = { NULL }, *envpw = NULL;
	char port[8], *p;
	int ch, error, fd, k, dflag = 0, interval = 1, cooldown = 30;

	/* make sure 0, 1 and 2 are open, children rely on them */
	while ((fd = open("/dev/null", O_RDWR)) != -1 && fd <= 2)
		;
	if (fd > 2)
		close(fd);

	/* waitpid needs SIGCHLD not to be ignored, it may be inherited */
	signal(SIGCHLD, SIG_DFL);

	/* + makes glibc stop at the server like musl does */
	while ((ch = getopt(argc, argv, "+b:c:dhi:m:s:t:Vv")) != -1) {
		switch (ch) {
		case 'b':
			opt[K_BADGE] = optarg;
			break;
		case 'c':
			opt[K_COOLDOWN] = optarg;
			break;
		case 'd':
			dflag = 1;
			break;
		case 'h':
			help();
		case 'i':
			opt[K_INTERVAL] = optarg;
			break;
		case 'm':
			opt[K_MATCH] = optarg;
			break;
		case 's':
			shot = optarg;
			break;
		case 't':
			opt[K_TOAST] = optarg;
			break;
		case 'V':
			debugging = 1;
			verbose = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();
	if (argc == 1)
		opt[K_SERVER] = argv[0];
	if (dflag && shot != NULL)
		fatalx("-d and -s cannot be used together");
	debug("vmnotify %s", VERSION);

	/* the environment wins over the file, empty means unset */
	if ((p = getenv("VNC_PASSWORD")) != NULL && *p != '\0') {
		if ((envpw = strdup(p)) == NULL)
			fatal("strdup");
		opt[K_PASSWORD] = envpw;
		/* also wipe it from what /proc/self/environ shows */
		explicit_bzero(p, strlen(p));
	}
	unsetenv("VNC_PASSWORD");

	readrc(rc);
	for (k = 0; k < NKEYS; k++)
		if (opt[k] == NULL)
			opt[k] = rc[k];

	if (opt[K_BADGE] != NULL) {
		parse_region(opt[K_BADGE], &badge);
		bp = &badge;
	}
	if (opt[K_COOLDOWN] != NULL)
		cooldown = number(opt[K_COOLDOWN], 0, 86400, "cooldown");
	if (opt[K_INTERVAL] != NULL)
		interval = number(opt[K_INTERVAL], 1, 3600, "interval");
	if (opt[K_TOAST] != NULL)
		parse_region(opt[K_TOAST], &toast);
	parse_server(opt[K_SERVER] != NULL ? opt[K_SERVER] :
	    (char []){ "127.0.0.1::5900" }, &host, port, sizeof(port));
	if (opt[K_MATCH] == NULL)
		opt[K_MATCH] = "teams|outlook|firefox";
	if ((error = regcomp(&re, opt[K_MATCH],
	    REG_EXTENDED | REG_ICASE | REG_NOSUB)) != 0) {
		char msg[256];

		regerror(error, &re, msg, sizeof(msg));
		fatalx("match: %s", msg);
	}

	debug("server %s port %s, toast %d,%d,%d,%d, interval %d, match %s",
	    host, port, toast.x, toast.y, toast.w, toast.h, interval,
	    opt[K_MATCH]);
	if (bp != NULL)
		debug("badge %d,%d,%d,%d, cooldown %d", bp->x, bp->y, bp->w,
		    bp->h, cooldown);
	debug("password %s", opt[K_PASSWORD] == NULL ? "not set" :
	    envpw != NULL ? "from VNC_PASSWORD" : "from ~/.vmnotifyrc");

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* tesseract's OpenMP threads make a single small image take seconds */
	if (setenv("OMP_THREAD_LIMIT", "1", 1) == -1)
		fatal("setenv");

	if (shot != NULL) {
		vnc_connect(&v, host, port);
		vnc_handshake(&v, opt[K_PASSWORD]);
		vnc_refresh(&v, NULL, NULL);
		screenshot(&v, shot);
		close(v.fd);
		free(v.fb);
		free(v.got);
		regfree(&re);
		if (envpw != NULL) {
			explicit_bzero(envpw, strlen(envpw));
			free(envpw);
		}
		for (k = 0; k < NKEYS; k++) {
			if (rc[k] != NULL) {
				explicit_bzero(rc[k], strlen(rc[k]));
				free(rc[k]);
			}
		}
		return 0;
	}

	fd = lock();
	if (dflag) {
		if (daemon(0, 0) == -1)
			fatal("daemon");
		openlog("vmnotify", LOG_PID, LOG_USER);
		logsyslog = 1;
		writepid(fd);
		supervise();
	} else
		writepid(fd);

	vnc_connect(&v, host, port);
	vnc_handshake(&v, opt[K_PASSWORD]);
	if (opt[K_PASSWORD] != NULL)
		explicit_bzero(opt[K_PASSWORD], strlen(opt[K_PASSWORD]));
	watch(&v, &toast, bp, &re, interval, cooldown);
}
