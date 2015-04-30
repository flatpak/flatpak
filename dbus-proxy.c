#include <unistd.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gunixconnection.h>

typedef struct XdgAppProxy XdgAppProxy;
typedef struct XdgAppProxyClient XdgAppProxyClient;

typedef struct {
  gsize size;
  gsize pos;
  gboolean send_credentials;
  GList *control_messages;

  guchar data[16];
  /* data continues here */
} Buffer;

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
};

typedef struct {
  GObjectClass parent_class;
} XdgAppProxyClientClass;

struct XdgAppProxy {
  GSocketService parent;

  GList *clients;
  char *socket_path;
  char *dbus_address;
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

static void
xdg_app_proxy_finalize (GObject *object)
{
  XdgAppProxy *proxy = XDG_APP_PROXY (object);

  g_clear_pointer (&proxy->dbus_address, g_free);
  g_assert (proxy->clients == NULL);

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

static void
got_buffer_from_client (XdgAppProxyClient *client, ProxySide *side, Buffer *buffer)
{
  queue_outgoing_buffer (&client->bus_side, buffer);

  if (!client->authenticated && g_strstr_len ((char *)buffer->data, buffer->size, "BEGIN\r\n") != NULL)
    client->authenticated = TRUE;
}

static void
got_buffer_from_bus (XdgAppProxyClient *client, ProxySide *side, Buffer *buffer)
{
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
