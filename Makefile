PKGNAME=	totem-3.0
CATEGORIES=	mate
MASTER_SITES=
DISTNAME=

MAINTAINER=	nervoso@k1.com.br
HOMEPAGE=	https://www.gnome.org/projects/totem/
COMMENT=	Movie player for the GNOME/MATE Desktop (GTK3/GStreamer1)

CONFLICTS+=	totem-gtk3-[0-9]*
CONFLICTS+=	totem-[0-9]*

WRKSRC=		${WRKDIR}/${PKGNAME}

USE_LANGUAGES+=	c
USE_TOOLS+=	pkg-config msgfmt

NO_CONFIGURE=	yes

INSTALLATION_DIRS+=	bin
INSTALLATION_DIRS+=	share/applications
INSTALLATION_DIRS+=	share/icons/hicolor/48x48/apps
INSTALLATION_DIRS+=	share/locale/pt_BR/LC_MESSAGES
INSTALLATION_DIRS+=	share/locale/es/LC_MESSAGES
INSTALLATION_DIRS+=	share/locale/fr/LC_MESSAGES
INSTALLATION_DIRS+=	share/locale/ru/LC_MESSAGES

do-extract:
	mkdir -p ${WRKSRC}
	cp -rpf ${FILESDIR}/* ${WRKSRC}

.include "../../x11/gtk3/buildlink3.mk"
.include "../../multimedia/gstreamer1/buildlink3.mk"
.include "../../multimedia/gst-plugins1-base/buildlink3.mk"
DEPENDS+=	gst-plugins1-aom>=1.0:../../multimedia/gst-plugins1-aom
.include "../../multimedia/totem-pl-parser/buildlink3.mk"
.include "../../databases/shared-mime-info/buildlink3.mk"
.include "../../graphics/hicolor-icon-theme/buildlink3.mk"
.include "../../sysutils/desktop-file-utils/desktopdb.mk"
.include "../../mk/bsd.pkg.mk"
