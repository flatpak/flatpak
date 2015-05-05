#include <unistd.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gunixconnection.h>

typedef enum {
  XDG_APP_POLICY_NONE,
  XDG_APP_POLICY_SEE,
  XDG_APP_POLICY_TALK,
  XDG_APP_POLICY_OWN
} XdgAppPolicy;

/**
 * Mode of operation
 *
 * The proxy listens to a unix domain socket, and for each new connection it opens up
 * a new connection to the session bus and forwards data between the two.
 * During the authentication phase all data is sent, and in the first 1 byte zero we
 * also send the proxy credentials to the bus. This means the bus will know the pid
 * of the proxy as the pid of the app, unfortunately.
 *
 * After authentication we parse incoming dbus messages and do some minor validation
 * of the message headers. We then apply the policy to the header which may cause
 * us to drop or rewrite messages.
 *
 * Each well known name on the bus can have a policy of NONE, SEE, TALK, and OWN.
 * NONE means you can't see this name, nor send or receive messages from it.
 * SEE means you get told about the existance of this name and can get info about it.
 * TALK means you can also send and receive messages to the owner of the name
 * OWN means you can also aquire ownership of the name
 *
 * Policy is specified on the well known name, but clients are also allowed to
 * send directly to the unique id. To handle this we track all outgoing messages
 * sent to a well known name, and if we get a reply to it we record unique id that
 * replied to the message and apply the policy of the name to that unique id.
 * We also parse all NameOwnerChanged signals from the bus and update the policy
 * similarly.
 *
 * This means that each unique id destination effectively gets the
 * maximum policy of each of the names it has at one point owned. The fact
 * that dropping the name does not lower the policy is unfortunate, but it
 * is essentially impossible to avoid this (at least for some time) due to races.
 *
 */

typedef struct XdgAppProxy XdgAppProxy;
typedef struct XdgAppProxyClient XdgAppProxyClient;

XdgAppPolicy xdg_app_proxy_get_policy (XdgAppProxy *proxy, const char *name);

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
} ProxySide;

struct XdgAppProxyClient {
  GObject parent;

  XdgAppProxy *proxy;

  gboolean authenticated;

  ProxySide client_side;
  ProxySide bus_side;

  /* Filtering data: */
  guint32 hello_serial;
  guint32 last_serial;
  GHashTable *rewrite_reply;
  GHashTable *named_reply;
  GHashTable *get_owner_reply;
  GHashTable *list_names_reply;
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

  GHashTable *policy;
};

typedef struct {
  GSocketServiceClass parent_class;
} XdgAppProxyClass;


enum {
  PROP_0,

  PROP_DBUS_ADDRESS
};

#define XDG_APP_TYPE_PROXY xdg_app_proxy_get_type()
#define XDG_APP_PROXY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_PROXY, XdgAppProxy))
#define XDG_APP_IS_PROXY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_PROXY))

GType xdg_app_proxy_get_type (void);

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

  if (side->in_source)
    g_source_destroy (side->in_source);
  if (side->out_source)
    g_source_destroy (side->out_source);
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
  g_hash_table_destroy (client->list_names_reply);
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
}

static void
xdg_app_proxy_client_init (XdgAppProxyClient *client)
{
  init_side (client, &client->client_side);
  init_side (client, &client->bus_side);

  client->rewrite_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
  client->named_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  client->get_owner_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  client->list_names_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
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
  gpointer res;
  res = g_hash_table_lookup (proxy->policy, name);
  return GPOINTER_TO_INT (res);
}

void
xdg_app_proxy_set_filter (XdgAppProxy *proxy,
                          gboolean filter)
{
  proxy->filter = filter;
}

void
xdg_app_proxy_set_policy (XdgAppProxy *proxy,
                          const char *name,
                          XdgAppPolicy policy)
{
  g_hash_table_replace (proxy->policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
xdg_app_proxy_finalize (GObject *object)
{
  XdgAppProxy *proxy = XDG_APP_PROXY (object);

  g_clear_pointer (&proxy->dbus_address, g_free);
  g_assert (proxy->clients == NULL);

  g_hash_table_destroy (proxy->policy);

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
client_message_has_reply (Header *header)
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

static void
queue_error_roundtrip (XdgAppProxyClient *client, Header *header, const char *error_name)
{
  Buffer *ping_buffer = get_ping_buffer_for_header (header);
  GDBusMessage *reply;

  queue_outgoing_buffer (&client->bus_side, ping_buffer);

  reply = get_error_for_header (header, error_name);
  g_hash_table_replace (client->rewrite_reply, GINT_TO_POINTER (header->serial), reply);
}


static void
queue_access_denied_roundtrip (XdgAppProxyClient *client, Header *header)
{
  queue_error_roundtrip (client, header, "org.freedesktop.DBus.Error.AccessDenied");
}

static void
queue_name_has_no_owner_roundtrip (XdgAppProxyClient *client, Header *header)
{
  queue_error_roundtrip (client, header, "org.freedesktop.DBus.Error.NameHasNoOwner");
}

static void
queue_service_unknown_roundtrip (XdgAppProxyClient *client, Header *header)
{
  queue_error_roundtrip (client, header, "org.freedesktop.DBus.Error.ServiceUnknown");
}

static void
queue_bool_reply_roundtrip (XdgAppProxyClient *client, Header *header, gboolean val)
{
  Buffer *ping_buffer = get_ping_buffer_for_header (header);
  GDBusMessage *reply;

  queue_outgoing_buffer (&client->bus_side, ping_buffer);

  reply = get_bool_reply_for_header (header, val);
  g_hash_table_replace (client->rewrite_reply, GINT_TO_POINTER (header->serial), reply);
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
filter_name_owner_changed (XdgAppProxyClient *client, Buffer *buffer)
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

static void
got_buffer_from_client (XdgAppProxyClient *client, ProxySide *side, Buffer *buffer)
{
  if (!client->authenticated)
    {
      queue_outgoing_buffer (&client->bus_side, buffer);

      if (g_strstr_len ((char *)buffer->data, buffer->size, "BEGIN\r\n") != NULL)
        client->authenticated = TRUE;
    }
  else if (!client->proxy->filter)
    {
      queue_outgoing_buffer (&client->bus_side, buffer);
    }
  else
    {
      Header header;
      BusHandler handler;

      /* Filtering */

      if (!parse_header (buffer, &header))
        {
          g_warning ("Invalid message header format");
          side_closed (side);
          buffer_free (buffer);
          return;
        }

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

      if (client->hello_serial == 0 && is_dbus_method_call (&header) &&
          g_strcmp0 (header.member, "Hello") == 0)
        client->hello_serial = header.serial;

      handler = get_dbus_method_handler (client, &header);

      switch (handler)
        {
        case HANDLE_FILTER_HAS_OWNER_REPLY:
        case HANDLE_FILTER_GET_OWNER_REPLY:
          if (!validate_arg0_name (client, buffer, XDG_APP_POLICY_SEE, NULL))
            {
              buffer_free (buffer);
              if (handler == HANDLE_FILTER_GET_OWNER_REPLY)
                queue_name_has_no_owner_roundtrip (client, &header);
              else
                queue_bool_reply_roundtrip (client, &header, FALSE);
              break;
            }

          if (handler == HANDLE_FILTER_GET_OWNER_REPLY)
            {
              char *name = get_arg0_string (buffer);
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
          g_hash_table_replace (client->list_names_reply, GINT_TO_POINTER (header.serial), GINT_TO_POINTER (1));
          goto handle_pass;

        case HANDLE_PASS:
        handle_pass:
          if (client_message_has_reply (&header) &&
              header.destination != NULL &&
              *header.destination != ':')
            {
              /* Sending to a well known name, track return unique id */
              g_hash_table_replace (client->named_reply, GINT_TO_POINTER (header.serial), g_strdup (header.destination));
            }

          queue_outgoing_buffer (&client->bus_side, buffer);
          break;

        case HANDLE_HIDE:
        handle_hide:
          buffer_free (buffer);

          if (client_message_has_reply (&header))
            {
              if (client->proxy->log_messages)
                g_print ("*HIDDEN* (ping)\n");

              if ((header.destination != NULL && header.destination[0] == ':') ||
                  (header.flags & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START) != 0)
                queue_name_has_no_owner_roundtrip (client, &header);
              else
                queue_service_unknown_roundtrip (client, &header);
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
          buffer_free (buffer);

          if (client_message_has_reply (&header))
            {
              if (client->proxy->log_messages)
                g_print ("*DENIED* (ping)\n");

              queue_access_denied_roundtrip (client, &header);
            }
          else
            {
              if (client->proxy->log_messages)
                g_print ("*DENIED*\n");
            }
          break;
        }
    }
}

static void
got_buffer_from_bus (XdgAppProxyClient *client, ProxySide *side, Buffer *buffer)
{
  if (!client->authenticated)
    {
      queue_outgoing_buffer (&client->client_side, buffer);
    }
  else if (!client->proxy->filter)
    {
      queue_outgoing_buffer (&client->client_side, buffer);
    }
  else
    {
      Header header;
      GDBusMessage *rewritten;

      /* Filtering */

      if (!parse_header (buffer, &header))
        {
          g_warning ("Invalid message header format");
          side_closed (side);
          return;
        }

      print_incoming_header (&header);

      if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN &&
          g_strcmp0 (header.sender, "org.freedesktop.DBus") == 0 &&
          header.has_reply_serial &&
          client->hello_serial != 0 &&
          header.reply_serial == client->hello_serial)
        {
          char *my_id = get_arg0_string (buffer);
          xdg_app_proxy_client_update_unique_id_policy (client, my_id, XDG_APP_POLICY_TALK);
        }

      if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN &&
          header.sender == NULL &&
          header.has_reply_serial &&
          (rewritten = g_hash_table_lookup (client->rewrite_reply, GINT_TO_POINTER (header.reply_serial))) != NULL)
        {
          Buffer *rewritten_buffer;
          g_hash_table_steal (client->rewrite_reply, GINT_TO_POINTER (header.reply_serial));

          if (client->proxy->log_messages)
            g_print ("*REWRITTEN*\n");

          g_dbus_message_set_serial (rewritten, header.serial);
          rewritten_buffer = message_to_buffer (rewritten);
          g_object_unref (rewritten);
          buffer_free (buffer);
          queue_outgoing_buffer (&client->client_side, rewritten_buffer);
        }
      else
        {
          char *name;
          XdgAppPolicy policy;

          if (header.has_reply_serial &&
              (name = g_hash_table_lookup (client->named_reply, GINT_TO_POINTER (header.reply_serial))) != NULL)
            {
              if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN &&
                  header.sender != NULL &&
                  *header.sender == ':')
                {
                  xdg_app_proxy_client_update_unique_id_policy_from_name (client, header.sender, name);

                  g_hash_table_remove (client->named_reply, GINT_TO_POINTER (header.reply_serial));
                }
            }

          if (g_strcmp0 (header.sender, "org.freedesktop.DBus") == 0 &&
              header.has_reply_serial &&
              (name = g_hash_table_lookup (client->get_owner_reply, GINT_TO_POINTER (header.reply_serial))) != NULL)
            {
              if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
                {
                  char *owner = get_arg0_string (buffer);
                  xdg_app_proxy_client_update_unique_id_policy_from_name (client, owner, name);
                  g_free (owner);
                }

              g_hash_table_remove (client->get_owner_reply, GINT_TO_POINTER (header.reply_serial));
            }

          policy = xdg_app_proxy_client_get_policy (client, header.sender);

          if (policy >= XDG_APP_POLICY_TALK)
            {
              /* Filter ListNames replies */
              if (header.has_reply_serial &&
                  g_hash_table_lookup (client->list_names_reply, GINT_TO_POINTER (header.reply_serial)))
                {
                  Buffer *filtered;
                  g_hash_table_remove (client->list_names_reply, GINT_TO_POINTER (header.reply_serial));

                  filtered = filter_names_list (client, buffer);
                  buffer_free (buffer);
                  buffer = filtered;
                }
              else if (message_is_name_owner_changed (client, &header))
                {
                  if (filter_name_owner_changed (client, buffer))
                    {
                      buffer_free (buffer);
                      buffer = NULL;
                    }
                }

              if (buffer)
                queue_outgoing_buffer (&client->client_side, buffer);
            }
          else
            {
              if (client->proxy->log_messages)
                g_print ("*FILTERED IN*\n");
              buffer_free (buffer);
            }
        }
    }
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
  xdg_app_proxy_set_policy (proxy, "org.freedesktop.DBus", XDG_APP_POLICY_TALK);
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

}

XdgAppProxy *
xdg_app_proxy_new (const char *dbus_address)
{
  XdgAppProxy *proxy;

  proxy = g_object_new (XDG_APP_TYPE_PROXY, "dbus-address", dbus_address, NULL);
  return proxy;
}

gboolean
xdg_app_proxy_start (XdgAppProxy *proxy, GError **error)
{
  GSocketAddress *address;
  gboolean res;

  proxy->socket_path = g_build_filename (g_get_user_runtime_dir (), "gdbus-proxy", NULL);
  unlink (proxy->socket_path);

  g_print ("listening on DBUS_SESSION_BUS_ADDRESS=\"unix:path=%s\"\n", proxy->socket_path);
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

int
main (int argc, char *argv[])
{
  GMainLoop *service_loop;
  XdgAppProxy *proxy;
  GError *error = NULL;

  proxy = xdg_app_proxy_new (g_getenv ("DBUS_SESSION_BUS_ADDRESS"));

  xdg_app_proxy_start (proxy, &error);
  g_assert_no_error (error);

  service_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (service_loop);

  g_main_loop_unref (service_loop);

  return 0;
}
