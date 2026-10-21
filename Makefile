PROG=		vmnotify
PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin
MANDIR?=	${PREFIX}/share/man/man1
CFLAGS?=	-O2 -pipe
CFLAGS+=	-Wall -Wextra

all: ${PROG}

${PROG}: vmnotify.c
	${CC} ${CFLAGS} ${CPPFLAGS} -o $@ vmnotify.c ${LDFLAGS}

install: ${PROG}
	install -d ${DESTDIR}${BINDIR} ${DESTDIR}${MANDIR}
	install -m 755 ${PROG} ${DESTDIR}${BINDIR}
	install -m 644 ${PROG}.1 ${DESTDIR}${MANDIR}

clean:
	rm -f ${PROG}

.PHONY: all install clean
