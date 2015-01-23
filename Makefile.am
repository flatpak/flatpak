NULL =

if BUILD_DOCUMENTATION
SUBDIRS = doc
endif

AM_CPPFLAGS =						\
	-DXDG_APP_BINDIR=\"$(bindir)\"			\
	-DXDG_APP_BASEDIR=\"$(pkgdatadir)\"		\
	-DXDG_APP_TRIGGERDIR=\"$(pkgdatadir)/triggers\" \
	-DHELPER=\"$(bindir)/xdg-app-helper\"		\
	$(NULL)

triggersdir = $(pkgdatadir)/triggers
dist_triggers_SCRIPTS = \
	triggers/gtk-icon-cache.trigger \
	triggers/mime-database.trigger \
	triggers/desktop-database.trigger \
	$(NULL)

bin_PROGRAMS = \
	xdg-app-helper \
	xdg-app \
	$(NULL)

xdg_app_helper_SOURCES = xdg-app-helper.c

xdg_app_SOURCES = \
	xdg-app-main.c \
	xdg-app-builtins.h \
	xdg-app-builtins-add-remote.c \
	xdg-app-builtins-delete-remote.c \
	xdg-app-builtins-list-remotes.c \
	xdg-app-builtins-repo-contents.c \
	xdg-app-builtins-install.c \
	xdg-app-builtins-update.c \
	xdg-app-builtins-uninstall.c \
	xdg-app-builtins-list.c \
	xdg-app-builtins-run.c \
	xdg-app-builtins-build-init.c \
	xdg-app-builtins-build.c \
	xdg-app-builtins-build-finish.c \
	xdg-app-builtins-build-export.c \
	xdg-app-dir.c \
	xdg-app-dir.h \
	xdg-app-utils.h \
	xdg-app-utils.c \
	$(NULL)
xdg_app_LDADD = $(OSTREE_LIBS) $(SOUP_LIBS)
xdg_app_CFLAGS = $(OSTREE_CFLAGS) $(SOUP_CFLAGS)

install-exeic-hook:
	$(SUDO_BIN) chown root $(DESTDIR)$(bindir)/xdg-app-helper
	$(SUDO_BIN) chmod u+s $(DESTDIR)$(bindir)/xdg-app-helper

completiondir = $(datadir)/bash-completion/completions
completion_DATA = completion/xdg-app
EXTRA_DIST = $(completion_DATA)
