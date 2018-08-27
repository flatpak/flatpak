bin_PROGRAMS += \
	flatpak \
	$(NULL)

EXTRA_DIST += app/parse-datetime.y

flatpak_dbus_built_sources = app/flatpak-permission-dbus-generated.c app/flatpak-permission-dbus-generated.h

app/flatpak-permission-dbus-generated.c: data/org.freedesktop.impl.portal.PermissionStore.xml Makefile
	mkdir -p $(builddir)/common
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.impl.portal  \
		--c-namespace XdpDbus                           \
		--generate-c-code $(builddir)/app/flatpak-permission-dbus-generated  \
		$(srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml  \
		$(NULL)

app/%-dbus-generated.h: app/%-dbus-generated.c
	@true # Built as a side-effect of the rules for the .c

flatpak_SOURCES = \
	app/flatpak-main.c \
	app/flatpak-builtins.h \
	app/flatpak-builtins-utils.h \
	app/flatpak-builtins-utils.c \
	app/flatpak-builtins-add-remote.c \
	app/flatpak-builtins-delete-remote.c \
	app/flatpak-builtins-list-remotes.c \
	app/flatpak-builtins-ls-remote.c \
	app/flatpak-builtins-info-remote.c \
	app/flatpak-builtins-install.c \
	app/flatpak-builtins-override.c \
	app/flatpak-builtins-make-current.c \
	app/flatpak-builtins-update.c \
	app/flatpak-builtins-uninstall.c \
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
	app/flatpak-builtins-repo-update.c \
	app/flatpak-builtins-repo.c \
	app/flatpak-builtins-document-export.c \
	app/flatpak-builtins-document-unexport.c \
	app/flatpak-builtins-document-info.c \
	app/flatpak-builtins-document-list.c \
	app/flatpak-builtins-permission-remove.c \
	app/flatpak-builtins-permission-list.c \
	app/flatpak-builtins-permission-show.c \
	app/flatpak-builtins-permission-reset.c \
	app/flatpak-builtins-search.c \
	app/flatpak-builtins-repair.c \
	app/flatpak-builtins-create-usb.c \
	app/flatpak-table-printer.c \
	app/flatpak-table-printer.h \
	app/flatpak-complete.c \
	app/flatpak-complete.h \
	app/flatpak-cli-transaction.c \
	app/flatpak-cli-transaction.h \
	app/parse-datetime.h \
	$(NULL)

nodist_flatpak_SOURCES = \
	$(flatpak_dbus_built_sources) \
	app/parse-datetime.c \
	$(NULL)

app/parse-datetime.c: app/parse-datetime.y Makefile
	$(AM_V_GEN) $(YACC) $< -o $@

BUILT_SOURCES += $(flatpak_dbus_built_sources)
CLEANFILES += app/parse-datetime.c $(flatpak_dbus_built_sources)

flatpak_LDADD = $(AM_LDADD) $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(APPSTREAM_GLIB_LIBS) \
	libglnx.la libflatpak-common.la
flatpak_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(APPSTREAM_GLIB_CFLAGS) \
	-DFLATPAK_COMPILATION \
        -I$(srcdir)/app \
        -I$(builddir)/app \
        -DLOCALEDIR=\"$(localedir)\"
