bin_PROGRAMS += \
	xdg-app \
	$(NULL)

xdg_app_SOURCES = \
	app/flatpak-main.c \
	app/flatpak-builtins.h \
	app/flatpak-builtins-add-remote.c \
	app/flatpak-builtins-delete-remote.c \
	app/flatpak-builtins-list-remotes.c \
	app/flatpak-builtins-ls-remote.c \
	app/flatpak-builtins-install.c \
	app/flatpak-builtins-override.c \
	app/flatpak-builtins-make-current.c \
	app/flatpak-builtins-update.c \
	app/flatpak-builtins-uninstall.c \
	app/flatpak-builtins-list.c \
	app/flatpak-builtins-info.c \
	app/flatpak-builtins-run.c \
	app/flatpak-builtins-enter.c \
	app/flatpak-builtins-build-init.c \
	app/flatpak-builtins-build.c \
	app/flatpak-builtins-build-finish.c \
	app/flatpak-builtins-build-export.c \
	app/flatpak-builtins-build-bundle.c \
	app/flatpak-builtins-build-import-bundle.c \
	app/flatpak-builtins-build-sign.c \
	app/flatpak-builtins-repo-update.c \
	app/flatpak-builtins-document.c \
	$(xdp_dbus_built_sources)	\
	$(NULL)

xdg_app_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(LIBARCHIVE_LIBS) libglnx.la libxdgapp-common.la
xdg_app_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(LIBARCHIVE_CFLAGS)

