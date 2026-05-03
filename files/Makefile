NOMAN=	yes
BINDIR=	${PREFIX}/bin

PROG=	totem
SRCS=	totem.c

CFLAGS+=	-I/usr/pkg/include/gtk-3.0
CFLAGS+=	-I/usr/pkg/include/pango-1.0
CFLAGS+=	-I/usr/pkg/include/cairo
CFLAGS+=	-I/usr/pkg/include/gdk-pixbuf-2.0
CFLAGS+=	-I/usr/pkg/include/atk-1.0
CFLAGS+=	-I/usr/pkg/include/gstreamer-1.0
CFLAGS+=	-I/usr/pkg/include/gst-plugins-base-1.0
CFLAGS+=	-I/usr/pkg/include/totem-pl-parser/1/plparser

LDFLAGS+=	-L/usr/pkg/lib -Wl,-R/usr/pkg/lib
LDFLAGS+=	-lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0
LDFLAGS+=	-latk-1.0 -lcairo-gobject -lcairo
LDFLAGS+=	-lgdk_pixbuf-2.0 -lgio-2.0
LDFLAGS+=	-lgstreamer-1.0 -lgstvideo-1.0
LDFLAGS+=	-lgobject-2.0 -lglib-2.0
LDFLAGS+=	-ltotem-plparser
LDFLAGS+=	-lm
LDFLAGS+=	-lintl

LOCALES=	pt_BR es fr ru

all: totem mo

mo:
.for L in ${LOCALES}
	msgfmt -o po/${L}.mo po/${L}.po
.endfor

install:
	install -c -o root -g wheel -m 644 totem.png ${DESTDIR}${PREFIX}/share/icons/hicolor/48x48/apps
	install -c -o root -g wheel -m 644 totem.desktop ${DESTDIR}${PREFIX}/share/applications
.for L in ${LOCALES}
	mkdir -p ${DESTDIR}${PREFIX}/share/locale/${L}/LC_MESSAGES
	install -c -o root -g wheel -m 644 po/${L}.mo \
	    ${DESTDIR}${PREFIX}/share/locale/${L}/LC_MESSAGES/totem.mo
.endfor

.include <bsd.prog.mk>
