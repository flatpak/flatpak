#ifndef XDP_ERROR_H
#define XDP_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * XdpErrorEnum:
 */
typedef enum {
  XDP_ERROR_FAILED     = 0,
  XDP_ERROR_NOT_FOUND,
  XDP_ERROR_NOT_ALLOWED,
  XDP_ERROR_INVALID_ARGUMENT,
} XdpErrorEnum;


#define XDP_ERROR xdp_error_quark()

GQuark  xdp_error_quark      (void);

G_END_DECLS

#endif /* XDP_ERROR_H */
