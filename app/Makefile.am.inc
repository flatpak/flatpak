bin_PROGRAMS += \
	xdg-app \
	$(NULL)

xdg_app_SOURCES = \
	app/xdg-app-main.c \
	app/xdg-app-builtins.h \
	app/xdg-app-builtins-add-remote.c \
	app/xdg-app-builtins-delete-remote.c \
	app/xdg-app-builtins-list-remotes.c \
	app/xdg-app-builtins-ls-remote.c \
	app/xdg-app-builtins-install.c \
	app/xdg-app-builtins-override.c \
	app/xdg-app-builtins-make-current.c \
	app/xdg-app-builtins-update.c \
	app/xdg-app-builtins-uninstall.c \
	app/xdg-app-builtins-list.c \
	app/xdg-app-builtins-info.c \
	app/xdg-app-builtins-run.c \
	app/xdg-app-builtins-enter.c \
	app/xdg-app-builtins-build-init.c \
	app/xdg-app-builtins-build.c \
	app/xdg-app-builtins-build-finish.c \
	app/xdg-app-builtins-build-export.c \
	app/xdg-app-builtins-build-bundle.c \
	app/xdg-app-builtins-build-sign.c \
	app/xdg-app-builtins-repo-update.c \
	app/xdg-app-builtins-document.c \
	$(xdp_dbus_built_sources)	\
	$(NULL)

xdg_app_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(LIBARCHIVE_LIBS) libglnx.la libxdgapp-common.la
xdg_app_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(LIBARCHIVE_CFLAGS)

