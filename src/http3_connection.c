#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>

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

static int php_http3_call_method_on_object(zval *object, const char *method_name,
                                           uint32_t param_count, zval *params, zval *retval,
                                           const char *context_message);

static int php_http3_connection_call_fake_method(php_http3_connection *connection,
                                                 const char *method_name,
                                                 uint32_t param_count, zval *params,
                                                 zval *retval);

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

static void php_http3_connection_cache_quic_stream(php_http3_connection *connection,
                                                   int64_t stream_id, zval *stream_object) {
  zval copy;

  ZVAL_COPY(&copy, stream_object);
  zend_hash_index_update(&connection->quic_streams, stream_id, &copy);
}

static int php_http3_connection_get_quic_stream(php_http3_connection *connection,
                                                int64_t stream_id, zval *stream_object) {
  zval *cached;
  zval params[1];
  zval retval;

  cached = zend_hash_index_find(&connection->quic_streams, stream_id);
  if (cached != NULL && Z_TYPE_P(cached) == IS_OBJECT) {
    ZVAL_COPY(stream_object, cached);
    return SUCCESS;
  }

  if (Z_ISUNDEF(connection->quic_connection)) {
    zend_throw_exception(php_http3_state_exception_ce, "QUIC connection is not available", 0);
    return FAILURE;
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

  if (Z_TYPE(retval) != IS_OBJECT) {
    zval_ptr_dtor(&retval);
    zend_throw_exception(php_http3_state_exception_ce, "QUIC stream was not found", 0);
    return FAILURE;
  }

  php_http3_connection_cache_quic_stream(connection, stream_id, &retval);
  ZVAL_COPY(stream_object, &retval);
  zval_ptr_dtor(&retval);
  return SUCCESS;
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
  zval signal;

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

int php_http3_connection_write_stream(php_http3_connection *connection, int64_t stream_id,
                                      zend_string *data) {
  zval params[2];
  zval retval;
  zval stream_object;

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

  ZVAL_COPY(&connection->quic_connection, quic);
  connection->use_fake_adapter = 0;
  connection->next_stream_id = 0;
  connection->closing = 0;
  connection->close_called = 0;
  connection->state = PHP_HTTP3_CONN_INITIAL;
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
  connection->state = PHP_HTTP3_CONN_INITIAL;
  connection->next_stream_id = 0;
  connection->use_fake_adapter = 0;
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
