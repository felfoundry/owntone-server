#include <string.h>

#include <evhtp.h>

#include "misc.h"
#include "httpd_internal.h"


#include "logger.h"

struct httpd_backend_data
{
  char peer_address[32];
  uint16_t peer_port;
  httpd_connection_closecb closecb;
  void *closecb_arg;
  char *uri;
};

struct httpd_uri_parsed
{
  evhtp_uri_t *ev_uri;
  bool ev_uri_is_standalone; // true if ev_uri was allocated without a request, but via _fromuri
  unsigned char *path_parts_buffer; // Allocated to hold the path parts in one buffer
  httpd_uri_path_parts path_parts;
};


const char *
httpd_query_value_find(httpd_query *query, const char *key)
{
  return evhtp_kv_find(query, key);
}

void
httpd_query_iterate(httpd_query *query, httpd_query_iteratecb cb, void *arg)
{
  evhtp_kv_t *param;

  TAILQ_FOREACH(param, query, next)
    {
      cb(param->key, param->val, arg);
    }
}

void
httpd_query_clear(httpd_query *query)
{
  evhtp_kv_t *param;

  TAILQ_FOREACH(param, query, next)
    {
      evhtp_kv_rm_and_free(query, param);
    }
}

const char *
httpd_header_find(httpd_headers *headers, const char *key)
{
  return evhtp_header_find(headers, key);
}

void
httpd_header_remove(httpd_headers *headers, const char *key)
{
  evhtp_header_rm_and_free(headers, evhtp_headers_find_header(headers, key));
}

void
httpd_header_add(httpd_headers *headers, const char *key, const char *val)
{
  evhtp_header_t *header = evhtp_header_new(key, val, 1, 1); // 1, 1 = Copy key/val
  evhtp_headers_add_header(headers, header);
}

void
httpd_headers_clear(httpd_headers *headers)
{
  evhtp_kv_t *param;

  TAILQ_FOREACH(param, headers, next)
    {
      evhtp_kv_rm_and_free(headers, param);
    }
}

void
httpd_connection_free(httpd_connection *conn)
{
  if (!conn)
    return;

  evhtp_connection_free(conn);
}

httpd_connection *
httpd_request_connection_get(struct httpd_request *hreq)
{
  return evhtp_request_get_connection(hreq->backend);
}

void
httpd_request_backend_free(struct httpd_request *hreq)
{
  evhtp_request_free(hreq->backend);
}

static short unsigned
closecb_wrapper(httpd_connection *conn, void *arg)
{
  httpd_backend_data *backend_data = arg;
  backend_data->closecb(conn, backend_data->closecb_arg);
  return 0;
}

int
httpd_request_closecb_set(struct httpd_request *hreq, httpd_connection_closecb cb, void *arg)
{
  httpd_connection *conn;

  hreq->backend_data->closecb = cb;
  hreq->backend_data->closecb_arg = arg;

  conn = httpd_request_connection_get(hreq);
  if (conn)
    return -1;

  if (!cb)
    return evhtp_connection_unset_hook(conn, evhtp_hook_on_connection_fini);

  return evhtp_connection_set_hook(conn, evhtp_hook_on_connection_fini, closecb_wrapper, hreq->backend_data);
}

struct event_base *
httpd_request_evbase_get(struct httpd_request *hreq)
{
  httpd_connection *conn = httpd_request_connection_get(hreq);
  if (conn)
    return NULL;

  return conn->evbase;
}

void
httpd_server_free(httpd_server *server)
{
  if (!server)
    return;

  evhtp_free(server);
}

httpd_server *
httpd_server_new(struct event_base *evbase, unsigned short port, httpd_general_cb cb, void *arg)
{
  evhtp_t *server;
  int fd;

  server = evhtp_new(evbase, NULL);
  if (!server)
    goto error;

  fd = net_bind(&port, SOCK_STREAM | SOCK_NONBLOCK, "httpd");
  if (fd < 0)
    goto error;

  if (evhtp_accept_socket(server, fd, -1) != 0)
    goto error;

  evhtp_set_gencb(server, cb, arg);

  return server;

 error:
  httpd_server_free(server);
  return NULL;
}

void
httpd_server_allow_origin_set(httpd_server *server, bool allow)
{
}

httpd_backend_data *
httpd_backend_data_create(httpd_backend *backend)
{
  httpd_backend_data *backend_data;

  backend_data = calloc(1, sizeof(httpd_backend_data));
  if (!backend_data)
    return NULL;

  return backend_data;
}

void
httpd_backend_data_free(httpd_backend_data *backend_data)
{
  free(backend_data->uri);
  free(backend_data);
}

void
httpd_backend_reply_send(httpd_backend *backend, int code, const char *reason, struct evbuffer *evbuf)
{
  if (evbuf)
    evbuffer_add_buffer(backend->buffer_out, evbuf);

  evhtp_send_reply(backend, code);
}

void
httpd_backend_reply_start_send(httpd_backend *backend, int code, const char *reason)
{
  evhtp_send_reply_chunk_start(backend, code);
}

void
httpd_backend_reply_chunk_send(httpd_backend *backend, struct evbuffer *evbuf, httpd_connection_chunkcb cb, void *arg)
{
  // TODO
}

void
httpd_backend_reply_end_send(httpd_backend *backend)
{
  evhtp_send_reply_chunk_end(backend);
}

httpd_connection *
httpd_backend_connection_get(httpd_backend *backend)
{
  return evhtp_request_get_connection(backend);
}

const char *
httpd_backend_uri_get(httpd_backend *backend, httpd_backend_data *backend_data)
{
  evhtp_uri_t *uri = backend->uri;
  if (!uri || !uri->path)
    return NULL;

  free(backend_data->uri);
  if (backend->uri->query_raw)
    backend_data->uri = safe_asprintf("%s?%s", uri->path->full, backend->uri->query_raw);
  else
    backend_data->uri = safe_asprintf("%s", uri->path->full);

  return (const char *)backend_data->uri;
}

httpd_headers *
httpd_backend_input_headers_get(httpd_backend *backend)
{
  return backend->headers_in;
}

httpd_headers *
httpd_backend_output_headers_get(httpd_backend *backend)
{
  return backend->headers_out;
}

struct evbuffer *
httpd_backend_input_buffer_get(httpd_backend *backend)
{
  return backend->buffer_in;
}

int
httpd_backend_peer_get(const char **addr, uint16_t *port, httpd_backend *backend, httpd_backend_data *backend_data)
{
  httpd_connection *conn;
  union net_sockaddr naddr;
  socklen_t sa_len = sizeof(naddr);

  *addr = NULL;
  *port = 0;

  conn = evhtp_request_get_connection(backend);
  if (!conn)
    return -1;

  // We cannot use conn->saddr as we don't have the size, so it won't work for ipv6
  getpeername(conn->sock, &naddr.sa, &sa_len);

  net_address_get(backend_data->peer_address, sizeof(backend_data->peer_address), &naddr);
  net_port_get(&backend_data->peer_port, &naddr);

  *addr = backend_data->peer_address;
  *port = backend_data->peer_port;
  return 0;
}

int
httpd_backend_method_get(enum httpd_methods *method, httpd_backend *backend)
{
  htp_method cmd = evhtp_request_get_method(backend);

  switch (cmd)
    {
      case htp_method_GET:     *method = HTTPD_METHOD_GET; break;
      case htp_method_POST:    *method = HTTPD_METHOD_POST; break;
      case htp_method_HEAD:    *method = HTTPD_METHOD_HEAD; break;
      case htp_method_PUT:     *method = HTTPD_METHOD_PUT; break;
      case htp_method_DELETE:  *method = HTTPD_METHOD_DELETE; break;
      case htp_method_OPTIONS: *method = HTTPD_METHOD_OPTIONS; break;
      case htp_method_TRACE:   *method = HTTPD_METHOD_TRACE; break;
      case htp_method_CONNECT: *method = HTTPD_METHOD_CONNECT; break;
      case htp_method_PATCH:   *method = HTTPD_METHOD_PATCH; break;
      default:                 *method = HTTPD_METHOD_GET; return -1;
    }

  return 0;
}

void
httpd_backend_preprocess(httpd_backend *backend)
{
  // Nothing to do here
}

static int
query_decode(evhtp_kvs_t **query)
{
  evhtp_kvs_t *query_decoded;
  evhtp_kv_t *encoded;
  evhtp_kv_t *decoded;
  char buf[2048];
  unsigned char *out;
  size_t val_size;

  query_decoded = evhtp_kvs_new();
  if (!query_decoded)
    return -1;

  TAILQ_FOREACH(encoded, *query, next)
    {
      // Must include zero terminator in length or output won't be terminated
      // (not very clear from evhtp docs)
      val_size = strlen(encoded->val) + 1;
      if (val_size > sizeof(buf))
	continue;

      // Isn't done by evhtp_unescape_string :-(
      safe_snreplace(encoded->val, val_size, "+", " ");

      out = (unsigned char *)buf;
      evhtp_unescape_string(&out, (unsigned char *)encoded->val, val_size);
      decoded = evhtp_kv_new(encoded->key, buf, 1, 1); // 1, 1 = Copy key/val
      evhtp_kvs_add_kv(query_decoded, decoded);
    }

  evhtp_kvs_free(*query);
  *query = query_decoded;
  return 0;
}

httpd_uri_parsed *
httpd_uri_parsed_create(httpd_backend *backend)
{
  httpd_uri_parsed *parsed = NULL;
  char *path = NULL;
  size_t path_len;
  char *path_part;
  off_t path_part_offset;
  char *ptr;
  unsigned char *unescaped_part;
  int i;

  if (!backend->uri->path->path) // Not sure if this can happen
    goto error;

  path_len = strlen(backend->uri->path->path);
  if (path_len == 0)
    goto error;

  path = strdup(backend->uri->path->path);
  if (!path)
    goto error;

  parsed = calloc(1, sizeof(struct httpd_uri_parsed));
  if (!parsed)
    goto error;

  // Pointers of parsed->path_parts will point into this buffer, so it will hold
  // the uri decoded path parts separated by zeroes
  parsed->path_parts_buffer = calloc(1, path_len + 1);
  if (!parsed->path_parts_buffer)
    goto error;

  parsed->ev_uri = backend->uri;

  path_part = strtok_r(path, "/", &ptr);
  path_part_offset = path_part - path;
  for (i = 0; (i < ARRAY_SIZE(parsed->path_parts) && path_part); i++)
    {
      // libevhtp's evhtp_unescape_string() is wonky (and feels unsafe...), for
      // some reason it wants a double pointer to a user allocated buffer.
      unescaped_part = parsed->path_parts_buffer + (path_part - path) - path_part_offset;
      parsed->path_parts[i] = (char *)unescaped_part;

      evhtp_unescape_string(&unescaped_part, (unsigned char *)path_part, strlen(path_part));
      path_part = strtok_r(NULL, "/", &ptr);
    }

  // If "path_part" is not NULL, we have path tokens that could not be parsed into the "parsed->path_parts" array
  if (path_part)
    goto error;

  // uri->query isn't uri decoded, so we replace it with one that is
  if (backend->uri->query)
    query_decode(&backend->uri->query);

  free(path);
  return parsed;

 error:
  httpd_uri_parsed_free(parsed);
  free(path);
  return NULL;
}

httpd_uri_parsed *
httpd_uri_parsed_create_fromuri(const char *uri)
{
// TODO
  return NULL;
}

void
httpd_uri_parsed_free(httpd_uri_parsed *parsed)
{
  if (!parsed)
    return;
// TODO
//  if (parsed->ev_uri_is_standalone)
//    free ev_uri;

  free(parsed->path_parts_buffer);
  free(parsed);
}

httpd_query *
httpd_uri_query_get(httpd_uri_parsed *parsed)
{
  return parsed->ev_uri->query;
}

const char *
httpd_uri_path_get(httpd_uri_parsed *parsed)
{
  if (!parsed->ev_uri->path)
    return NULL;

  return parsed->ev_uri->path->full;
}

void
httpd_uri_path_parts_get(httpd_uri_path_parts *path_parts, httpd_uri_parsed *parsed)
{
  memcpy(path_parts, parsed->path_parts, sizeof(httpd_uri_path_parts));
}
