NULL =

AM_CPPFLAGS =						\
	-DXDG_APP_BASEDIR=\"$(datadir)/xdg-app\"        \
	-DHELPER=\"$(bindir)/xdg-app-helper\"		\
	$(NULL)

bin_PROGRAMS = \
	xdg-app-helper \
	xdg-app \
	$(NULL)

xdg_app_helper_SOURCES = xdg-app-helper.c

xdg_app_SOURCES = \
	xdg-app-main.c \
	xdg-app-builtins.h \
	xdg-app-builtins-add-repo.c \
	xdg-app-builtins-install.c \
	xdg-app-builtins-update.c \
	xdg-app-builtins-run.c \
	xdg-app-dir.c \
	xdg-app-dir.h \
	xdg-app-utils.h \
	xdg-app-utils.c \
	$(NULL)
xdg_app_LDADD = $(OSTREE_LIBS)
xdg_app_CFLAGS = $(OSTREE_CFLAGS)

install-exec-hook:
	chown root $(bindir)/xdg-app-helper
	chmod u+s $(bindir)/xdg-app-helper
