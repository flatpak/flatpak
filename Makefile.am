NULL =

if BUILD_DOCUMENTATION
SUBDIRS = doc
endif

AM_CPPFLAGS =						\
	-DXDG_APP_BINDIR=\"$(bindir)\"			\
	-DXDG_APP_SYSTEMDIR=\"$(localstatedir)/xdg-app\"\
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

libexec_PROGRAMS = \
	xdg-app-session-helper \
	$(NULL)

xdg_app_helper_SOURCES = xdg-app-helper.c

dbus_built_sources = xdg-app-dbus.c xdg-app-dbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources) : Makefile.am xdg-app-dbus-interfaces.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.XdgApp.	\
		--c-namespace XdgApp				\
		--generate-c-code xdg-app-dbus			\
		$(srcdir)/xdg-app-dbus-interfaces.xml		\
		$(NULL)

resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/xdg-app.gresource.xml)

xdg-app-resources.h: xdg-app.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _gtk --generate-header

xdg-app-resources.c: xdg-app.gresource.xml $(resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _gtk --generate-source

# D-BUS service file
%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = xdg-app-session.service.in
service_DATA = xdg-app-session.service

xdg_app_session_helper_SOURCES = \
	xdg-app-session-helper.c	\
	$(dbus_built_sources)		\
	xdg-app-resources.h		\
	xdg-app-resources.c		\
	$(NULL)

xdg_app_session_helper_LDADD = $(BASE_LIBS)
xdg_app_session_helper_CFLAGS = $(BASE_CFLAGS)

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
	xdg-app-builtins-repo-update.c \
	xdg-app-dir.c \
	xdg-app-dir.h \
	xdg-app-run.c \
	xdg-app-run.h \
	xdg-app-utils.h \
	xdg-app-utils.c \
	$(dbus_built_sources)		\
	$(NULL)

xdg_app_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS)
xdg_app_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS)

install-exec-hook:
if PRIV_MODE_SETUID
	$(SUDO_BIN) chown root $(DESTDIR)$(bindir)/xdg-app-helper
	$(SUDO_BIN) chmod u+s $(DESTDIR)$(bindir)/xdg-app-helper
else
if PRIV_MODE_FILECAPS
	$(SUDO_BIN) setcap cap_sys_admin,cap_mknod+ep /gnome/bin/xdg-app-helper
endif
endif

completiondir = $(datadir)/bash-completion/completions
completion_DATA = completion/xdg-app
EXTRA_DIST = $(completion_DATA)
