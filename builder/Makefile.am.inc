bin_PROGRAMS += \
	xdg-app-builder \
	$(NULL)

xdg_app_builder_SOURCES = \
	builder/xdg-app-builder-main.c \
	builder/builder-manifest.c \
	builder/builder-manifest.h \
	builder/builder-options.c \
	builder/builder-options.h \
	builder/builder-module.c \
	builder/builder-module.h \
	builder/builder-source.c \
	builder/builder-source.h \
	builder/builder-source-archive.c \
	builder/builder-source-archive.h \
	builder/builder-source-git.c \
	builder/builder-source-git.h \
	builder/builder-source-bzr.c \
	builder/builder-source-bzr.h \
	builder/builder-source-file.c \
	builder/builder-source-file.h \
	builder/builder-source-patch.c \
	builder/builder-source-patch.h \
	builder/builder-context.c \
	builder/builder-context.h \
	builder/builder-cache.c \
	builder/builder-cache.h \
	builder/builder-utils.c \
	builder/builder-utils.h \
	$(NULL)

xdg_app_builder_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(JSON_LIBS) $(SOUP_LIBS) $(LIBARCHIVE_LIBS) libglnx.la libxdgapp.la
xdg_app_builder_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(JSON_CFLAGS) $(SOUP_CFLAGS) $(LIBARCHIVE_CFLAGS)

