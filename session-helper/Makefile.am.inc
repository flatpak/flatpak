resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/session-helper/xdg-app.gresource.xml)

libexec_PROGRAMS += \
	xdg-app-session-helper \
	$(NULL)

session-helper/xdg-app-resources.h: session-helper/xdg-app.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _gtk --generate-header

session-helper/xdg-app-resources.c: session-helper/xdg-app.gresource.xml $(resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _gtk --generate-source

# D-BUS service file
%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = session-helper/xdg-app-session.service.in
service_DATA = session-helper/xdg-app-session.service
DISTCLEANFILES += $(service_DATA)

EXTRA_DIST += session-helper/xdg-app.gresource.xml $(service_in_files)

xdg_app_session_helper_SOURCES = \
	session-helper/xdg-app-session-helper.c	\
	session-helper/xdg-app-resources.h	\
	session-helper/xdg-app-resources.c	\
	$(NULL)

xdg_app_session_helper_LDADD = $(BASE_LIBS) libxdgapp.la
xdg_app_session_helper_CFLAGS = $(BASE_CFLAGS)
