EXTRA_DIST += icon-validator/meson.build

libexec_PROGRAMS += \
        flatpak-validate-icon \
        $(NULL)

flatpak_validate_icon_SOURCES = icon-validator/validate-icon.c
flatpak_validate_icon_LDADD = $(GDK_PIXBUF_LIBS)
flatpak_validate_icon_CFLAGS = $(AM_CFLAGS) $(GDK_PIXBUF_CFLAGS) -DLIBEXECDIR=\"$(libexecdir)\"
