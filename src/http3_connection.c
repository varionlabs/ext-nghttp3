#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <string.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>
#include <nghttp3/nghttp3.h>

#include "internal/event.h"
#include "internal/exception.h"
#include "internal/http3_connection.h"
#include "internal/http3_request_stream.h"
#include "internal/macros.h"

zend_class_entry *php_http3_connection_ce;
static zend_object_handlers php_http3_connection_handlers;

static const char *PHP_HTTP3_STATE_REQUEST_HEADERS_SUBMITTED = "requestHeadersSubmitted";
static const char *PHP_HTTP3_STATE_LOCAL_ENDED = "localEnded";
static const char *PHP_HTTP3_STATE_REMOTE_HEADERS_RECEIVED = "remoteHeadersReceived";
static const char *PHP_HTTP3_STATE_REMOTE_ENDED = "remoteEnded";
static const char *PHP_HTTP3_STATE_TERMINAL = "terminal";
static const char *PHP_HTTP3_STATE_RESET = "reset";
static const char *PHP_HTTP3_STATE_RESET_ERROR_CODE = "resetErrorCode";

/* Varion\Ngtcp2\Event constants */
#define PHP_NGTCP2_EVENT_CONNECTION_CLOSED 2
#define PHP_NGTCP2_EVENT_CONNECTION_DRAINING 3
#define PHP_NGTCP2_EVENT_STREAM_READABLE 11
#define PHP_NGTCP2_EVENT_STREAM_CLOSED 13
#define PHP_NGTCP2_EVENT_STREAM_RESET 14

#define PHP_HTTP3_LOCAL_CONTROL_STREAM_ID 2
#define PHP_HTTP3_LOCAL_QPACK_ENCODER_STREAM_ID 6
#define PHP_HTTP3_LOCAL_QPACK_DECODER_STREAM_ID 10

typedef struct _php_http3_native_stream_body {
  zend_string *data;
  size_t read_offset;
  size_t acked_offset;
  zend_bool eof;
} php_http3_native_stream_body;

static int php_http3_call_method_on_object(zval *object, const char *method_name,
                                           uint32_t param_count, zval *params, zval *retval,
                                           const char *context_message);

static int php_http3_connection_call_fake_method(php_http3_connection *connection,
                                                 const char *method_name,
                                                 uint32_t param_count, zval *params,
                                                 zval *retval);

static int php_http3_connection_flush_h3_writes(php_http3_connection *connection);
static zval *php_http3_connection_get_or_create_stream_state(php_http3_connection *connection,
                                                             int64_t stream_id);
static zend_bool php_http3_stream_state_get_bool(zval *state, const char *key);
static void php_http3_stream_state_set_bool(zval *state, const char *key, zend_bool value);
static void php_http3_stream_state_set_error_code(zval *state, uint64_t error_code);
static void php_http3_connection_mark_stream_closed(php_http3_connection *connection,
                                                    int64_t stream_id);
static int php_http3_connection_queue_event(php_http3_connection *connection,
                                            php_http3_event_type type, int64_t stream_id,
                                            uint64_t error_code, zend_string *payload);
static int php_http3_connection_find_quic_stream(php_http3_connection *connection,
                                                 int64_t stream_id, zval *stream_object,
                                                 zend_bool allow_missing,
                                                 zend_bool *stream_found);
static int php_http3_connection_open_uni_stream_id(php_http3_connection *connection,
                                                   int64_t *stream_id);
static void php_http3_connection_cache_quic_stream(php_http3_connection *connection,
                                                   int64_t stream_id, zval *stream_object);

static void php_http3_native_stream_body_ptr_dtor(zval *zv) {
  php_http3_native_stream_body *body = (php_http3_native_stream_body *)Z_PTR_P(zv);

  if (body == NULL) {
    return;
  }

  if (body->data != NULL) {
    zend_string_release(body->data);
    body->data = NULL;
  }

  efree(body);
}

static void php_http3_connection_throw_nghttp3_error(const char *context, int rv) {
  zend_throw_exception_ex(php_http3_native_exception_ce, 0, "%s: %s", context,
                          nghttp3_strerror(rv));
}

static php_http3_native_stream_body *
php_http3_connection_get_or_create_native_body(php_http3_connection *connection,
                                               int64_t stream_id, zend_bool create_if_missing) {
  zval *zv;
  php_http3_native_stream_body *body;
  zval value;

  zv = zend_hash_index_find(&connection->native_stream_bodies, (zend_ulong)stream_id);
  if (zv != NULL && Z_TYPE_P(zv) == IS_PTR) {
    return (php_http3_native_stream_body *)Z_PTR_P(zv);
  }

  if (!create_if_missing) {
    return NULL;
  }

  body = ecalloc(1, sizeof(*body));
  body->data = zend_string_init("", 0, 0);
  body->read_offset = 0;
  body->eof = 0;

  ZVAL_PTR(&value, body);
  zend_hash_index_update(&connection->native_stream_bodies, (zend_ulong)stream_id, &value);
  return body;
}

static int php_http3_connection_append_native_body(php_http3_connection *connection,
                                                   int64_t stream_id, zend_string *data) {
  php_http3_native_stream_body *body;
  size_t old_len;
  size_t append_len;

  body = php_http3_connection_get_or_create_native_body(connection, stream_id, 1);
  if (body == NULL || body->data == NULL) {
    zend_throw_exception(php_http3_native_exception_ce,
                         "Failed to allocate native stream body buffer", 0);
    return FAILURE;
  }

  append_len = ZSTR_LEN(data);
  if (append_len == 0) {
    return SUCCESS;
  }

  old_len = ZSTR_LEN(body->data);
  body->data = zend_string_extend(body->data, old_len + append_len, 0);
  memcpy(ZSTR_VAL(body->data) + old_len, ZSTR_VAL(data), append_len);
  ZSTR_VAL(body->data)[old_len + append_len] = '\0';
  return SUCCESS;
}

static int php_http3_connection_set_native_body_eof(php_http3_connection *connection,
                                                    int64_t stream_id) {
  php_http3_native_stream_body *body;

  body = php_http3_connection_get_or_create_native_body(connection, stream_id, 1);
  if (body == NULL) {
    zend_throw_exception(php_http3_native_exception_ce,
                         "Failed to allocate native stream body state", 0);
    return FAILURE;
  }

  body->eof = 1;
  return SUCCESS;
}

static void php_http3_connection_compact_native_body(php_http3_native_stream_body *body) {
  size_t drop;
  size_t len;

  if (body == NULL || body->data == NULL || body->acked_offset == 0) {
    return;
  }

  len = ZSTR_LEN(body->data);
  drop = body->acked_offset > len ? len : body->acked_offset;
  if (drop == 0) {
    return;
  }

  if (drop < len) {
    memmove(ZSTR_VAL(body->data), ZSTR_VAL(body->data) + drop, len - drop);
  }
  ZSTR_VAL(body->data)[len - drop] = '\0';
  body->data = zend_string_truncate(body->data, len - drop, 0);
  if (body->read_offset >= drop) {
    body->read_offset -= drop;
  } else {
    body->read_offset = 0;
  }
  body->acked_offset = 0;
}

static void php_http3_connection_add_native_body_acked(php_http3_connection *connection,
                                                       int64_t stream_id, uint64_t datalen) {
  php_http3_native_stream_body *body;

  body = php_http3_connection_get_or_create_native_body(connection, stream_id, 0);
  if (body == NULL || datalen == 0) {
    return;
  }

  body->acked_offset += (size_t)datalen;
  php_http3_connection_compact_native_body(body);
}

static int php_http3_connection_emit_headers_once(php_http3_connection *connection,
                                                  int64_t stream_id) {
  zval *state;

  state = php_http3_connection_get_or_create_stream_state(connection, stream_id);
  if (php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_TERMINAL)) {
    return SUCCESS;
  }
  if (php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_REMOTE_HEADERS_RECEIVED)) {
    return SUCCESS;
  }

  php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_REMOTE_HEADERS_RECEIVED, 1);
  return php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_HEADERS_RECEIVED, stream_id,
                                          0, NULL);
}

static int php_http3_connection_emit_data_event(php_http3_connection *connection,
                                                int64_t stream_id, const uint8_t *data,
                                                size_t datalen) {
  zval *state;
  zend_string *payload;
  int rv;

  if (datalen == 0) {
    return SUCCESS;
  }

  state = php_http3_connection_get_or_create_stream_state(connection, stream_id);
  if (php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_TERMINAL)) {
    return SUCCESS;
  }

  payload = zend_string_init((const char *)data, datalen, 0);
  rv = php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_DATA_RECEIVED, stream_id, 0,
                                        payload);
  zend_string_release(payload);
  return rv;
}

static int php_http3_connection_emit_terminal_once(php_http3_connection *connection,
                                                   int64_t stream_id,
                                                   php_http3_event_type terminal_type,
                                                   uint64_t error_code) {
  zval *state;

  state = php_http3_connection_get_or_create_stream_state(connection, stream_id);
  if (php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_TERMINAL)) {
    return SUCCESS;
  }

  if (terminal_type == PHP_HTTP3_EVENT_STREAM_RESET) {
    php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_RESET, 1);
    php_http3_stream_state_set_error_code(state, error_code);
  } else {
    php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_REMOTE_ENDED, 1);
  }
  php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_TERMINAL, 1);
  zend_hash_index_del(&connection->native_stream_bodies, (zend_ulong)stream_id);
  php_http3_connection_mark_stream_closed(connection, stream_id);
  return php_http3_connection_queue_event(connection, terminal_type, stream_id, error_code, NULL);
}

static nghttp3_ssize php_http3_h3_read_data_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                               nghttp3_vec *vec, size_t veccnt, uint32_t *pflags,
                                               void *conn_user_data, void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;
  php_http3_native_stream_body *body;
  size_t left;

  (void)h3_conn;
  (void)stream_user_data;

  if (connection == NULL || veccnt == 0) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  body = php_http3_connection_get_or_create_native_body(connection, stream_id, 0);
  if (body == NULL || body->data == NULL) {
    return NGHTTP3_ERR_WOULDBLOCK;
  }

  if (body->read_offset < ZSTR_LEN(body->data)) {
    left = ZSTR_LEN(body->data) - body->read_offset;
    vec[0].base = (uint8_t *)(ZSTR_VAL(body->data) + body->read_offset);
    vec[0].len = left;
    body->read_offset += left;
    if (body->eof && body->read_offset >= ZSTR_LEN(body->data)) {
      *pflags |= NGHTTP3_DATA_FLAG_EOF;
    }
    return 1;
  }

  if (body->eof) {
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 0;
  }

  return NGHTTP3_ERR_WOULDBLOCK;
}

static int php_http3_h3_stream_close_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                        uint64_t app_error_code, void *conn_user_data,
                                        void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;
  php_http3_event_type event_type;
  int rv;

  (void)h3_conn;
  (void)stream_user_data;

  if (connection == NULL) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  event_type = app_error_code != 0 ? PHP_HTTP3_EVENT_STREAM_RESET
                                    : PHP_HTTP3_EVENT_REQUEST_COMPLETED;
  rv = php_http3_connection_emit_terminal_once(connection, stream_id, event_type, app_error_code);
  return rv == SUCCESS ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int php_http3_h3_recv_data_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                     const uint8_t *data, size_t datalen,
                                     void *conn_user_data, void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;

  (void)h3_conn;
  (void)stream_user_data;

  if (connection == NULL) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  if (php_http3_connection_emit_data_event(connection, stream_id, data, datalen) != SUCCESS) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

static int php_http3_h3_deferred_consume_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                            size_t consumed, void *conn_user_data,
                                            void *stream_user_data) {
  (void)h3_conn;
  (void)stream_id;
  (void)consumed;
  (void)conn_user_data;
  (void)stream_user_data;
  return 0;
}

static int php_http3_h3_acked_stream_data_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                             uint64_t datalen, void *conn_user_data,
                                             void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;

  (void)h3_conn;
  (void)stream_user_data;

  if (connection == NULL) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  php_http3_connection_add_native_body_acked(connection, stream_id, datalen);
  return 0;
}

static int php_http3_h3_recv_header_cb(nghttp3_conn *h3_conn, int64_t stream_id, int32_t token,
                                       nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                                       void *conn_user_data, void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;

  (void)h3_conn;
  (void)token;
  (void)name;
  (void)value;
  (void)flags;
  (void)stream_user_data;

  if (connection == NULL) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return php_http3_connection_emit_headers_once(connection, stream_id) == SUCCESS
           ? 0
           : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int php_http3_h3_end_stream_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                      void *conn_user_data, void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;

  (void)h3_conn;
  (void)stream_user_data;

  if (connection == NULL) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return php_http3_connection_emit_terminal_once(connection, stream_id,
                                                 PHP_HTTP3_EVENT_REQUEST_COMPLETED, 0) == SUCCESS
           ? 0
           : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int php_http3_h3_stop_sending_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                        uint64_t app_error_code, void *conn_user_data,
                                        void *stream_user_data) {
  php_http3_connection *connection = (php_http3_connection *)conn_user_data;

  (void)h3_conn;
  (void)stream_user_data;

  if (connection == NULL) {
    return NGHTTP3_ERR_CALLBACK_FAILURE;
  }

  return php_http3_connection_reset_stream(connection, stream_id, app_error_code) == SUCCESS
           ? 0
           : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int php_http3_h3_reset_stream_cb(nghttp3_conn *h3_conn, int64_t stream_id,
                                        uint64_t app_error_code, void *conn_user_data,
                                        void *stream_user_data) {
  return php_http3_h3_stop_sending_cb(h3_conn, stream_id, app_error_code, conn_user_data,
                                      stream_user_data);
}

static int php_http3_connection_init_native_h3(php_http3_connection *connection) {
  nghttp3_callbacks callbacks;
  nghttp3_settings settings;
  int rv;

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.acked_stream_data = php_http3_h3_acked_stream_data_cb;
  callbacks.stream_close = php_http3_h3_stream_close_cb;
  callbacks.recv_data = php_http3_h3_recv_data_cb;
  callbacks.deferred_consume = php_http3_h3_deferred_consume_cb;
  callbacks.recv_header = php_http3_h3_recv_header_cb;
  callbacks.end_stream = php_http3_h3_end_stream_cb;
  callbacks.stop_sending = php_http3_h3_stop_sending_cb;
  callbacks.reset_stream = php_http3_h3_reset_stream_cb;

  nghttp3_settings_default(&settings);
  rv = nghttp3_conn_client_new(&connection->h3_conn, &callbacks, &settings, NULL, connection);
  if (rv != 0) {
    php_http3_connection_throw_nghttp3_error("nghttp3_conn_client_new failed", rv);
    return FAILURE;
  }

  connection->native_h3_enabled = 1;
  connection->native_h3_streams_bound = 0;
  return SUCCESS;
}

static int php_http3_connection_bind_native_h3_streams(php_http3_connection *connection) {
  int64_t control_stream_id = PHP_HTTP3_LOCAL_CONTROL_STREAM_ID;
  int64_t qpack_encoder_stream_id = PHP_HTTP3_LOCAL_QPACK_ENCODER_STREAM_ID;
  int64_t qpack_decoder_stream_id = PHP_HTTP3_LOCAL_QPACK_DECODER_STREAM_ID;
  int rv;

  if (!connection->native_h3_enabled || connection->h3_conn == NULL) {
    return SUCCESS;
  }
  if (connection->native_h3_streams_bound) {
    return SUCCESS;
  }

  if (php_http3_connection_open_uni_stream_id(connection, &control_stream_id) != SUCCESS) {
    if (EG(exception)) {
      return FAILURE;
    }
    control_stream_id = PHP_HTTP3_LOCAL_CONTROL_STREAM_ID;
  }
  if (php_http3_connection_open_uni_stream_id(connection, &qpack_encoder_stream_id) != SUCCESS) {
    if (EG(exception)) {
      return FAILURE;
    }
    qpack_encoder_stream_id = PHP_HTTP3_LOCAL_QPACK_ENCODER_STREAM_ID;
  }
  if (php_http3_connection_open_uni_stream_id(connection, &qpack_decoder_stream_id) != SUCCESS) {
    if (EG(exception)) {
      return FAILURE;
    }
    qpack_decoder_stream_id = PHP_HTTP3_LOCAL_QPACK_DECODER_STREAM_ID;
  }

  rv = nghttp3_conn_bind_control_stream(connection->h3_conn, control_stream_id);
  if (rv != 0) {
    php_http3_connection_throw_nghttp3_error("nghttp3_conn_bind_control_stream failed", rv);
    return FAILURE;
  }

  rv = nghttp3_conn_bind_qpack_streams(connection->h3_conn, qpack_encoder_stream_id,
                                       qpack_decoder_stream_id);
  if (rv != 0) {
    php_http3_connection_throw_nghttp3_error("nghttp3_conn_bind_qpack_streams failed", rv);
    return FAILURE;
  }

  connection->native_h3_streams_bound = 1;
  return SUCCESS;
}

static void php_http3_connection_init_stream_state(zval *state) {
  array_init(state);
  add_assoc_bool(state, PHP_HTTP3_STATE_REQUEST_HEADERS_SUBMITTED, 0);
  add_assoc_bool(state, PHP_HTTP3_STATE_LOCAL_ENDED, 0);
  add_assoc_bool(state, PHP_HTTP3_STATE_REMOTE_HEADERS_RECEIVED, 0);
  add_assoc_bool(state, PHP_HTTP3_STATE_REMOTE_ENDED, 0);
  add_assoc_bool(state, PHP_HTTP3_STATE_TERMINAL, 0);
  add_assoc_bool(state, PHP_HTTP3_STATE_RESET, 0);
  add_assoc_null(state, PHP_HTTP3_STATE_RESET_ERROR_CODE);
}

static zval *php_http3_connection_get_or_create_stream_state(php_http3_connection *connection,
                                                             int64_t stream_id) {
  zval *state = zend_hash_index_find(&connection->stream_states, stream_id);

  if (state != NULL) {
    return state;
  }

  {
    zval new_state;
    php_http3_connection_init_stream_state(&new_state);
    zend_hash_index_update(&connection->stream_states, stream_id, &new_state);
  }

  return zend_hash_index_find(&connection->stream_states, stream_id);
}

static zend_bool php_http3_stream_state_get_bool(zval *state, const char *key) {
  zval *value;

  if (state == NULL || Z_TYPE_P(state) != IS_ARRAY) {
    return 0;
  }

  value = zend_hash_str_find(Z_ARRVAL_P(state), key, strlen(key));
  if (value == NULL) {
    return 0;
  }

  return zend_is_true(value);
}

static void php_http3_stream_state_set_bool(zval *state, const char *key, zend_bool value) {
  if (state == NULL || Z_TYPE_P(state) != IS_ARRAY) {
    return;
  }

  add_assoc_bool(state, key, value);
}

static void php_http3_stream_state_set_error_code(zval *state, uint64_t error_code) {
  if (state == NULL || Z_TYPE_P(state) != IS_ARRAY) {
    return;
  }

  add_assoc_long(state, PHP_HTTP3_STATE_RESET_ERROR_CODE, (zend_long)error_code);
}

static void php_http3_connection_mark_stream_closed(php_http3_connection *connection,
                                                    int64_t stream_id) {
  zval *stream_zv = zend_hash_index_find(&connection->request_streams, stream_id);

  if (stream_zv == NULL || Z_TYPE_P(stream_zv) != IS_OBJECT) {
    return;
  }

  Z_HTTP3_REQUEST_STREAM_P(stream_zv)->closed = 1;
}

static int php_http3_connection_queue_event(php_http3_connection *connection,
                                            php_http3_event_type type, int64_t stream_id,
                                            uint64_t error_code, zend_string *payload) {
  zval event_zv;
  php_http3_event_object *event_obj;

  object_init_ex(&event_zv, php_http3_event_class_for_type(type));
  event_obj = Z_HTTP3_EVENT_OBJ_P(&event_zv);
  event_obj->type = type;
  event_obj->stream_id = stream_id;
  event_obj->error_code = error_code;

  if (event_obj->payload != NULL) {
    zend_string_release(event_obj->payload);
  }
  if (payload != NULL) {
    event_obj->payload = zend_string_copy(payload);
  } else {
    event_obj->payload = zend_string_init("", 0, 0);
  }

  add_next_index_zval(&connection->event_queue, &event_zv);
  return SUCCESS;
}

static int php_http3_connection_consume_readable_signal(php_http3_connection *connection,
                                                        zval *signal) {
  zval *stream_id_zv;
  zval *data_zv;
  zval *fin_zv;
  zval *state;
  int64_t stream_id;
  zend_bool fin;

  stream_id_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "streamId", sizeof("streamId") - 1);
  if (stream_id_zv == NULL) {
    return SUCCESS;
  }

  stream_id = (int64_t)zval_get_long(stream_id_zv);
  state = php_http3_connection_get_or_create_stream_state(connection, stream_id);
  if (php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_TERMINAL)) {
    return SUCCESS;
  }

  if (!php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_REMOTE_HEADERS_RECEIVED)) {
    php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_HEADERS_RECEIVED, stream_id, 0,
                                     NULL);
    php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_REMOTE_HEADERS_RECEIVED, 1);
  }

  data_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "data", sizeof("data") - 1);
  if (data_zv != NULL && Z_TYPE_P(data_zv) == IS_STRING && Z_STRLEN_P(data_zv) > 0) {
    php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_DATA_RECEIVED, stream_id, 0,
                                     Z_STR_P(data_zv));
  }

  fin_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "fin", sizeof("fin") - 1);
  fin = fin_zv != NULL ? zend_is_true(fin_zv) : 0;
  if (fin && !php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_TERMINAL)) {
    php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_REQUEST_COMPLETED, stream_id, 0,
                                     NULL);
    php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_REMOTE_ENDED, 1);
    php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_TERMINAL, 1);
    php_http3_connection_mark_stream_closed(connection, stream_id);
  }

  return SUCCESS;
}

static int php_http3_connection_consume_reset_signal(php_http3_connection *connection,
                                                     zval *signal) {
  zval *stream_id_zv;
  zval *error_code_zv;
  zval *state;
  int64_t stream_id;
  uint64_t error_code = 0;

  stream_id_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "streamId", sizeof("streamId") - 1);
  if (stream_id_zv == NULL) {
    return SUCCESS;
  }

  error_code_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "errorCode", sizeof("errorCode") - 1);
  if (error_code_zv != NULL) {
    error_code = (uint64_t)zval_get_long(error_code_zv);
  }

  stream_id = (int64_t)zval_get_long(stream_id_zv);
  state = php_http3_connection_get_or_create_stream_state(connection, stream_id);
  if (php_http3_stream_state_get_bool(state, PHP_HTTP3_STATE_TERMINAL)) {
    return SUCCESS;
  }

  php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_STREAM_RESET, stream_id, error_code,
                                   NULL);
  php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_RESET, 1);
  php_http3_stream_state_set_bool(state, PHP_HTTP3_STATE_TERMINAL, 1);
  php_http3_stream_state_set_error_code(state, error_code);
  php_http3_connection_mark_stream_closed(connection, stream_id);

  return SUCCESS;
}

static int php_http3_connection_consume_closing_signal(php_http3_connection *connection,
                                                       zval *signal) {
  zval *closed_zv;
  zval *error_code_zv;
  zend_bool closed = 0;
  uint64_t error_code = 0;

  closed_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "closed", sizeof("closed") - 1);
  if (closed_zv != NULL) {
    closed = zend_is_true(closed_zv);
  }

  if (connection->state == PHP_HTTP3_CONN_CLOSED) {
    return SUCCESS;
  }
  if (connection->state == PHP_HTTP3_CONN_GOAWAY_RECEIVED ||
      connection->state == PHP_HTTP3_CONN_CLOSING) {
    connection->closing = 1;
    if (closed) {
      connection->state = PHP_HTTP3_CONN_CLOSED;
    }
    return SUCCESS;
  }

  error_code_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "errorCode", sizeof("errorCode") - 1);
  if (error_code_zv != NULL) {
    error_code = (uint64_t)zval_get_long(error_code_zv);
  }

  connection->closing = 1;
  connection->state = closed ? PHP_HTTP3_CONN_CLOSED : PHP_HTTP3_CONN_GOAWAY_RECEIVED;
  php_http3_connection_queue_event(connection, PHP_HTTP3_EVENT_GOAWAY_RECEIVED, -1, error_code,
                                   NULL);

  return SUCCESS;
}

static int php_http3_connection_consume_signal(php_http3_connection *connection, zval *signal) {
  zval *type_zv;
  zend_long type;

  if (Z_TYPE_P(signal) != IS_ARRAY) {
    return SUCCESS;
  }

  type_zv = zend_hash_str_find(Z_ARRVAL_P(signal), "type", sizeof("type") - 1);
  if (type_zv == NULL) {
    return SUCCESS;
  }

  type = zval_get_long(type_zv);
  switch ((php_http3_signal_type)type) {
  case PHP_HTTP3_SIGNAL_STREAM_READABLE:
    return php_http3_connection_consume_readable_signal(connection, signal);
  case PHP_HTTP3_SIGNAL_STREAM_RESET:
    return php_http3_connection_consume_reset_signal(connection, signal);
  case PHP_HTTP3_SIGNAL_CONNECTION_CLOSING:
    return php_http3_connection_consume_closing_signal(connection, signal);
  default:
    return SUCCESS;
  }
}

static int php_http3_call_method_on_object(zval *object, const char *method_name,
                                           uint32_t param_count, zval *params, zval *retval,
                                           const char *context_message) {
  zval function_name;

  ZVAL_STRING(&function_name, method_name);
  if (call_user_function(NULL, object, &function_name, retval, param_count, params) ==
      FAILURE) {
    zval_ptr_dtor(&function_name);
    if (!EG(exception)) {
      zend_throw_exception(php_http3_native_exception_ce, context_message, 0);
    }
    return FAILURE;
  }
  zval_ptr_dtor(&function_name);

  if (EG(exception)) {
    return FAILURE;
  }

  return SUCCESS;
}

static int php_http3_connection_pump_fake_signals(php_http3_connection *connection) {
  zval retval;
  zval *signal;

  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
    return SUCCESS;
  }

  ZVAL_UNDEF(&retval);
  if (php_http3_connection_call_fake_method(connection, "pollSignals", 0, NULL, &retval) !=
      SUCCESS) {
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  if (Z_TYPE(retval) != IS_ARRAY) {
    zval_ptr_dtor(&retval);
    zend_throw_exception(php_http3_native_exception_ce,
                         "Fake adapter pollSignals() must return array", 0);
    return FAILURE;
  }

  ZEND_HASH_FOREACH_VAL(Z_ARRVAL(retval), signal) {
    if (php_http3_connection_consume_signal(connection, signal) != SUCCESS) {
      zval_ptr_dtor(&retval);
      return FAILURE;
    }
  } ZEND_HASH_FOREACH_END();

  zval_ptr_dtor(&retval);
  return SUCCESS;
}

static int php_http3_connection_call_fake_method(php_http3_connection *connection,
                                                 const char *method_name,
                                                 uint32_t param_count, zval *params,
                                                 zval *retval) {
  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
    return FAILURE;
  }

  return php_http3_call_method_on_object(&connection->fake_adapter, method_name, param_count,
                                         params, retval,
                                         "Failed to call fake adapter method");
}

static int php_http3_connection_open_uni_stream_id(php_http3_connection *connection,
                                                   int64_t *stream_id) {
  zval retval;
  zval id_retval;

  if (Z_ISUNDEF(connection->quic_connection)) {
    zend_throw_exception(php_http3_state_exception_ce, "QUIC connection is not available", 0);
    return FAILURE;
  }

  if (!zend_hash_str_exists(&Z_OBJCE(connection->quic_connection)->function_table,
                            ZEND_STRL("openunistream"))) {
    return FAILURE;
  }

  ZVAL_UNDEF(&retval);
  if (php_http3_call_method_on_object(&connection->quic_connection, "openUniStream", 0, NULL,
                                      &retval, "Failed to call QUIC openUniStream()") !=
      SUCCESS) {
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  if (Z_TYPE(retval) != IS_OBJECT) {
    zval_ptr_dtor(&retval);
    zend_throw_exception(php_http3_native_exception_ce,
                         "QUIC openUniStream() must return stream object", 0);
    return FAILURE;
  }

  ZVAL_UNDEF(&id_retval);
  if (php_http3_call_method_on_object(&retval, "getId", 0, NULL, &id_retval,
                                      "Failed to read QUIC uni stream id") != SUCCESS) {
    zval_ptr_dtor(&retval);
    if (!Z_ISUNDEF(id_retval)) {
      zval_ptr_dtor(&id_retval);
    }
    return FAILURE;
  }

  if (Z_TYPE(id_retval) != IS_LONG) {
    zval_ptr_dtor(&retval);
    zval_ptr_dtor(&id_retval);
    zend_throw_exception(php_http3_native_exception_ce,
                         "QUIC uni stream getId() must return int", 0);
    return FAILURE;
  }

  *stream_id = (int64_t)Z_LVAL(id_retval);
  php_http3_connection_cache_quic_stream(connection, *stream_id, &retval);
  zval_ptr_dtor(&retval);
  zval_ptr_dtor(&id_retval);
  return SUCCESS;
}

static int php_http3_connection_write_quic_stream_optional(php_http3_connection *connection,
                                                           int64_t stream_id, zend_string *data,
                                                           zend_bool allow_missing,
                                                           zend_bool *stream_found) {
  zval stream_object;
  zval params[1];
  zval retval;
  zend_bool found = 0;

  ZVAL_UNDEF(&stream_object);
  if (php_http3_connection_find_quic_stream(connection, stream_id, &stream_object, allow_missing,
                                            &found) != SUCCESS) {
    return FAILURE;
  }
  if (!found) {
    if (stream_found != NULL) {
      *stream_found = 0;
    }
    return SUCCESS;
  }

  ZVAL_STR_COPY(&params[0], data);
  ZVAL_UNDEF(&retval);
  if (php_http3_call_method_on_object(&stream_object, "write", 1, params, &retval,
                                      "Failed to call QUIC stream write()") != SUCCESS) {
    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&stream_object);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  zval_ptr_dtor(&params[0]);
  zval_ptr_dtor(&stream_object);
  if (!Z_ISUNDEF(retval)) {
    zval_ptr_dtor(&retval);
  }
  if (stream_found != NULL) {
    *stream_found = 1;
  }
  return SUCCESS;
}

static int php_http3_connection_finish_quic_stream_optional(php_http3_connection *connection,
                                                            int64_t stream_id,
                                                            zend_bool allow_missing) {
  zval stream_object;
  zval retval;
  zend_bool found = 0;

  ZVAL_UNDEF(&stream_object);
  if (php_http3_connection_find_quic_stream(connection, stream_id, &stream_object, allow_missing,
                                            &found) != SUCCESS) {
    return FAILURE;
  }
  if (!found) {
    return SUCCESS;
  }

  ZVAL_UNDEF(&retval);
  if (php_http3_call_method_on_object(&stream_object, "end", 0, NULL, &retval,
                                      "Failed to call QUIC stream end()") != SUCCESS) {
    zval_ptr_dtor(&stream_object);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  zval_ptr_dtor(&stream_object);
  if (!Z_ISUNDEF(retval)) {
    zval_ptr_dtor(&retval);
  }
  return SUCCESS;
}

static int php_http3_connection_flush_h3_writes(php_http3_connection *connection) {
  nghttp3_vec vecs[8];
  int64_t stream_id = -1;
  int fin = 0;
  nghttp3_ssize veccnt;
  size_t total_len;
  size_t i;
  zend_string *payload;
  int rv;
  zend_bool stream_found;
  int guard = 0;

  if (!connection->native_h3_enabled || connection->h3_conn == NULL) {
    return SUCCESS;
  }

  for (guard = 0; guard < 4096; ++guard) {
    veccnt = nghttp3_conn_writev_stream(connection->h3_conn, &stream_id, &fin, vecs,
                                        sizeof(vecs) / sizeof(vecs[0]));
    if (veccnt < 0) {
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_writev_stream failed",
                                               (int)veccnt);
      return FAILURE;
    }
    if (veccnt == 0 && stream_id < 0) {
      return SUCCESS;
    }

    if (veccnt == 0 && stream_id >= 0) {
      if (fin) {
        if (php_http3_connection_finish_quic_stream_optional(connection, stream_id, 1) !=
            SUCCESS) {
          return FAILURE;
        }
      }
      rv = nghttp3_conn_add_write_offset(connection->h3_conn, stream_id, 0);
      if (rv != 0) {
        php_http3_connection_throw_nghttp3_error("nghttp3_conn_add_write_offset failed", rv);
        return FAILURE;
      }
      continue;
    }

    total_len = 0;
    for (i = 0; i < (size_t)veccnt; ++i) {
      total_len += vecs[i].len;
    }

    payload = zend_string_alloc(total_len, 0);
    total_len = 0;
    for (i = 0; i < (size_t)veccnt; ++i) {
      if (vecs[i].len == 0) {
        continue;
      }
      memcpy(ZSTR_VAL(payload) + total_len, vecs[i].base, vecs[i].len);
      total_len += vecs[i].len;
    }
    ZSTR_VAL(payload)[total_len] = '\0';

    stream_found = 0;
    if (php_http3_connection_write_quic_stream_optional(connection, stream_id, payload, 1,
                                                        &stream_found) != SUCCESS) {
      zend_string_release(payload);
      return FAILURE;
    }
    zend_string_release(payload);

    if (fin) {
      if (php_http3_connection_finish_quic_stream_optional(connection, stream_id, 1) != SUCCESS) {
        return FAILURE;
      }
    }

    rv = nghttp3_conn_add_write_offset(connection->h3_conn, stream_id, total_len);
    if (rv != 0) {
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_add_write_offset failed", rv);
      return FAILURE;
    }
    (void)stream_found;
  }

  zend_throw_exception(php_http3_native_exception_ce,
                       "nghttp3 write loop reached iteration limit", 0);
  return FAILURE;
}

static void php_http3_connection_cache_quic_stream(php_http3_connection *connection,
                                                   int64_t stream_id, zval *stream_object) {
  zval copy;

  ZVAL_COPY(&copy, stream_object);
  zend_hash_index_update(&connection->quic_streams, stream_id, &copy);
}

static int php_http3_connection_find_quic_stream(php_http3_connection *connection,
                                                 int64_t stream_id, zval *stream_object,
                                                 zend_bool allow_missing,
                                                 zend_bool *stream_found) {
  zval *cached;
  zval params[1];
  zval retval;

  if (stream_found != NULL) {
    *stream_found = 0;
  }

  cached = zend_hash_index_find(&connection->quic_streams, stream_id);
  if (cached != NULL && Z_TYPE_P(cached) == IS_OBJECT) {
    if (stream_found != NULL) {
      *stream_found = 1;
    }
    ZVAL_COPY(stream_object, cached);
    return SUCCESS;
  }

  if (Z_ISUNDEF(connection->quic_connection)) {
    if (!allow_missing) {
      zend_throw_exception(php_http3_state_exception_ce, "QUIC connection is not available", 0);
    }
    return allow_missing ? SUCCESS : FAILURE;
  }

  ZVAL_LONG(&params[0], stream_id);
  ZVAL_UNDEF(&retval);
  if (php_http3_call_method_on_object(&connection->quic_connection, "getStream", 1, params,
                                      &retval, "Failed to call QUIC getStream()") != SUCCESS) {
    zval_ptr_dtor(&params[0]);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }
  zval_ptr_dtor(&params[0]);

  if (Z_TYPE(retval) == IS_OBJECT) {
    if (stream_found != NULL) {
      *stream_found = 1;
    }
    php_http3_connection_cache_quic_stream(connection, stream_id, &retval);
    ZVAL_COPY(stream_object, &retval);
    zval_ptr_dtor(&retval);
    return SUCCESS;
  }

  zval_ptr_dtor(&retval);
  if (allow_missing) {
    return SUCCESS;
  }

  zend_throw_exception(php_http3_state_exception_ce, "QUIC stream was not found", 0);
  return FAILURE;
}

static int php_http3_connection_get_quic_stream(php_http3_connection *connection,
                                                int64_t stream_id, zval *stream_object) {
  return php_http3_connection_find_quic_stream(connection, stream_id, stream_object, 0, NULL);
}

static int php_http3_connection_consume_ngtcp2_stream_readable(php_http3_connection *connection,
                                                               int64_t stream_id) {
  zval stream_object;
  zval read_params[1];
  zval read_retval;
  zval is_closed_retval;
  zval signal;
  smart_str payload = {0};
  zend_bool fin = 0;
  int i;
  nghttp3_ssize nread;

  ZVAL_UNDEF(&stream_object);
  if (php_http3_connection_get_quic_stream(connection, stream_id, &stream_object) != SUCCESS) {
    if (!EG(exception)) {
      return SUCCESS;
    }
    return FAILURE;
  }

  ZVAL_LONG(&read_params[0], 65535);
  for (i = 0; i < 1024; ++i) {
    ZVAL_UNDEF(&read_retval);
    if (php_http3_call_method_on_object(&stream_object, "read", 1, read_params, &read_retval,
                                        "Failed to read QUIC stream") != SUCCESS) {
      zval_ptr_dtor(&read_params[0]);
      zval_ptr_dtor(&stream_object);
      if (!Z_ISUNDEF(read_retval)) {
        zval_ptr_dtor(&read_retval);
      }
      smart_str_free(&payload);
      return FAILURE;
    }

    if (Z_TYPE(read_retval) != IS_STRING || Z_STRLEN(read_retval) == 0) {
      zval_ptr_dtor(&read_retval);
      break;
    }

    smart_str_appendl(&payload, Z_STRVAL(read_retval), Z_STRLEN(read_retval));
    zval_ptr_dtor(&read_retval);
  }
  zval_ptr_dtor(&read_params[0]);
  smart_str_0(&payload);

  ZVAL_UNDEF(&is_closed_retval);
  if (php_http3_call_method_on_object(&stream_object, "isClosed", 0, NULL, &is_closed_retval,
                                      "Failed to inspect QUIC stream state") != SUCCESS) {
    zval_ptr_dtor(&stream_object);
    smart_str_free(&payload);
    if (!Z_ISUNDEF(is_closed_retval)) {
      zval_ptr_dtor(&is_closed_retval);
    }
    return FAILURE;
  }
  fin = zend_is_true(&is_closed_retval);

  if (connection->native_h3_enabled && connection->h3_conn != NULL) {
    nread = nghttp3_conn_read_stream(connection->h3_conn, stream_id,
                                     payload.s != NULL ? (const uint8_t *)ZSTR_VAL(payload.s)
                                                       : (const uint8_t *)"",
                                     payload.s != NULL ? ZSTR_LEN(payload.s) : 0, fin);
    if (nread < 0) {
      zval_ptr_dtor(&stream_object);
      zval_ptr_dtor(&is_closed_retval);
      smart_str_free(&payload);
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_read_stream failed", (int)nread);
      return FAILURE;
    }

    zval_ptr_dtor(&stream_object);
    zval_ptr_dtor(&is_closed_retval);
    smart_str_free(&payload);
    return SUCCESS;
  }

  array_init(&signal);
  add_assoc_long(&signal, "type", PHP_HTTP3_SIGNAL_STREAM_READABLE);
  add_assoc_long(&signal, "streamId", (zend_long)stream_id);
  if (payload.s != NULL) {
    add_assoc_str(&signal, "data", zend_string_copy(payload.s));
  } else {
    add_assoc_string(&signal, "data", "");
  }
  add_assoc_bool(&signal, "fin", fin);

  if (php_http3_connection_consume_signal(connection, &signal) != SUCCESS) {
    zval_ptr_dtor(&signal);
    zval_ptr_dtor(&stream_object);
    zval_ptr_dtor(&read_retval);
    zval_ptr_dtor(&is_closed_retval);
    return FAILURE;
  }

  zval_ptr_dtor(&signal);
  zval_ptr_dtor(&stream_object);
  zval_ptr_dtor(&is_closed_retval);
  smart_str_free(&payload);
  return SUCCESS;
}

static int php_http3_connection_consume_ngtcp2_stream_closed(php_http3_connection *connection,
                                                             int64_t stream_id) {
  int rv;
  zval signal;

  if (connection->native_h3_enabled && connection->h3_conn != NULL) {
    rv = nghttp3_conn_close_stream(connection->h3_conn, stream_id, 0);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_close_stream failed", rv);
      return FAILURE;
    }
  }

  array_init(&signal);
  add_assoc_long(&signal, "type", PHP_HTTP3_SIGNAL_STREAM_READABLE);
  add_assoc_long(&signal, "streamId", (zend_long)stream_id);
  add_assoc_string(&signal, "data", "");
  add_assoc_bool(&signal, "fin", 1);

  if (php_http3_connection_consume_signal(connection, &signal) != SUCCESS) {
    zval_ptr_dtor(&signal);
    return FAILURE;
  }

  zval_ptr_dtor(&signal);
  return SUCCESS;
}

static int php_http3_connection_consume_ngtcp2_event(php_http3_connection *connection,
                                                     zval *event_object) {
  zval retval;
  zval signal;
  zend_long type;
  zend_long stream_id = -1;
  zend_long error_code = 0;

  ZVAL_UNDEF(&retval);
  if (php_http3_call_method_on_object(event_object, "getType", 0, NULL, &retval,
                                      "Failed to read QUIC event type") != SUCCESS) {
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  type = zval_get_long(&retval);
  zval_ptr_dtor(&retval);

  if (type == PHP_NGTCP2_EVENT_STREAM_READABLE || type == PHP_NGTCP2_EVENT_STREAM_CLOSED ||
      type == PHP_NGTCP2_EVENT_STREAM_RESET) {
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(event_object, "getStreamId", 0, NULL, &retval,
                                        "Failed to read QUIC event stream id") != SUCCESS) {
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }
    stream_id = zval_get_long(&retval);
    zval_ptr_dtor(&retval);
  }

  switch (type) {
  case PHP_NGTCP2_EVENT_STREAM_READABLE:
    return php_http3_connection_consume_ngtcp2_stream_readable(connection, stream_id);
  case PHP_NGTCP2_EVENT_STREAM_CLOSED:
    return php_http3_connection_consume_ngtcp2_stream_closed(connection, stream_id);
  case PHP_NGTCP2_EVENT_STREAM_RESET:
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(event_object, "getErrorCode", 0, NULL, &retval,
                                        "Failed to read QUIC event error code") != SUCCESS) {
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }
    error_code = zval_get_long(&retval);
    zval_ptr_dtor(&retval);

    if (connection->native_h3_enabled && connection->h3_conn != NULL) {
      int close_rv = nghttp3_conn_close_stream(connection->h3_conn, stream_id,
                                               (uint64_t)error_code);
      if (close_rv != 0 && close_rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        php_http3_connection_throw_nghttp3_error("nghttp3_conn_close_stream failed", close_rv);
        return FAILURE;
      }
    }

    array_init(&signal);
    add_assoc_long(&signal, "type", PHP_HTTP3_SIGNAL_STREAM_RESET);
    add_assoc_long(&signal, "streamId", stream_id);
    add_assoc_long(&signal, "errorCode", error_code);
    if (php_http3_connection_consume_signal(connection, &signal) != SUCCESS) {
      zval_ptr_dtor(&signal);
      return FAILURE;
    }
    zval_ptr_dtor(&signal);
    return SUCCESS;
  case PHP_NGTCP2_EVENT_CONNECTION_CLOSED:
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(event_object, "getErrorCode", 0, NULL, &retval,
                                        "Failed to read QUIC connection error code") != SUCCESS) {
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }
    error_code = zval_get_long(&retval);
    zval_ptr_dtor(&retval);

    array_init(&signal);
    add_assoc_long(&signal, "type", PHP_HTTP3_SIGNAL_CONNECTION_CLOSING);
    add_assoc_long(&signal, "errorCode", error_code);
    add_assoc_bool(&signal, "closed", 1);
    if (php_http3_connection_consume_signal(connection, &signal) != SUCCESS) {
      zval_ptr_dtor(&signal);
      return FAILURE;
    }
    zval_ptr_dtor(&signal);
    return SUCCESS;
  case PHP_NGTCP2_EVENT_CONNECTION_DRAINING:
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(event_object, "getErrorCode", 0, NULL, &retval,
                                        "Failed to read QUIC connection error code") != SUCCESS) {
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }
    error_code = zval_get_long(&retval);
    zval_ptr_dtor(&retval);

    array_init(&signal);
    add_assoc_long(&signal, "type", PHP_HTTP3_SIGNAL_CONNECTION_CLOSING);
    add_assoc_long(&signal, "errorCode", error_code);
    if (php_http3_connection_consume_signal(connection, &signal) != SUCCESS) {
      zval_ptr_dtor(&signal);
      return FAILURE;
    }
    zval_ptr_dtor(&signal);
    return SUCCESS;
  default:
    return SUCCESS;
  }
}

static int php_http3_connection_pump_ngtcp2_signals(php_http3_connection *connection) {
  zval retval;
  zval *event_object;

  if (connection->use_fake_adapter || Z_ISUNDEF(connection->quic_connection)) {
    return SUCCESS;
  }

  ZVAL_UNDEF(&retval);
  if (php_http3_call_method_on_object(&connection->quic_connection, "pollEvents", 0, NULL,
                                      &retval, "Failed to call QUIC pollEvents()") != SUCCESS) {
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  if (Z_TYPE(retval) != IS_ARRAY) {
    zval_ptr_dtor(&retval);
    zend_throw_exception(php_http3_native_exception_ce,
                         "QUIC pollEvents() must return array", 0);
    return FAILURE;
  }

  ZEND_HASH_FOREACH_VAL(Z_ARRVAL(retval), event_object) {
    if (Z_TYPE_P(event_object) != IS_OBJECT) {
      continue;
    }
    if (php_http3_connection_consume_ngtcp2_event(connection, event_object) != SUCCESS) {
      zval_ptr_dtor(&retval);
      return FAILURE;
    }
  } ZEND_HASH_FOREACH_END();

  zval_ptr_dtor(&retval);
  return SUCCESS;
}

int php_http3_connection_open_request_stream_id(php_http3_connection *connection,
                                                int64_t *stream_id) {
  zval retval;
  zval stream_id_retval;

  if (!connection->use_fake_adapter) {
    if (Z_ISUNDEF(connection->quic_connection)) {
      zend_throw_exception(php_http3_state_exception_ce, "QUIC connection is not available", 0);
      return FAILURE;
    }

    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(&connection->quic_connection, "openStream", 0, NULL,
                                        &retval, "Failed to call QUIC openStream()") != SUCCESS) {
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }

    if (Z_TYPE(retval) != IS_OBJECT) {
      zval_ptr_dtor(&retval);
      zend_throw_exception(php_http3_native_exception_ce,
                           "QUIC openStream() must return stream object", 0);
      return FAILURE;
    }

    ZVAL_UNDEF(&stream_id_retval);
    if (php_http3_call_method_on_object(&retval, "getId", 0, NULL, &stream_id_retval,
                                        "Failed to read QUIC stream id") != SUCCESS) {
      zval_ptr_dtor(&retval);
      if (!Z_ISUNDEF(stream_id_retval)) {
        zval_ptr_dtor(&stream_id_retval);
      }
      return FAILURE;
    }

    if (Z_TYPE(stream_id_retval) != IS_LONG) {
      zval_ptr_dtor(&retval);
      zval_ptr_dtor(&stream_id_retval);
      zend_throw_exception(php_http3_native_exception_ce,
                           "QUIC stream getId() must return int", 0);
      return FAILURE;
    }

    *stream_id = (int64_t)Z_LVAL(stream_id_retval);
    php_http3_connection_cache_quic_stream(connection, *stream_id, &retval);
    zval_ptr_dtor(&retval);
    zval_ptr_dtor(&stream_id_retval);
    if (*stream_id >= connection->next_stream_id) {
      connection->next_stream_id = *stream_id + 4;
    }
    return SUCCESS;
  }

  ZVAL_UNDEF(&retval);
  if (php_http3_connection_call_fake_method(connection, "openBidirectionalStream", 0,
                                            NULL, &retval) != SUCCESS) {
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  if (Z_TYPE(retval) != IS_LONG) {
    zval_ptr_dtor(&retval);
    zend_throw_exception(php_http3_native_exception_ce,
                         "Fake adapter openBidirectionalStream() must return int", 0);
    return FAILURE;
  }

  *stream_id = (int64_t)Z_LVAL(retval);
  zval_ptr_dtor(&retval);
  return SUCCESS;
}

int php_http3_connection_submit_request_headers(php_http3_connection *connection,
                                                int64_t stream_id, zval *headers) {
  nghttp3_nv *nva = NULL;
  zend_string **name_refs = NULL;
  zend_string **value_refs = NULL;
  size_t expected;
  size_t idx = 0;
  zval *pair;
  int rv;
  static nghttp3_data_reader dr = { php_http3_h3_read_data_cb };

  if (!connection->native_h3_enabled || connection->h3_conn == NULL) {
    zend_throw_exception(php_http3_state_exception_ce,
                         "Native nghttp3 context is not available", 0);
    return FAILURE;
  }

  if (php_http3_connection_bind_native_h3_streams(connection) != SUCCESS) {
    return FAILURE;
  }

  expected = zend_hash_num_elements(Z_ARRVAL_P(headers));
  if (expected == 0) {
    zend_throw_exception(php_http3_invalid_operation_ce, "headers must not be empty", 0);
    return FAILURE;
  }

  nva = ecalloc(expected, sizeof(*nva));
  name_refs = ecalloc(expected, sizeof(*name_refs));
  value_refs = ecalloc(expected, sizeof(*value_refs));

  ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(headers), pair) {
    zval *name_zv;
    zval *value_zv;

    if (Z_TYPE_P(pair) != IS_ARRAY) {
      continue;
    }

    name_zv = zend_hash_index_find(Z_ARRVAL_P(pair), 0);
    value_zv = zend_hash_index_find(Z_ARRVAL_P(pair), 1);
    if (name_zv == NULL || value_zv == NULL) {
      continue;
    }

    name_refs[idx] = zval_get_string(name_zv);
    value_refs[idx] = zval_get_string(value_zv);
    nva[idx].name = (const uint8_t *)ZSTR_VAL(name_refs[idx]);
    nva[idx].value = (const uint8_t *)ZSTR_VAL(value_refs[idx]);
    nva[idx].namelen = ZSTR_LEN(name_refs[idx]);
    nva[idx].valuelen = ZSTR_LEN(value_refs[idx]);
    nva[idx].flags = NGHTTP3_NV_FLAG_NONE;
    ++idx;
  } ZEND_HASH_FOREACH_END();

  if (idx == 0) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "headers must be [[name, value], ...]", 0);
    rv = FAILURE;
    goto cleanup;
  }

  (void)php_http3_connection_get_or_create_native_body(connection, stream_id, 1);
  rv = nghttp3_conn_submit_request(connection->h3_conn, stream_id, nva, idx, &dr, NULL);
  if (rv != 0) {
    php_http3_connection_throw_nghttp3_error("nghttp3_conn_submit_request failed", rv);
    rv = FAILURE;
    goto cleanup;
  }

  rv = php_http3_connection_flush_h3_writes(connection);

cleanup:
  for (idx = 0; idx < expected; ++idx) {
    if (name_refs[idx] != NULL) {
      zend_string_release(name_refs[idx]);
    }
    if (value_refs[idx] != NULL) {
      zend_string_release(value_refs[idx]);
    }
  }
  efree(name_refs);
  efree(value_refs);
  efree(nva);

  return rv;
}

int php_http3_connection_write_stream(php_http3_connection *connection, int64_t stream_id,
                                      zend_string *data) {
  zval params[2];
  zval retval;
  zval stream_object;
  int rv;

  if (!connection->use_fake_adapter && connection->native_h3_enabled &&
      connection->h3_conn != NULL) {
    if (php_http3_connection_append_native_body(connection, stream_id, data) != SUCCESS) {
      return FAILURE;
    }
    rv = nghttp3_conn_resume_stream(connection->h3_conn, stream_id);
    if (rv != 0) {
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_resume_stream failed", rv);
      return FAILURE;
    }

    return php_http3_connection_flush_h3_writes(connection);
  }

  if (!connection->use_fake_adapter) {
    ZVAL_UNDEF(&stream_object);
    if (php_http3_connection_get_quic_stream(connection, stream_id, &stream_object) != SUCCESS) {
      return FAILURE;
    }

    ZVAL_STR_COPY(&params[0], data);
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(&stream_object, "write", 1, params, &retval,
                                        "Failed to call QUIC stream write()") != SUCCESS) {
      zval_ptr_dtor(&params[0]);
      zval_ptr_dtor(&stream_object);
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }

    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&stream_object);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return SUCCESS;
  }

  ZVAL_LONG(&params[0], stream_id);
  ZVAL_STR_COPY(&params[1], data);
  ZVAL_UNDEF(&retval);

  if (php_http3_connection_call_fake_method(connection, "writeStream", 2, params,
                                            &retval) != SUCCESS) {
    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&params[1]);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  zval_ptr_dtor(&params[0]);
  zval_ptr_dtor(&params[1]);
  if (!Z_ISUNDEF(retval)) {
    zval_ptr_dtor(&retval);
  }
  return SUCCESS;
}

int php_http3_connection_finish_stream(php_http3_connection *connection, int64_t stream_id) {
  zval params[1];
  zval retval;
  zval stream_object;
  int rv;

  if (!connection->use_fake_adapter && connection->native_h3_enabled &&
      connection->h3_conn != NULL) {
    if (php_http3_connection_set_native_body_eof(connection, stream_id) != SUCCESS) {
      return FAILURE;
    }
    rv = nghttp3_conn_resume_stream(connection->h3_conn, stream_id);
    if (rv != 0) {
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_resume_stream failed", rv);
      return FAILURE;
    }
    return php_http3_connection_flush_h3_writes(connection);
  }

  if (!connection->use_fake_adapter) {
    ZVAL_UNDEF(&stream_object);
    if (php_http3_connection_get_quic_stream(connection, stream_id, &stream_object) != SUCCESS) {
      return FAILURE;
    }

    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(&stream_object, "end", 0, NULL, &retval,
                                        "Failed to call QUIC stream end()") != SUCCESS) {
      zval_ptr_dtor(&stream_object);
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }

    zval_ptr_dtor(&stream_object);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return SUCCESS;
  }

  ZVAL_LONG(&params[0], stream_id);
  ZVAL_UNDEF(&retval);

  if (php_http3_connection_call_fake_method(connection, "finishStream", 1, params,
                                            &retval) != SUCCESS) {
    zval_ptr_dtor(&params[0]);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  zval_ptr_dtor(&params[0]);
  if (!Z_ISUNDEF(retval)) {
    zval_ptr_dtor(&retval);
  }
  return SUCCESS;
}

int php_http3_connection_reset_stream(php_http3_connection *connection, int64_t stream_id,
                                      uint64_t error_code) {
  zval params[2];
  zval retval;
  zval stream_object;
  int rv;

  if (!connection->use_fake_adapter && connection->native_h3_enabled &&
      connection->h3_conn != NULL) {
    rv = nghttp3_conn_close_stream(connection->h3_conn, stream_id, error_code);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
      php_http3_connection_throw_nghttp3_error("nghttp3_conn_close_stream failed", rv);
      return FAILURE;
    }
  }

  if (!connection->use_fake_adapter) {
    ZVAL_UNDEF(&stream_object);
    if (php_http3_connection_get_quic_stream(connection, stream_id, &stream_object) != SUCCESS) {
      return FAILURE;
    }

    ZVAL_LONG(&params[0], (zend_long)error_code);
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(&stream_object, "reset", 1, params, &retval,
                                        "Failed to call QUIC stream reset()") != SUCCESS) {
      zval_ptr_dtor(&params[0]);
      zval_ptr_dtor(&stream_object);
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      return FAILURE;
    }

    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&stream_object);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return SUCCESS;
  }

  ZVAL_LONG(&params[0], stream_id);
  ZVAL_LONG(&params[1], (zend_long)error_code);
  ZVAL_UNDEF(&retval);

  if (php_http3_connection_call_fake_method(connection, "resetStream", 2, params,
                                            &retval) != SUCCESS) {
    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&params[1]);
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
    return FAILURE;
  }

  zval_ptr_dtor(&params[0]);
  zval_ptr_dtor(&params[1]);
  if (!Z_ISUNDEF(retval)) {
    zval_ptr_dtor(&retval);
  }
  return SUCCESS;
}

static zend_bool php_http3_is_native_ngtcp2_connection(zval *quic) {
  if (quic == NULL || Z_TYPE_P(quic) != IS_OBJECT) {
    return 0;
  }

  return zend_string_equals_literal(Z_OBJCE_P(quic)->name, "Varion\\Ngtcp2\\Connection");
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_http3_connection_construct, 0, 0, 1)
  ZEND_ARG_OBJ_INFO(0, quic, Varion\\Ngtcp2\\QuicConnection, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_http3_connection_create_request_stream, 0, 0,
                                       Varion\\Nghttp3\\Http3RequestStream, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_connection_poll_events, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_connection_is_closing, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_connection_close, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Nghttp3_Http3Connection, __construct) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);
  zval *quic = NULL;

  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT(quic)
  ZEND_PARSE_PARAMETERS_END();

  if (!Z_ISUNDEF(connection->quic_connection)) {
    zval_ptr_dtor(&connection->quic_connection);
  }
  if (!Z_ISUNDEF(connection->fake_adapter)) {
    zval_ptr_dtor(&connection->fake_adapter);
    ZVAL_UNDEF(&connection->fake_adapter);
  }
  if (!Z_ISUNDEF(connection->event_queue)) {
    zval_ptr_dtor(&connection->event_queue);
  }
  array_init(&connection->event_queue);
  zend_hash_clean(&connection->request_streams);
  zend_hash_clean(&connection->quic_streams);
  zend_hash_clean(&connection->stream_states);
  zend_hash_clean(&connection->native_stream_bodies);
  if (connection->h3_conn != NULL) {
    nghttp3_conn_del(connection->h3_conn);
    connection->h3_conn = NULL;
  }

  ZVAL_COPY(&connection->quic_connection, quic);
  connection->use_fake_adapter = 0;
  connection->native_h3_enabled = 0;
  connection->native_h3_streams_bound = 0;
  connection->next_stream_id = 0;
  connection->closing = 0;
  connection->close_called = 0;
  connection->state = PHP_HTTP3_CONN_INITIAL;

  if (php_http3_is_native_ngtcp2_connection(quic)) {
    if (php_http3_connection_init_native_h3(connection) != SUCCESS) {
      RETURN_THROWS();
    }
  }
}

PHP_METHOD(Nghttp3_Http3Connection, createRequestStream) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);
  int64_t stream_id;
  zval stream_ref;
  zval state;

  if (connection->state == PHP_HTTP3_CONN_GOAWAY_RECEIVED) {
    zend_throw_exception(php_http3_state_exception_ce,
                         "createRequestStream() is not allowed after GOAWAY", 0);
    RETURN_THROWS();
  }
  if (connection->state == PHP_HTTP3_CONN_CLOSING ||
      connection->state == PHP_HTTP3_CONN_CLOSED ||
      connection->closing) {
    zend_throw_exception(php_http3_state_exception_ce,
                         "createRequestStream() is not allowed while closing", 0);
    RETURN_THROWS();
  }

  if (php_http3_connection_open_request_stream_id(connection, &stream_id) != SUCCESS) {
    if (!EG(exception)) {
      zend_throw_exception(php_http3_native_exception_ce,
                           "Failed to allocate request stream", 0);
    }
    RETURN_THROWS();
  }

  connection->state = PHP_HTTP3_CONN_ACTIVE;

  php_http3_request_stream_create(return_value, ZEND_THIS, stream_id);

  ZVAL_COPY(&stream_ref, return_value);
  zend_hash_index_update(&connection->request_streams, stream_id, &stream_ref);

  php_http3_connection_init_stream_state(&state);
  zend_hash_index_update(&connection->stream_states, stream_id, &state);
}

PHP_METHOD(Nghttp3_Http3Connection, pollEvents) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);

  if (connection->use_fake_adapter) {
    if (php_http3_connection_pump_fake_signals(connection) != SUCCESS) {
      RETURN_THROWS();
    }
  } else {
    if (php_http3_connection_pump_ngtcp2_signals(connection) != SUCCESS) {
      RETURN_THROWS();
    }
  }

  RETVAL_COPY(&connection->event_queue);
  zval_ptr_dtor(&connection->event_queue);
  array_init(&connection->event_queue);
}

PHP_METHOD(Nghttp3_Http3Connection, isClosing) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);
  RETURN_BOOL(connection->closing);
}

PHP_METHOD(Nghttp3_Http3Connection, close) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);
  zval retval;

  if (connection->close_called) {
    return;
  }

  if (!connection->use_fake_adapter && !Z_ISUNDEF(connection->quic_connection)) {
    ZVAL_UNDEF(&retval);
    if (php_http3_call_method_on_object(&connection->quic_connection, "close", 0, NULL, &retval,
                                        "Failed to call QUIC close()") != SUCCESS) {
      if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
      }
      RETURN_THROWS();
    }
    if (!Z_ISUNDEF(retval)) {
      zval_ptr_dtor(&retval);
    }
  }

  connection->close_called = 1;
  connection->closing = 1;
  if (connection->state != PHP_HTTP3_CONN_CLOSED) {
    connection->state = PHP_HTTP3_CONN_CLOSING;
  }
}

static const zend_function_entry php_http3_connection_methods[] = {
  PHP_ME(Nghttp3_Http3Connection, __construct, arginfo_http3_connection_construct,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3Connection, createRequestStream,
         arginfo_http3_connection_create_request_stream, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3Connection, pollEvents, arginfo_http3_connection_poll_events,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3Connection, isClosing, arginfo_http3_connection_is_closing,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3Connection, close, arginfo_http3_connection_close, ZEND_ACC_PUBLIC)
  PHP_FE_END
};

static zend_object *php_http3_connection_create_object(zend_class_entry *ce) {
  php_http3_connection *connection;

  connection = zend_object_alloc(sizeof(*connection), ce);
  ZVAL_UNDEF(&connection->quic_connection);
  ZVAL_UNDEF(&connection->fake_adapter);
  array_init(&connection->event_queue);
  zend_hash_init(&connection->request_streams, 8, NULL, ZVAL_PTR_DTOR, 0);
  zend_hash_init(&connection->quic_streams, 8, NULL, ZVAL_PTR_DTOR, 0);
  zend_hash_init(&connection->stream_states, 8, NULL, ZVAL_PTR_DTOR, 0);
  zend_hash_init(&connection->native_stream_bodies, 8, NULL,
                 php_http3_native_stream_body_ptr_dtor, 0);
  connection->h3_conn = NULL;
  connection->state = PHP_HTTP3_CONN_INITIAL;
  connection->next_stream_id = 0;
  connection->use_fake_adapter = 0;
  connection->native_h3_enabled = 0;
  connection->native_h3_streams_bound = 0;
  connection->closing = 0;
  connection->close_called = 0;

  zend_object_std_init(&connection->std, ce);
  object_properties_init(&connection->std, ce);
  connection->std.handlers = &php_http3_connection_handlers;

  return &connection->std;
}

static void php_http3_connection_free_object(zend_object *object) {
  php_http3_connection *connection;

  connection = (php_http3_connection *)((char *)object -
                                        XtOffsetOf(php_http3_connection, std));
  if (!Z_ISUNDEF(connection->quic_connection)) {
    zval_ptr_dtor(&connection->quic_connection);
    ZVAL_UNDEF(&connection->quic_connection);
  }
  if (!Z_ISUNDEF(connection->fake_adapter)) {
    zval_ptr_dtor(&connection->fake_adapter);
    ZVAL_UNDEF(&connection->fake_adapter);
  }
  if (!Z_ISUNDEF(connection->event_queue)) {
    zval_ptr_dtor(&connection->event_queue);
    ZVAL_UNDEF(&connection->event_queue);
  }

  zend_hash_destroy(&connection->request_streams);
  zend_hash_destroy(&connection->quic_streams);
  zend_hash_destroy(&connection->stream_states);
  zend_hash_destroy(&connection->native_stream_bodies);
  if (connection->h3_conn != NULL) {
    nghttp3_conn_del(connection->h3_conn);
    connection->h3_conn = NULL;
  }
  zend_object_std_dtor(&connection->std);
}

int php_nghttp3_connection_init(INIT_FUNC_ARGS) {
  zend_class_entry ce;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "Http3Connection", php_http3_connection_methods);
  php_http3_connection_ce = zend_register_internal_class(&ce);
  php_http3_connection_ce->create_object = php_http3_connection_create_object;
  php_http3_connection_ce->ce_flags |= ZEND_ACC_FINAL;

  memcpy(&php_http3_connection_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  php_http3_connection_handlers.offset = XtOffsetOf(php_http3_connection, std);
  php_http3_connection_handlers.free_obj = php_http3_connection_free_object;

  return SUCCESS;
}
