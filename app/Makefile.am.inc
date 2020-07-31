bin_PROGRAMS += \
	flatpak \
	$(NULL)

EXTRA_DIST += app/parse-datetime.y

flatpak_dbus_built_sources = app/flatpak-permission-dbus-generated.c app/flatpak-permission-dbus-generated.h

app/flatpak-permission-dbus-generated.c: data/org.freedesktop.impl.portal.PermissionStore.xml Makefile
	mkdir -p $(builddir)/app
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.impl.portal  \
		--c-namespace XdpDbus                           \
		--generate-c-code $(builddir)/app/flatpak-permission-dbus-generated  \
		$(srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml  \
		$(NULL)

app/%-dbus-generated.h: app/%-dbus-generated.c
	@true # Built as a side-effect of the rules for the .c


if BUILD_SYSTEM_HELPER
polkit_sources = \
	app/flatpak-polkit-agent-text-listener.h \
	app/flatpak-polkit-agent-text-listener.c \
	$(NULL)
endif

noinst_LTLIBRARIES += libflatpak-app.la

libflatpak_app_la_SOURCES = \
	app/flatpak-builtins-utils.h \
	app/flatpak-builtins-utils.c \
	app/flatpak-table-printer.c \
	app/flatpak-table-printer.h \
	app/parse-datetime.h \
	$(NULL)

nodist_libflatpak_app_la_SOURCES = \
	app/parse-datetime.c \
	$(NULL)

libflatpak_app_la_LIBADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(OSTREE_LIBS) \
	$(SOUP_LIBS) \
	$(JSON_LIBS) \
	$(APPSTREAM_GLIB_LIBS) \
	$(SYSTEMD_LIBS) \
	$(POLKIT_LIBS) \
	$(NULL)

libflatpak_app_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(APPSTREAM_GLIB_CFLAGS) \
	 $(SYSTEMD_CFLAGS) \
	$(POLKIT_CFLAGS) \
	-DFLATPAK_COMPILATION \
        -I$(srcdir)/app \
        -I$(builddir)/app \
        -DLOCALEDIR=\"$(localedir)\" \
	$(NULL)

flatpak_SOURCES = \
	app/flatpak-main.c \
	app/flatpak-builtins.h \
	app/flatpak-builtins-remote-add.c \
	app/flatpak-builtins-remote-modify.c \
	app/flatpak-builtins-remote-delete.c \
	app/flatpak-builtins-remote-list.c \
	app/flatpak-builtins-remote-ls.c \
	app/flatpak-builtins-remote-info.c \
	app/flatpak-builtins-install.c \
	app/flatpak-builtins-override.c \
	app/flatpak-builtins-make-current.c \
	app/flatpak-builtins-update.c \
	app/flatpak-builtins-uninstall.c \
	app/flatpak-builtins-mask.c \
	app/flatpak-builtins-pin.c \
	app/flatpak-builtins-list.c \
	app/flatpak-builtins-info.c \
	app/flatpak-builtins-config.c \
	app/flatpak-builtins-run.c \
	app/flatpak-builtins-enter.c \
	app/flatpak-builtins-ps.c \
	app/flatpak-builtins-build-init.c \
	app/flatpak-builtins-build.c \
	app/flatpak-builtins-build-finish.c \
	app/flatpak-builtins-build-export.c \
	app/flatpak-builtins-build-bundle.c \
	app/flatpak-builtins-build-import-bundle.c \
	app/flatpak-builtins-build-commit-from.c \
	app/flatpak-builtins-build-sign.c \
	app/flatpak-builtins-build-update-repo.c \
	app/flatpak-builtins-repo.c \
	app/flatpak-builtins-document-export.c \
	app/flatpak-builtins-document-unexport.c \
	app/flatpak-builtins-document-info.c \
	app/flatpak-builtins-document-list.c \
	app/flatpak-builtins-permission-remove.c \
	app/flatpak-builtins-permission-set.c \
	app/flatpak-builtins-permission-list.c \
	app/flatpak-builtins-permission-show.c \
	app/flatpak-builtins-permission-reset.c \
	app/flatpak-builtins-search.c \
	app/flatpak-builtins-repair.c \
	app/flatpak-builtins-create-usb.c \
	app/flatpak-builtins-kill.c \
	app/flatpak-builtins-history.c \
	app/flatpak-complete.c \
	app/flatpak-complete.h \
	app/flatpak-cli-transaction.c \
	app/flatpak-cli-transaction.h \
	app/flatpak-quiet-transaction.c \
	app/flatpak-quiet-transaction.h \
	app/parse-datetime.h \
	$(polkit_sources) \
	$(NULL)

nodist_flatpak_SOURCES = \
	$(flatpak_dbus_built_sources) \
	$(NULL)

app/parse-datetime.c: app/parse-datetime.y Makefile
	$(AM_V_GEN) $(YACC) $< -o $@

BUILT_SOURCES += $(flatpak_dbus_built_sources)
CLEANFILES += app/parse-datetime.c $(flatpak_dbus_built_sources)

flatpak_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(OSTREE_LIBS) \
	$(SOUP_LIBS) \
	$(JSON_LIBS) \
	$(APPSTREAM_GLIB_LIBS) \
	$(SYSTEMD_LIBS) \
	$(POLKIT_LIBS) \
	libglnx.la \
	libflatpak-app.la \
	libflatpak-common.la \
	libflatpak-common-base.la \
	$(NULL)

flatpak_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(APPSTREAM_GLIB_CFLAGS) \
	$(SYSTEMD_CFLAGS) \
	$(POLKIT_CFLAGS) \
	-DFLATPAK_COMPILATION \
        -I$(srcdir)/app \
        -I$(builddir)/app \
        -DLIBEXECDIR=\"$(libexecdir)\" \
        -DLOCALEDIR=\"$(localedir)\" \
	$(NULL)
