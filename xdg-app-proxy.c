/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include <unistd.h>
#include <string.h>

#include "xdg-app-proxy.h"

#include <gio/gunixsocketaddress.h>
#include <gio/gunixconnection.h>
#include <gio/gunixfdmessage.h>

/**
 * The proxy listens to a unix domain socket, and for each new
 * connection it opens up a new connection to a specified dbus bus
 * address (typically the session bus) and forwards data between the
 * two.  During the authentication phase all data is forwarded as
 * received, and additionally for the first 1 byte zero we also send
 * the proxy credentials to the bus.
 *
 * Once the connection is authenticated there are two modes, filtered
 * and unfiltered. In the unfiltered mode we just send all messages on
 * as we receive, but in the in the filtering mode we apply a policy,
 * which is similar to the policy supported by kdbus.
 *
 * The policy for the filtering consists of a mapping from well known
 * names to a policy that is either SEE, TALK or OWN. The default
 * initial policy is that the the user is only allowed to TALK to the
 * bus itself (org.freedesktop.DBus, or no destination specified), and
 * TALK to its own unique id. All other clients are invisible. The
 * well known names can be specified exactly, or as a simple one-level
 * wildcard like "org.foo.*" which matches "org.foo.bar", but not
 * "org.foobar" or "org.foo.bar.gazonk".
 *
 * Polices are specified for well known names, but they also affect
 * the owner of that name, so that the policy for a unique id is the
 * superset of the polices for all the names it owns. Due to technical
 * reasons the policies for a name are only in effect once the client
 * is told of them in the course of normal communications. For
 * instance, there is no way the proxy could know that :1.55 owns
 * org.freedesktop.ScreenSaver until it has sent a message to the name
 * and got a reply to it, or it has called GetNameOwner, so :1.55 will
 * be invisible until such a call happens, but then it will get
 * assigned the policy based on the org.freedesktop.ScreenSaver
 * policy.
 *
 * Additionally, the policy for a unique name is "sticky", in that we
 * keep the highest policy granted by a once-owned name even when the
 * client releases that name. This is impossible to avoid in a
 * race-free way in a proxy. But this is rarely a problem in practice,
 * as clients rarely release names and stay on the bus.
 *
 * Here is a desciption of the policy levels:
 * (all policy levels also imply the ones before it)
 *
 *  SEE:
 *    The name/id is visible in the ListNames reply
 *    The name/id is visible in the ListActivatableNames reply
 *    You can call GetNameOwner on the name
 *    You can call NameHasOwner on the name
 *    You see NameOwnerChanged signals on the name
 *    You see NameOwnerChanged signals on the client when it disconnects
 *    You can call the GetXXX methods on the name/id to get e.g. the peer pid
 *    You get AccessDenied rather than NameHasNoOwner when sending messages to the name/id
 *
 * TALK:
 *    You can send method calls and signals to the name/id
 *    You will receive broadcast signals from the name/id (if you have a match rule for them)
 *    You can call StartServiceByName on the name
 *
 * OWN:
 *    You are allowed to call RequestName/ReleaseName/ListQueuedOwners on the name.
 *
 * The policy apply only to signals and method calls. All replies
 * (errors or method returns) are allowed once for an outstanding
 * method call, and never otherwise.
 *
 * Every peer on the bus is considered priviledged, and we thus trust
 * it. So we rely on similar proxies to be running for all untrusted
 * clients. Any such priviledged peer is allowed to send method call
 * or unicast signal messages to the proxied client. Once another peer
 * sends you a message that peer unique id is now made visible (policy
 * SEE) to the proxied client, allowing it the client to track caller
 * lifetimes via NameOwnerChanged signals.
 *
 * Differences to kdbus custom endpoint policies:
 *
 *  * The proxy will return the credentials (like pid) of the proxy,
 *    not the real client.
 *
 *  * Only the names this client has seen so far that a peer owns
 *    affect the policy.
 *
 *  * Policy is not dropped when a peer releases a name.
 *
 *  * Peers that call you become visible (SEE) (and get signals for
 *    NameOwnerChange disconnect) In kdbus currently custom endpoints
 *    never get NameOwnerChange signals for unique ids, but this is
 *    problematic as it disallows a services to track lifetimes of its
 *    clients.
 *
 * Mode of operation
 *
 * Once authenticated we receive incoming messagages one at a time,
 * and then we demarshal the message headers to make routing decisions
 * on. This means we trust the bus bus to do message format validation
 * etc (because we don't parse the body). Also we assume that the bus
 * verifies reply_serials, i.e. that a reply can only be sent once and
 * by the real recipient of an previously sent method call.
 *
 * We don't however trust the serials from the client. We verify that
 * they are strictly increasing to make sure the code is not confused
 * by serials being reused.
 *
 * The filter is strictly passive, in that we never construct our own
 * requests. For each message received from the client we look up the
 * type and the destination policy and make a decision to either pass
 * it on as is, rewrite it before passing on (for instance ListName
 * replies), drop it completely, or return a made up reply/error to
 * the sender.
 *
 * When returning a made up reply we replace the actual message with a
 * Ping request to the bus with the same serial and replace the
 * resulting reply with the made up reply (with the serial from the
 * ping reply). This means we keep the strict message ordering and
 * serial numbers of the bus.
 *
 * Policy is applied to unique ids in the following cases:
 *  * When we get a method call from a unique id, it gets SEE
 *  * When we do a method call on a well known name, we take
 *    the unique id from the reply and apply the policy of the
 *    name to it.
 *  * If we get a response from GetNameOwner we apply to that
 *    unique id the policy of the name.
 *  * When we get a reply to the initial Hello request we give
 *    our own assigned unique id TALK.
 *
 * All messages sent to the bus itself are fully demarshalled
 * and handled on a per-method basis:
 *
 * Hello, AddMatch, RemoveMatch, GetId: Always allowed
 * ListNames, ListActivatableNames: Always allowed, but response filtered
 * UpdateActivationEnvironment, BecomeMonitor: Always denied
 * RequestName, ReleaseName, ListQueuedOwners: Only allowed if arg0 is a name with policy OWN
 * NameHasOwner, GetNameOwner: Only pass on if arg0 is a name with policy SEE, otherwise return fake reply
 * StartServiceByName: Only allowed if policy TALK on arg0
 * GetConnectionUnixProcessID, GetConnectionCredentials,
 *  GetAdtAuditSessionData, GetConnectionSELinuxSecurityContext,
 *  GetConnectionUnixUser: Allowed if policy SEE on arg0
 *
 * For unknown methods, we return a fake error.
 *
 */

typedef struct XdgAppProxyClient XdgAppProxyClient;

XdgAppPolicy xdg_app_proxy_get_policy (XdgAppProxy *proxy, const char *name);

typedef enum {
  EXPECTED_REPLY_NONE,
  EXPECTED_REPLY_NORMAL,
  EXPECTED_REPLY_HELLO,
  EXPECTED_REPLY_GET_NAME_OWNER,
  EXPECTED_REPLY_LIST_NAMES,
  EXPECTED_REPLY_REWRITE,
} ExpectedReplyType;

typedef struct {
  gsize size;
  gsize pos;
  gboolean send_credentials;
  GList *control_messages;

  guchar data[16];
  /* data continues here */
} Buffer;

typedef struct {
  gboolean big_endian;
  guchar type;
  guchar flags;
  guint32 length;
  guint32 serial;
  const char *path;
  const char *interface;
  const char *member;
  const char *error_name;
  const char *destination;
  const char *sender;
  const char *signature;
  gboolean has_reply_serial;
  guint32 reply_serial;
  guint32 unix_fds;
} Header;

typedef struct {
  gboolean got_first_byte; /* always true on bus side */
  gboolean closed; /* always true on bus side */

  XdgAppProxyClient *client;
  GSocketConnection *connection;
  GSource *in_source;
  GSource *out_source;

  Buffer *current_read_buffer;
  Buffer header_buffer;

  GList *buffers; /* to be sent */
  GList *control_messages;

  GHashTable *expected_replies;
} ProxySide;

struct XdgAppProxyClient {
  GObject parent;

  XdgAppProxy *proxy;

  gboolean authenticated;

  ProxySide client_side;
  ProxySide bus_side;

  /* Filtering data: */
  guint32 last_serial;
  GHashTable *rewrite_reply;
  GHashTable *named_reply;
  GHashTable *get_owner_reply;

  GHashTable *unique_id_policy;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppProxyClientClass;

struct XdgAppProxy {
  GSocketService parent;

  gboolean log_messages;

  GList *clients;
  char *socket_path;
  char *dbus_address;

  gboolean filter;

  GHashTable *wildcard_policy;
  GHashTable *policy;
};

typedef struct {
  GSocketServiceClass parent_class;
} XdgAppProxyClass;


enum {
  PROP_0,

  PROP_DBUS_ADDRESS,
  PROP_SOCKET_PATH
};

#define XDG_APP_TYPE_PROXY xdg_app_proxy_get_type()
#define XDG_APP_PROXY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_PROXY, XdgAppProxy))
#define XDG_APP_IS_PROXY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_PROXY))


#define XDG_APP_TYPE_PROXY_CLIENT xdg_app_proxy_client_get_type()
#define XDG_APP_PROXY_CLIENT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_PROXY_CLIENT, XdgAppProxyClient))
#define XDG_APP_IS_PROXY_CLIENT(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_PROXY_CLIENT))

GType xdg_app_proxy_client_get_type (void);

G_DEFINE_TYPE (XdgAppProxy, xdg_app_proxy, G_TYPE_SOCKET_SERVICE)
G_DEFINE_TYPE (XdgAppProxyClient, xdg_app_proxy_client, G_TYPE_OBJECT)

static void
buffer_free (Buffer *buffer)
{
  g_list_free_full (buffer->control_messages, g_object_unref);
  g_free (buffer);
}

static void
free_side (ProxySide *side)
{
  g_clear_object (&side->connection);

  g_list_free_full (side->buffers, (GDestroyNotify)buffer_free);
  g_list_free_full (side->control_messages, (GDestroyNotify)g_object_unref);

  if (side->in_source)
    g_source_destroy (side->in_source);
  if (side->out_source)
    g_source_destroy (side->out_source);

  g_hash_table_destroy (side->expected_replies);
}

static void
xdg_app_proxy_client_finalize (GObject *object)
{
  XdgAppProxyClient *client = XDG_APP_PROXY_CLIENT (object);

  client->proxy->clients = g_list_remove (client->proxy->clients, client);
  g_clear_object (&client->proxy);

  g_hash_table_destroy (client->rewrite_reply);
  g_hash_table_destroy (client->named_reply);
  g_hash_table_destroy (client->get_owner_reply);
  g_hash_table_destroy (client->unique_id_policy);

  free_side (&client->client_side);
  free_side (&client->bus_side);

  G_OBJECT_CLASS (xdg_app_proxy_client_parent_class)->finalize (object);
}

static void
xdg_app_proxy_client_class_init (XdgAppProxyClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdg_app_proxy_client_finalize;
}

static void
init_side (XdgAppProxyClient *client, ProxySide *side)
{
  side->got_first_byte = (side == &client->bus_side);
  side->client = client;
  side->header_buffer.size = 16;
  side->header_buffer.pos = 0;
  side->current_read_buffer = &side->header_buffer;
  side->expected_replies = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static void
xdg_app_proxy_client_init (XdgAppProxyClient *client)
{
  init_side (client, &client->client_side);
  init_side (client, &client->bus_side);

  client->rewrite_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
  client->named_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  client->get_owner_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  client->unique_id_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

XdgAppProxyClient *
xdg_app_proxy_client_new (XdgAppProxy *proxy, GSocketConnection *connection)
{
  XdgAppProxyClient *client;

  client = g_object_new (XDG_APP_TYPE_PROXY_CLIENT, NULL);
  client->proxy = g_object_ref (proxy);
  client->client_side.connection = g_object_ref (connection);

  proxy->clients = g_list_prepend (proxy->clients, client);

  return client;
}

XdgAppPolicy
xdg_app_proxy_get_policy (XdgAppProxy *proxy,
                          const char *name)
{
  guint policy, wildcard_policy;
  char *dot;
  char buffer[256];

  policy = GPOINTER_TO_INT (g_hash_table_lookup (proxy->policy, name));

  dot = strrchr (name, '.');
  if (dot && (dot - name) <= 255)
    {
      strncpy (buffer, name, dot - name);
      buffer[dot-name] = 0;
      wildcard_policy = GPOINTER_TO_INT (g_hash_table_lookup (proxy->wildcard_policy, buffer));
      policy = MAX (policy, wildcard_policy);
    }

  return policy;
}

void
xdg_app_proxy_set_filter (XdgAppProxy *proxy,
                          gboolean filter)
{
  proxy->filter = filter;
}

void
xdg_app_proxy_set_log_messages (XdgAppProxy *proxy,
                                gboolean log)
{
  proxy->log_messages = log;
}

void
xdg_app_proxy_add_policy (XdgAppProxy *proxy,
                          const char *name,
                          XdgAppPolicy policy)
{
  g_hash_table_replace (proxy->policy, g_strdup (name), GINT_TO_POINTER (policy));
}

void
xdg_app_proxy_add_wildcarded_policy (XdgAppProxy *proxy,
                                     const char *name,
                                     XdgAppPolicy policy)
{
  g_hash_table_replace (proxy->wildcard_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
xdg_app_proxy_finalize (GObject *object)
{
  XdgAppProxy *proxy = XDG_APP_PROXY (object);

  g_clear_pointer (&proxy->dbus_address, g_free);
  g_assert (proxy->clients == NULL);

  g_hash_table_destroy (proxy->policy);
  g_hash_table_destroy (proxy->wildcard_policy);

  G_OBJECT_CLASS (xdg_app_proxy_parent_class)->finalize (object);
}

static void
xdg_app_proxy_set_property (GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec)
{
  XdgAppProxy *proxy = XDG_APP_PROXY (object);

  switch (prop_id)
    {
    case PROP_DBUS_ADDRESS:
      proxy->dbus_address = g_value_dup_string (value);
      break;
    case PROP_SOCKET_PATH:
      proxy->socket_path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_proxy_get_property (GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec)
{
  XdgAppProxy *proxy = XDG_APP_PROXY (object);

  switch (prop_id)
    {
    case PROP_DBUS_ADDRESS:
      g_value_set_string (value, proxy->dbus_address);
      break;
    case PROP_SOCKET_PATH:
      g_value_set_string (value, proxy->socket_path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static Buffer *
buffer_new (gsize size, Buffer *old)
{
  Buffer *buffer = g_malloc0 (sizeof (Buffer) + size - 16);

  buffer->control_messages = NULL;
  buffer->size = size;

  if (old)
    {
      buffer->pos = old->pos;
      /* Takes ownership of any old control messages */
      buffer->control_messages = old->control_messages;
      old->control_messages = NULL;

      g_assert (size >= old->size);
      memcpy (buffer->data, old->data, old->size);
    }

  return buffer;
}

static ProxySide *
get_other_side (ProxySide *side)
{
  XdgAppProxyClient *client = side->client;

  if (side == &client->client_side)
    return &client->bus_side;

  return &client->client_side;
}

static void
side_closed (ProxySide *side)
{
  GSocket *socket, *other_socket;
  ProxySide *other_side = get_other_side (side);

  if (side->closed)
    return;

  socket = g_socket_connection_get_socket (side->connection);
  g_socket_close (socket, NULL);
  side->closed = TRUE;

  other_socket = g_socket_connection_get_socket (other_side->connection);
  if (!other_side->closed && other_side->buffers == NULL)
    {
      other_socket = g_socket_connection_get_socket (other_side->connection);
      g_socket_close (other_socket, NULL);
      other_side->closed = TRUE;
    }

  if (other_side->closed)
    g_object_unref (side->client);
  else
    {
      GError *error = NULL;

      other_socket = g_socket_connection_get_socket (other_side->connection);
      if (!g_socket_shutdown (other_socket, TRUE, FALSE, &error))
        {
          g_warning ("Unable to shutdown read side: %s", error->message);
          g_error_free (error);
        }
    }
}

static gboolean
buffer_read (ProxySide *side,
             Buffer *buffer,
             GSocket *socket)
{
  gssize res;
  GInputVector v;
  GError *error = NULL;
  GSocketControlMessage **messages;
  int num_messages, i;

  v.buffer = &buffer->data[buffer->pos];
  v.size = buffer->size - buffer->pos;

  res = g_socket_receive_message (socket, NULL, &v, 1,
                                  &messages,
                                  &num_messages,
                                  G_SOCKET_MSG_NONE, NULL, &error);
  if (res < 0 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    {
      g_error_free (error);
      return FALSE;
    }

  if (res <= 0)
    {
      if (res != 0)
        {
          g_warning ("Error reading from socket: %s", error->message);
          g_error_free (error);
        }

      side_closed (side);
      return FALSE;
    }

  for (i = 0; i < num_messages; i++)
    buffer->control_messages = g_list_append (buffer->control_messages, messages[i]);

  g_free (messages);

  buffer->pos += res;
  return buffer->pos == buffer->size;
}

static gboolean
buffer_write (ProxySide *side,
              Buffer *buffer,
              GSocket *socket)
{
  gssize res;
  GOutputVector v;
  GError *error = NULL;
  GSocketControlMessage **messages = NULL;
  int i, n_messages;
  GList *l;

  if (buffer->send_credentials &&
      G_IS_UNIX_CONNECTION (side->connection))
    {
      g_assert (buffer->size == 1);

      if (!g_unix_connection_send_credentials (G_UNIX_CONNECTION (side->connection),
                                               NULL,
                                               &error))
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            {
              g_error_free (error);
              return FALSE;
            }

          g_warning ("Error writing credentials to socket: %s", error->message);
          g_error_free (error);

          side_closed (side);
          return FALSE;
        }

      buffer->pos = 1;
      return TRUE;
    }

  n_messages = g_list_length (buffer->control_messages);
  messages = g_new (GSocketControlMessage *, n_messages);
  for (l = buffer->control_messages, i = 0; l != NULL ; l = l->next, i++)
    messages[i] = l->data;

  v.buffer = &buffer->data[buffer->pos];
  v.size = buffer->size - buffer->pos;

  res = g_socket_send_message (socket, NULL, &v, 1,
                               messages, n_messages,
                               G_SOCKET_MSG_NONE, NULL, &error);
  g_free (messages);
  if (res < 0 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    {
      g_error_free (error);
      return FALSE;
    }

  if (res <= 0)
    {
      if (res < 0)
        {
          g_warning ("Error writing credentials to socket: %s", error->message);
          g_error_free (error);
        }

      side_closed (side);
      return FALSE;
    }

  g_list_free_full (buffer->control_messages, g_object_unref);

  buffer->pos += res;
  return buffer->pos == buffer->size;
}

static gboolean
side_out_cb (GSocket *socket, GIOCondition condition, gpointer user_data)
{
  ProxySide *side = user_data;
  XdgAppProxyClient *client = side->client;
  gboolean retval = G_SOURCE_CONTINUE;

  g_object_ref (client);

  if (side->buffers)
    {
      Buffer *buffer = side->buffers->data;

      if (buffer_write (side, buffer, socket))
        {
          side->buffers = g_list_delete_link (side->buffers, side->buffers);
          buffer_free (buffer);
        }
    }

  if (side->buffers == NULL)
    {
      ProxySide *other_side = get_other_side (side);

      side->out_source = NULL;
      retval = G_SOURCE_REMOVE;

      if (other_side->closed)
        side_closed (side);
    }

  g_object_unref (client);

  return retval;
}

static void
queue_expected_reply (ProxySide *side, guint32 serial, ExpectedReplyType type)
{
  g_hash_table_replace (side->expected_replies,
			GUINT_TO_POINTER (serial),
			GUINT_TO_POINTER (type));
}

static ExpectedReplyType
steal_expected_reply (ProxySide *side, guint32 serial)
{
  ExpectedReplyType type;

  type = GPOINTER_TO_UINT (g_hash_table_lookup (side->expected_replies,
						GUINT_TO_POINTER (serial)));
  if (type)
    g_hash_table_remove (side->expected_replies,
			 GUINT_TO_POINTER (serial));
  return type;
}


static void
queue_outgoing_buffer (ProxySide *side, Buffer *buffer)
{
  if (side->out_source == NULL)
    {
      GSocket *socket;

      socket = g_socket_connection_get_socket (side->connection);
      side->out_source = g_socket_create_source (socket, G_IO_OUT, NULL);
      g_source_set_callback (side->out_source, (GSourceFunc)side_out_cb, side, NULL);
      g_source_attach (side->out_source, NULL);
      g_source_unref (side->out_source);
    }

  buffer->pos = 0;
  side->buffers = g_list_append (side->buffers, buffer);
}

static guint32
read_uint32 (Header *header, guint8 *ptr)
{
  if (header->big_endian)
    return GUINT32_FROM_BE (*(guint32 *)ptr);
  else
    return GUINT32_FROM_LE (*(guint32 *)ptr);
}

static guint32
align_by_8 (guint32 offset)
{
  return 8 * ((offset + 7)/8);
}

static guint32
align_by_4 (guint32 offset)
{
  return 4 * ((offset + 3)/4);
}

static const char *
get_signature (Buffer *buffer, guint32 *offset, guint32 end_offset)
{
  guint8 len;
  char *str;

  if (*offset >= end_offset)
    return FALSE;

  len = buffer->data[*offset];
  (*offset)++;

  if ((*offset) + len + 1 > end_offset)
    return FALSE;

  if (buffer->data[(*offset) + len] != 0)
    return FALSE;

  str = (char *)&buffer->data[(*offset)];
  *offset += len + 1;

  return str;
}

static const char *
get_string (Buffer *buffer, Header *header, guint32 *offset, guint32 end_offset)
{
  guint8 len;
  char *str;

  *offset = align_by_4 (*offset);
  if (*offset + 4  >= end_offset)
    return FALSE;

  len = read_uint32 (header, &buffer->data[*offset]);
  *offset += 4;

  if ((*offset) + len + 1 > end_offset)
    return FALSE;

  if (buffer->data[(*offset) + len] != 0)
    return FALSE;

  str = (char *)&buffer->data[(*offset)];
  *offset += len + 1;

  return str;
}

static gboolean
parse_header (Buffer *buffer, Header *header)
{
  guint32 array_len, header_len;
  guint32 offset, end_offset;
  guint8 header_type;
  const char *signature;

  memset (header, 0, sizeof (Header));

  if (buffer->size < 16)
    return FALSE;

  if (buffer->data[3] != 1) /* Protocol version */
    return FALSE;

  if (buffer->data[0] == 'B')
    header->big_endian = TRUE;
  else if (buffer->data[0] == 'l')
    header->big_endian = FALSE;
  else
    return FALSE;

  header->type = buffer->data[1];
  header->flags = buffer->data[2];

  header->length = read_uint32 (header, &buffer->data[4]);
  header->serial = read_uint32 (header, &buffer->data[8]);

  if (header->serial == 0)
    return FALSE;

  array_len = read_uint32 (header, &buffer->data[12]);

  header_len = align_by_8 (12 + 4 + array_len);
  g_assert (buffer->size >= header_len); /* We should have verified this when reading in the message */
  if (header_len > buffer->size)
    return FALSE;

  offset = 12 + 4;
  end_offset = offset + array_len;

  while (offset < end_offset)
    {
      offset = align_by_8 (offset); /* Structs must be 8 byte aligned */
      if (offset >= end_offset)
        return FALSE;

      header_type = buffer->data[offset++];
      if (offset >= end_offset)
        return FALSE;

      signature = get_signature (buffer, &offset, end_offset);
      if (signature == NULL)
        return FALSE;

      switch (header_type)
        {
        case G_DBUS_MESSAGE_HEADER_FIELD_INVALID:
          return FALSE;

        case G_DBUS_MESSAGE_HEADER_FIELD_PATH:
          if (strcmp (signature, "o") != 0)
            return FALSE;
          header->path = get_string (buffer, header, &offset, end_offset);
          if (header->path == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_INTERFACE:
          if (strcmp (signature, "s") != 0)
            return FALSE;
          header->interface = get_string (buffer, header, &offset, end_offset);
          if (header->interface == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_MEMBER:
          if (strcmp (signature, "s") != 0)
            return FALSE;
          header->member = get_string (buffer, header, &offset, end_offset);
          if (header->member == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_ERROR_NAME:
          if (strcmp (signature, "s") != 0)
            return FALSE;
          header->error_name = get_string (buffer, header, &offset, end_offset);
          if (header->error_name == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_REPLY_SERIAL:
          if (offset + 4 > end_offset)
            return FALSE;

          header->has_reply_serial = TRUE;
          header->reply_serial = read_uint32 (header, &buffer->data[offset]);
          offset += 4;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_DESTINATION:
          if (strcmp (signature, "s") != 0)
            return FALSE;
          header->destination = get_string (buffer, header, &offset, end_offset);
          if (header->destination == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_SENDER:
          if (strcmp (signature, "s") != 0)
            return FALSE;
          header->sender = get_string (buffer, header, &offset, end_offset);
          if (header->sender == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_SIGNATURE:
          if (strcmp (signature, "g") != 0)
            return FALSE;
          header->signature = get_signature (buffer, &offset, end_offset);
          if (header->signature == NULL)
            return FALSE;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_NUM_UNIX_FDS:
          if (offset + 4 > end_offset)
            return FALSE;

          header->unix_fds = read_uint32 (header, &buffer->data[offset]);
          offset += 4;
          break;

        default:
          /* Unknown header field, for safety, fail parse */
          return FALSE;
        }
    }

  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      if (header->path == NULL || header->member == NULL)
        return FALSE;
      break;

    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
      if (!header->has_reply_serial)
        return FALSE;
      break;

    case G_DBUS_MESSAGE_TYPE_ERROR:
      if (header->error_name  == NULL || !header->has_reply_serial)
        return FALSE;
      break;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
      if (header->path == NULL ||
          header->interface == NULL ||
          header->member == NULL)
        return FALSE;
      if (strcmp (header->path, "/org/freedesktop/DBus/Local") == 0 ||
          strcmp (header->interface, "org.freedesktop.DBus.Local") == 0)
        return FALSE;
      break;
    default:
      /* Unknown message type, for safety, fail parse */
      return FALSE;
    }

  return TRUE;
}

static void
print_outgoing_header (Header *header)
{
  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      g_print ("C%d: -> %s call %s.%s at %s\n",
               header->serial,
               header->destination ? header->destination : "(no dest)",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;

    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
      g_print ("C%d: -> %s return from B%d\n",
               header->serial,
               header->destination ? header->destination : "(no dest)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_ERROR:
      g_print ("C%d: -> %s return error %s from B%d\n",
               header->serial,
               header->destination ? header->destination : "(no dest)",
               header->error_name ? header->error_name : "(no error)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
      g_print ("C%d: -> %s signal %s.%s at %s\n",
               header->serial,
               header->destination ? header->destination : "all",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;
    default:
      g_print ("unknown message type\n");
    }
}

static void
print_incoming_header (Header *header)
{
  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      g_print ("B%d: <- %s call %s.%s at %s\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;

    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
      g_print ("B%d: <- %s return from C%d\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_ERROR:
      g_print ("B%d: <- %s return error %s from C%d\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->error_name ? header->error_name : "(no error)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
      g_print ("B%d: <- %s signal %s.%s at %s\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;
    default:
      g_print ("unknown message type\n");
    }
}

static XdgAppPolicy
xdg_app_proxy_client_get_policy (XdgAppProxyClient *client, const char *source)
{
  if (source == NULL)
    return XDG_APP_POLICY_TALK; /* All clients can talk to the bus itself */

  if (source[0] == ':')
    return GPOINTER_TO_UINT (g_hash_table_lookup (client->unique_id_policy, source));

  return xdg_app_proxy_get_policy (client->proxy, source);
}

static void
xdg_app_proxy_client_update_unique_id_policy (XdgAppProxyClient *client,
                                              const char *unique_id,
                                              XdgAppPolicy policy)
{
  if (policy > XDG_APP_POLICY_NONE)
    {
      XdgAppPolicy old_policy;
      old_policy = GPOINTER_TO_UINT (g_hash_table_lookup (client->unique_id_policy, unique_id));
      if (policy > old_policy)
        g_hash_table_replace (client->unique_id_policy, g_strdup (unique_id), GINT_TO_POINTER (policy));
    }
}

static void
xdg_app_proxy_client_update_unique_id_policy_from_name (XdgAppProxyClient *client,
                                                        const char *unique_id,
                                                        const char *as_name)
{
  xdg_app_proxy_client_update_unique_id_policy (client,
                                                unique_id,
                                                xdg_app_proxy_get_policy (client->proxy, as_name));
}


static gboolean
client_message_generates_reply (Header *header)
{
  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      return (header->flags & G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) == 0;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
    case G_DBUS_MESSAGE_TYPE_ERROR:
    default:
      return FALSE;
    }
}

static Buffer *
message_to_buffer (GDBusMessage *message)
{
  Buffer *buffer;
  guchar *blob;
  gsize blob_size;

  blob = g_dbus_message_to_blob (message, &blob_size, G_DBUS_CAPABILITY_FLAGS_NONE, NULL);
  buffer = buffer_new (blob_size, NULL);
  memcpy (buffer->data, blob, blob_size);
  g_free (blob);

  return buffer;
}

static GDBusMessage *
get_error_for_header (Header *header, const char *error)
{
  GDBusMessage *reply;

  reply = g_dbus_message_new ();
  g_dbus_message_set_message_type (reply, G_DBUS_MESSAGE_TYPE_ERROR);
  g_dbus_message_set_flags (reply, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
  g_dbus_message_set_reply_serial (reply, header->serial);
  g_dbus_message_set_error_name (reply, error);
  g_dbus_message_set_body (reply, g_variant_new ("(s)", error));

  return reply;
}

static GDBusMessage *
get_bool_reply_for_header (Header *header, gboolean val)
{
  GDBusMessage *reply;

  reply = g_dbus_message_new ();
  g_dbus_message_set_message_type (reply, G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
  g_dbus_message_set_flags (reply, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
  g_dbus_message_set_reply_serial (reply, header->serial);
  g_dbus_message_set_body (reply, g_variant_new_boolean (val));

  return reply;
}

static Buffer *
get_ping_buffer_for_header (Header *header)
{
  Buffer *buffer;
  GDBusMessage *dummy;

  dummy = g_dbus_message_new_method_call (NULL, "/", "org.freedesktop.DBus.Peer", "Ping");
  g_dbus_message_set_serial (dummy, header->serial);
  g_dbus_message_set_flags (dummy, header->flags);

  buffer = message_to_buffer (dummy);

  g_object_unref (dummy);

  return buffer;
}

static Buffer *
get_error_for_roundtrip (XdgAppProxyClient *client, Header *header, const char *error_name)
{
  Buffer *ping_buffer = get_ping_buffer_for_header (header);
  GDBusMessage *reply;

  reply = get_error_for_header (header, error_name);
  g_hash_table_replace (client->rewrite_reply, GINT_TO_POINTER (header->serial), reply);
  return ping_buffer;
}

static Buffer *
get_bool_reply_for_roundtrip (XdgAppProxyClient *client, Header *header, gboolean val)
{
  Buffer *ping_buffer = get_ping_buffer_for_header (header);
  GDBusMessage *reply;

  reply = get_bool_reply_for_header (header, val);
  g_hash_table_replace (client->rewrite_reply, GINT_TO_POINTER (header->serial), reply);

  return ping_buffer;
}

typedef enum {
  HANDLE_PASS,
  HANDLE_DENY,
  HANDLE_HIDE,
  HANDLE_FILTER_NAME_LIST_REPLY,
  HANDLE_FILTER_HAS_OWNER_REPLY,
  HANDLE_FILTER_GET_OWNER_REPLY,
  HANDLE_VALIDATE_OWN,
  HANDLE_VALIDATE_SEE,
  HANDLE_VALIDATE_TALK,
} BusHandler;

static gboolean
is_dbus_method_call (Header *header)
{
  return
    header->type == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
    g_strcmp0 (header->destination, "org.freedesktop.DBus") == 0 &&
    g_strcmp0 (header->interface, "org.freedesktop.DBus") == 0;
}

static BusHandler
get_dbus_method_handler (XdgAppProxyClient *client, Header *header)
{
  XdgAppPolicy policy;
  const char *method;

  if (header->has_reply_serial)
    {
      ExpectedReplyType expected_reply =
	steal_expected_reply (&client->bus_side,
			      header->reply_serial);
      if (expected_reply == EXPECTED_REPLY_NONE)
	return HANDLE_DENY;

      return HANDLE_PASS;
    }

  policy = xdg_app_proxy_client_get_policy (client, header->destination);
  if (policy < XDG_APP_POLICY_SEE)
    return HANDLE_HIDE;
  if (policy < XDG_APP_POLICY_TALK)
    return HANDLE_DENY;

  if (!is_dbus_method_call (header))
    return HANDLE_PASS;

  method = header->member;
  if (method == NULL)
    return HANDLE_DENY;

  if (strcmp (method, "Hello") == 0 ||
      strcmp (method, "AddMatch") == 0 ||
      strcmp (method, "RemoveMatch") == 0 ||
      strcmp (method, "GetId") == 0)
    return HANDLE_PASS;

  if (strcmp (method, "UpdateActivationEnvironment") == 0 ||
      strcmp (method, "BecomeMonitor") == 0)
    return HANDLE_DENY;

  if (strcmp (method, "RequestName") == 0 ||
      strcmp (method, "ReleaseName") == 0 ||
      strcmp (method, "ListQueuedOwners") == 0)
    return HANDLE_VALIDATE_OWN;

  if (strcmp (method, "NameHasOwner") == 0)
    return HANDLE_FILTER_HAS_OWNER_REPLY;

  if (strcmp (method, "GetNameOwner") == 0)
    return HANDLE_FILTER_GET_OWNER_REPLY;

  if (strcmp (method, "GetConnectionUnixProcessID") == 0 ||
      strcmp (method, "GetConnectionCredentials") == 0 ||
      strcmp (method, "GetAdtAuditSessionData") == 0 ||
      strcmp (method, "GetConnectionSELinuxSecurityContext") == 0 ||
      strcmp (method, "GetConnectionUnixUser") == 0)
    return HANDLE_VALIDATE_SEE;

  if (strcmp (method, "StartServiceByName") == 0)
    return HANDLE_VALIDATE_TALK;

  if (strcmp (method, "ListNames") == 0 ||
      strcmp (method, "ListActivatableNames") == 0)
    return HANDLE_FILTER_NAME_LIST_REPLY;

  g_warning ("Unknown bus method %s\n", method);
  return HANDLE_DENY;
}

static XdgAppPolicy
policy_from_handler (BusHandler handler)
{
  switch (handler)
    {
    case HANDLE_VALIDATE_OWN:
      return XDG_APP_POLICY_OWN;
    case HANDLE_VALIDATE_TALK:
      return XDG_APP_POLICY_TALK;
    case HANDLE_VALIDATE_SEE:
      return XDG_APP_POLICY_SEE;
    default:
      return XDG_APP_POLICY_NONE;
    }
}

static char *
get_arg0_string (Buffer *buffer)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0;
  char *name = NULL;

  if (message != NULL &&
      (body = g_dbus_message_get_body (message)) != NULL &&
      (arg0 = g_variant_get_child_value (body, 0)) != NULL &&
      g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING))
    {
      name = g_variant_dup_string (arg0, NULL);
    }

  g_object_unref (message);

  return name;
}

static gboolean
validate_arg0_name (XdgAppProxyClient *client, Buffer *buffer, XdgAppPolicy required_policy, XdgAppPolicy *has_policy)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0;
  const char *name;
  XdgAppPolicy name_policy;
  gboolean res = FALSE;

  if (has_policy)
    *has_policy = XDG_APP_POLICY_NONE;

  if (message != NULL &&
      (body = g_dbus_message_get_body (message)) != NULL &&
      (arg0 = g_variant_get_child_value (body, 0)) != NULL &&
      g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING))
    {
      name = g_variant_get_string (arg0, NULL);
      name_policy = xdg_app_proxy_client_get_policy (client, name);

      if (has_policy)
        *has_policy = name_policy;

      if (name_policy >= required_policy)
        res = TRUE;
    }

  g_object_unref (message);
  return res;
}

static Buffer *
filter_names_list (XdgAppProxyClient *client, Buffer *buffer)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0, *new_names;
  const gchar **names;
  int i;
  GVariantBuilder builder;
  Buffer *filtered;

  if (message == NULL ||
      (body = g_dbus_message_get_body (message)) == NULL ||
      (arg0 = g_variant_get_child_value (body, 0)) == NULL ||
      !g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING_ARRAY))
    return NULL;

  names = g_variant_get_strv (arg0, NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (i = 0; names[i] != NULL; i++)
    {
      if (xdg_app_proxy_client_get_policy (client, names[i]) >= XDG_APP_POLICY_SEE)
        g_variant_builder_add (&builder, "s", names[i]);
    }
  g_free (names);

  new_names = g_variant_builder_end (&builder);
  g_dbus_message_set_body (message,
                           g_variant_new_tuple (&new_names, 1));

  filtered = message_to_buffer (message);
  g_object_unref (message);
  return filtered;
}

static gboolean
message_is_name_owner_changed (XdgAppProxyClient *client, Header *header)
{
  if (header->type == G_DBUS_MESSAGE_TYPE_SIGNAL &&
      g_strcmp0 (header->sender, "org.freedesktop.DBus") == 0 &&
      g_strcmp0 (header->interface, "org.freedesktop.DBus") == 0 &&
      g_strcmp0 (header->member, "NameOwnerChanged") == 0)
    return TRUE;
  return FALSE;
}

static gboolean
should_filter_name_owner_changed (XdgAppProxyClient *client, Buffer *buffer)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0, *arg1, *arg2;
  const gchar *name, *old, *new;
  gboolean filter = TRUE;

  if (message == NULL ||
      (body = g_dbus_message_get_body (message)) == NULL ||
      (arg0 = g_variant_get_child_value (body, 0)) == NULL ||
      !g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING) ||
      (arg1 = g_variant_get_child_value (body, 1)) == NULL ||
      !g_variant_is_of_type (arg1, G_VARIANT_TYPE_STRING) ||
      (arg2 = g_variant_get_child_value (body, 2)) == NULL ||
      !g_variant_is_of_type (arg2, G_VARIANT_TYPE_STRING))
    return TRUE;

  name = g_variant_get_string (arg0, NULL);
  old = g_variant_get_string (arg1, NULL);
  new = g_variant_get_string (arg2, NULL);

  if (name[0] != ':' &&
      xdg_app_proxy_client_get_policy (client, name) > XDG_APP_POLICY_SEE)
    {
      if (old[0] != 0)
        xdg_app_proxy_client_update_unique_id_policy_from_name (client, old, name);

      if (new[0] != 0)
        xdg_app_proxy_client_update_unique_id_policy_from_name (client, new, name);

      filter = FALSE;
    }

  g_object_unref (message);

  return filter;
}

static GList *
side_get_n_unix_fds (ProxySide *side, int n_fds)
{
  GList *res = NULL;

  while (side->control_messages != NULL)
    {
      GSocketControlMessage *control_message = side->control_messages->data;

      if (G_IS_UNIX_FD_MESSAGE (control_message))
        {
          GUnixFDMessage *fd_message = G_UNIX_FD_MESSAGE (control_message);
          GUnixFDList *fd_list = g_unix_fd_message_get_fd_list (fd_message);
          int len = g_unix_fd_list_get_length (fd_list);

          /* I believe that socket control messages are never merged, and
             the sender side sends only one unix-fd-list per message, so
             at this point there should always be one full fd list
             per requested number of fds */
          if (len != n_fds)
            {
              g_warning ("Not right nr of fds in socket message");
              return NULL;
            }

          side->control_messages = g_list_delete_link (side->control_messages, side->control_messages);

          return g_list_append (NULL, control_message);
        }

      g_object_unref (control_message);
      side->control_messages = g_list_delete_link (side->control_messages, side->control_messages);
    }

  return res;
}

static gboolean
update_socket_messages (ProxySide *side, Buffer *buffer, Header *header)
{
  /* We may accidentally combine multiple control messages into one
     buffer when we receive (since we can do several recvs), so we
     keep a list of all we get and then only re-attach the amount
     specified in the header to the buffer. */

  side->control_messages = g_list_concat (side->control_messages, buffer->control_messages);
  buffer->control_messages = NULL;
  if (header->unix_fds > 0)
    {
      buffer->control_messages = side_get_n_unix_fds (side, header->unix_fds);
      if (buffer->control_messages == NULL)
	{
	  g_warning ("Not enough fds for message");
	  side_closed (side);
	  buffer_free (buffer);
	  return FALSE;
	}
    }
  return TRUE;
}

static void
got_buffer_from_client (XdgAppProxyClient *client, ProxySide *side, Buffer *buffer)
{
  if (client->authenticated && client->proxy->filter)
    {
      Header header;
      BusHandler handler;
      ExpectedReplyType expecting_reply = EXPECTED_REPLY_NONE;

      /* Filter and rewrite outgoing messages as needed */

      if (!parse_header (buffer, &header))
        {
          g_warning ("Invalid message header format");
          side_closed (side);
          buffer_free (buffer);
          return;
        }

      if (!update_socket_messages (side, buffer, &header))
	return;

      /* Make sure the client is not playing games with the serials, as that
         could confuse us. */
      if (header.serial <= client->last_serial)
        {
          g_warning ("Invalid client serial");
          side_closed (side);
          buffer_free (buffer);
          return;
        }
      client->last_serial = header.serial;

      if (client->proxy->log_messages)
        print_outgoing_header (&header);

      /* Keep track of the initial Hello request so that we can read
	 the reply which has our assigned unique id */
      if (is_dbus_method_call (&header) &&
          g_strcmp0 (header.member, "Hello") == 0)
	expecting_reply = EXPECTED_REPLY_HELLO;

      handler = get_dbus_method_handler (client, &header);

      switch (handler)
        {
        case HANDLE_FILTER_HAS_OWNER_REPLY:
        case HANDLE_FILTER_GET_OWNER_REPLY:
          if (!validate_arg0_name (client, buffer, XDG_APP_POLICY_SEE, NULL))
            {
	      g_clear_pointer (&buffer, buffer_free);
              if (handler == HANDLE_FILTER_GET_OWNER_REPLY)
		buffer = get_error_for_roundtrip (client, &header,
						  "org.freedesktop.DBus.Error.NameHasNoOwner");
              else
                buffer = get_bool_reply_for_roundtrip (client, &header, FALSE);

	      expecting_reply = EXPECTED_REPLY_REWRITE;
              break;
            }

          if (handler == HANDLE_FILTER_GET_OWNER_REPLY)
            {
              char *name = get_arg0_string (buffer);
	      expecting_reply = EXPECTED_REPLY_GET_NAME_OWNER;
              g_hash_table_replace (client->get_owner_reply, GINT_TO_POINTER (header.serial), name);
            }

          goto handle_pass;

        case HANDLE_VALIDATE_OWN:
        case HANDLE_VALIDATE_SEE:
        case HANDLE_VALIDATE_TALK:
          {
            XdgAppPolicy name_policy;
            if (validate_arg0_name (client, buffer, policy_from_handler (handler), &name_policy))
              goto handle_pass;

            if (name_policy < (int)HANDLE_VALIDATE_SEE)
              goto handle_hide;
            else
              goto handle_deny;
          }

        case HANDLE_FILTER_NAME_LIST_REPLY:
	  expecting_reply = EXPECTED_REPLY_LIST_NAMES;
          goto handle_pass;

        case HANDLE_PASS:
        handle_pass:
          if (client_message_generates_reply (&header))
	    {
	      if (header.destination != NULL &&
		  *header.destination != ':')
		{
		  /* Sending to a well known name, track return unique id */
		  g_hash_table_replace (client->named_reply, GINT_TO_POINTER (header.serial), g_strdup (header.destination));
		}

	      if (expecting_reply == EXPECTED_REPLY_NONE)
		expecting_reply = EXPECTED_REPLY_NORMAL;
	    }

          break;

        case HANDLE_HIDE:
        handle_hide:
	  g_clear_pointer (&buffer, buffer_free);

          if (client_message_generates_reply (&header))
            {
	      const char *error;

              if (client->proxy->log_messages)
                g_print ("*HIDDEN* (ping)\n");

              if ((header.destination != NULL && header.destination[0] == ':') ||
                  (header.flags & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START) != 0)
                error = "org.freedesktop.DBus.Error.NameHasNoOwner";
              else
		error = "org.freedesktop.DBus.Error.ServiceUnknown";

              buffer = get_error_for_roundtrip (client, &header, error);
	      expecting_reply = EXPECTED_REPLY_REWRITE;
            }
          else
            {
              if (client->proxy->log_messages)
                g_print ("*HIDDEN*\n");
            }
          break;

        default:
        case HANDLE_DENY:
        handle_deny:
	  g_clear_pointer (&buffer, buffer_free);

          if (client_message_generates_reply (&header))
            {
              if (client->proxy->log_messages)
                g_print ("*DENIED* (ping)\n");

              buffer = get_error_for_roundtrip (client, &header,
						"org.freedesktop.DBus.Error.AccessDenied");
	      expecting_reply = EXPECTED_REPLY_REWRITE;
            }
          else
            {
              if (client->proxy->log_messages)
                g_print ("*DENIED*\n");
            }
          break;
        }

      if (buffer != NULL && expecting_reply != EXPECTED_REPLY_NONE)
	queue_expected_reply (side, header.serial, expecting_reply);
    }

  if (buffer && g_strstr_len ((char *)buffer->data, buffer->size, "BEGIN\r\n") != NULL)
    client->authenticated = TRUE;

  if (buffer)
    queue_outgoing_buffer (&client->bus_side, buffer);
}

static void
got_buffer_from_bus (XdgAppProxyClient *client, ProxySide *side, Buffer *buffer)
{
  if (client->authenticated && client->proxy->filter)
    {
      Header header;
      GDBusMessage *rewritten;
      char *name;
      XdgAppPolicy policy;
      ExpectedReplyType expected_reply;

      /* Filter and rewrite incomming messages as needed */

      if (!parse_header (buffer, &header))
        {
          g_warning ("Invalid message header format");
	  buffer_free (buffer);
          side_closed (side);
          return;
        }

      if (!update_socket_messages (side, buffer, &header))
	return;

      if (client->proxy->log_messages)
        print_incoming_header (&header);

      if (header.has_reply_serial)
	{
	  expected_reply = steal_expected_reply (get_other_side (side), header.reply_serial);

	  /* We only allow replies we expect */
	  if (expected_reply == EXPECTED_REPLY_NONE)
	    {
	      if (client->proxy->log_messages)
		g_print ("*Unexpected reply*\n");
	      buffer_free (buffer);
	      return;
	    }

	  /* If we sent a message to a named target and get a reply, then we allow further
	     communications with that unique id */
	  if ((name = g_hash_table_lookup (client->named_reply, GINT_TO_POINTER (header.reply_serial))) != NULL)
	    {
	      if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN &&
		  header.sender != NULL &&
		  *header.sender == ':')
		{
		  xdg_app_proxy_client_update_unique_id_policy_from_name (client, header.sender, name);

		  g_hash_table_remove (client->named_reply, GINT_TO_POINTER (header.reply_serial));
		}
	    }


	  switch (expected_reply)
	    {
	    case EXPECTED_REPLY_HELLO:
	      /* When we get the initial reply to Hello, allow all
		 further communications to our own unique id. */
	      {
		char *my_id = get_arg0_string (buffer);
		xdg_app_proxy_client_update_unique_id_policy (client, my_id, XDG_APP_POLICY_TALK);
		break;
	      }

	    case EXPECTED_REPLY_REWRITE:
	      /* Replace a roundtrip ping with the rewritten message */

	      rewritten = g_hash_table_lookup (client->rewrite_reply,
					       GINT_TO_POINTER (header.reply_serial));

	      if (client->proxy->log_messages)
		g_print ("*REWRITTEN*\n");

	      g_dbus_message_set_serial (rewritten, header.serial);
	      g_clear_pointer (&buffer, buffer_free);
	      buffer = message_to_buffer (rewritten);

	      g_hash_table_remove (client->rewrite_reply,
				   GINT_TO_POINTER (header.reply_serial));
	      break;

	    case EXPECTED_REPLY_GET_NAME_OWNER:
	      /* This is a reply from the bus to an allowed GetNameOwner
		 request, update the policy for this unique name based on
		 the policy */
	      {
		char *requested_name = g_hash_table_lookup (client->get_owner_reply, GINT_TO_POINTER (header.reply_serial));
		char *owner = get_arg0_string (buffer);

		xdg_app_proxy_client_update_unique_id_policy_from_name (client, owner, requested_name);

		g_hash_table_remove (client->get_owner_reply, GINT_TO_POINTER (header.reply_serial));
		g_free (owner);
		break;
	      }

	    case EXPECTED_REPLY_LIST_NAMES:
	      /* This is a reply from the bus to a ListNames request, filter
		 it according to the policy */
	      {
		Buffer *filtered_buffer;

		filtered_buffer = filter_names_list (client, buffer);
		g_clear_pointer (&buffer, buffer_free);
		buffer = filtered_buffer;
		break;
	      }

	    case EXPECTED_REPLY_NORMAL:
	      break;

	    default:
	      g_warning ("Unexpected expected reply type %d\n", expected_reply);
	    }
	}
      else /* Not reply */
	{
	  /* We filter all NameOwnerChanged signal according to the policy */
	  if (message_is_name_owner_changed (client, &header))
	    {
	      if (should_filter_name_owner_changed (client, buffer))
		g_clear_pointer (&buffer, buffer_free);
	    }
	}

      /* All incoming broadcast signals are filtered according to policy */
      if (header.type == G_DBUS_MESSAGE_TYPE_SIGNAL && header.destination == NULL)
	{
	  policy = xdg_app_proxy_client_get_policy (client, header.sender);
	  if (policy < XDG_APP_POLICY_TALK)
	    {
	      if (client->proxy->log_messages)
		g_print ("*FILTERED IN*\n");
	      g_clear_pointer (&buffer, buffer_free);
	    }
	}

      if (buffer && client_message_generates_reply (&header))
	queue_expected_reply (side, header.serial, EXPECTED_REPLY_NORMAL);
    }

  if (buffer)
    queue_outgoing_buffer (&client->client_side, buffer);
}

static void
got_buffer_from_side (ProxySide *side, Buffer *buffer)
{
  XdgAppProxyClient *client = side->client;

  if (side == &client->client_side)
    got_buffer_from_client (client, side, buffer);
  else
    got_buffer_from_bus (client, side, buffer);
}

static gboolean
side_in_cb (GSocket *socket, GIOCondition condition, gpointer user_data)
{
  ProxySide *side = user_data;
  XdgAppProxyClient *client = side->client;
  GError *error = NULL;
  Buffer *buffer;
  gboolean retval = G_SOURCE_CONTINUE;

  g_object_ref (client);

  if (!side->got_first_byte)
    buffer = buffer_new (1, NULL);
  else if (!client->authenticated)
    buffer = buffer_new (64, NULL);
  else
    buffer = side->current_read_buffer;

  if (buffer_read (side, buffer, socket) || !client->authenticated)
    {
      if (!side->got_first_byte)
        {
          if (buffer->pos > 0)
            {
              buffer->send_credentials = TRUE;
              buffer->size = buffer->pos;
              got_buffer_from_side (side, buffer);
              side->got_first_byte = TRUE;
            }
          else
            buffer_free (buffer);
        }
      else if (!client->authenticated)
        {
          if (buffer->pos > 0)
            {
              buffer->size = buffer->pos;
              got_buffer_from_side (side, buffer);
            }
          else
            buffer_free (buffer);
        }
      else if (buffer == &side->header_buffer)
        {
          gssize required;
          required = g_dbus_message_bytes_needed (buffer->data, buffer->size, &error);
          if (required < 0)
            {
              g_warning ("Invalid message header read");
              side_closed (side);
            }
          else
            side->current_read_buffer = buffer_new (required, buffer);
        }
      else
        {
          got_buffer_from_side (side, buffer);
          side->header_buffer.pos = 0;
          side->current_read_buffer = &side->header_buffer;
        }
    }

  if (side->closed)
    {
      side->in_source = NULL;
      retval = G_SOURCE_REMOVE;
    }

  g_object_unref (client);

  return retval;
}

static void
start_reading (ProxySide *side)
{
  GSocket *socket;

  socket = g_socket_connection_get_socket (side->connection);
  side->in_source = g_socket_create_source (socket, G_IO_IN, NULL);
  g_source_set_callback (side->in_source, (GSourceFunc)side_in_cb, side, NULL);
  g_source_attach (side->in_source, NULL);
  g_source_unref (side->in_source);
}

static void
client_connected_to_dbus (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  XdgAppProxyClient *client = user_data;
  GError *error = NULL;
  GIOStream *stream;

  stream = g_dbus_address_get_stream_finish (res, NULL, &error);
  if (stream == NULL)
    {
      g_warning ("Failed to connect to bus: %s\n", error->message);
      g_object_unref (client);
      return;
    }

  client->bus_side.connection = G_SOCKET_CONNECTION (stream);

  start_reading (&client->client_side);
  start_reading (&client->bus_side);
}

static gboolean
xdg_app_proxy_incoming (GSocketService    *service,
                        GSocketConnection *connection,
                        GObject           *source_object)
{
  XdgAppProxy *proxy = XDG_APP_PROXY (service);
  XdgAppProxyClient *client;

  client = xdg_app_proxy_client_new (proxy, connection);

  g_dbus_address_get_stream (proxy->dbus_address,
                             NULL,
                             client_connected_to_dbus,
                            client);
  return TRUE;
}

static void
xdg_app_proxy_init (XdgAppProxy *proxy)
{
  proxy->policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  proxy->wildcard_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  xdg_app_proxy_add_policy (proxy, "org.freedesktop.DBus", XDG_APP_POLICY_TALK);
}

static void
xdg_app_proxy_class_init (XdgAppProxyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GSocketServiceClass *socket_service_class = G_SOCKET_SERVICE_CLASS (klass);

  object_class->get_property = xdg_app_proxy_get_property;
  object_class->set_property = xdg_app_proxy_set_property;
  object_class->finalize = xdg_app_proxy_finalize;

  socket_service_class->incoming = xdg_app_proxy_incoming;

  g_object_class_install_property (object_class,
                                   PROP_DBUS_ADDRESS,
                                   g_param_spec_string ("dbus-address",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_SOCKET_PATH,
                                   g_param_spec_string ("socket-path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

}

XdgAppProxy *
xdg_app_proxy_new (const char *dbus_address,
                   const char *socket_path)
{
  XdgAppProxy *proxy;

  proxy = g_object_new (XDG_APP_TYPE_PROXY, "dbus-address", dbus_address, "socket-path", socket_path, NULL);
  return proxy;
}

gboolean
xdg_app_proxy_start (XdgAppProxy *proxy, GError **error)
{
  GSocketAddress *address;
  gboolean res;

  unlink (proxy->socket_path);

  address = g_unix_socket_address_new (proxy->socket_path);

  error = NULL;
  res = g_socket_listener_add_address (G_SOCKET_LISTENER (proxy),
                                       address,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, /* source_object */
                                       NULL, /* effective_address */
                                       error);
  g_object_unref (address);

  if (!res)
    return FALSE;


  g_socket_service_start (G_SOCKET_SERVICE (proxy));
  return TRUE;
}
