#ifndef __FLATPAK_DOCKER_REFERENCE_H__
#define __FLATPAK_DOCKER_REFERENCE_H__

#include <glib.h>

typedef struct _FlatpakDockerReference FlatpakDockerReference;

FlatpakDockerReference *flatpak_docker_reference_parse (const char *reference_str,
                                                        GError    **error);

const char *flatpak_docker_reference_get_uri        (FlatpakDockerReference *reference);
const char *flatpak_docker_reference_get_repository (FlatpakDockerReference *reference);
const char *flatpak_docker_reference_get_tag        (FlatpakDockerReference *reference);
const char *flatpak_docker_reference_get_digest     (FlatpakDockerReference *reference);

void flatpak_docker_reference_free (FlatpakDockerReference *reference);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FlatpakDockerReference, flatpak_docker_reference_free);

#endif /* __FLATPAK_DOCKER_REFERENCE_H__ */
