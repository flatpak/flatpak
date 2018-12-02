#ifndef __FLATPAK_POLKIT_AGENT_TEXT_LISTENER_H
#define __FLATPAK_POLKIT_AGENT_TEXT_LISTENER_H

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>

G_BEGIN_DECLS

typedef struct _FlatpakPolkitAgentTextListener FlatpakPolkitAgentTextListener;

#define FLATPAK_POLKIT_AGENT_TYPE_TEXT_LISTENER          (flatpak_polkit_agent_text_listener_get_type())
#define FLATPAK_POLKIT_AGENT_TEXT_LISTENER(o)            (G_TYPE_CHECK_INSTANCE_CAST ((o), FLATPAK_POLKIT_AGENT_TYPE_TEXT_LISTENER, FlatpakPolkitAgentTextListener))
#define FLATPAK_POLKIT_AGENT_IS_TEXT_LISTENER(o)         (G_TYPE_CHECK_INSTANCE_TYPE ((o), FLATPAK_POLKIT_AGENT_TYPE_TEXT_LISTENER))

GType                flatpak_polkit_agent_text_listener_get_type (void) G_GNUC_CONST;
PolkitAgentListener *flatpak_polkit_agent_text_listener_new (GCancellable   *cancellable,
                                                             GError        **error);


G_END_DECLS

#endif /* __FLATPAK_POLKIT_AGENT_TEXT_LISTENER_H */

