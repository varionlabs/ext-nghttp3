#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <Zend/zend_exceptions.h>

#include "internal/exception.h"
#include "internal/http3_connection.h"
#include "internal/http3_request_stream.h"
#include "internal/macros.h"

zend_class_entry *php_http3_connection_ce;
static zend_object_handlers php_http3_connection_handlers;

static int php_http3_connection_call_fake_method(php_http3_connection *connection,
                                                 const char *method_name,
                                                 uint32_t param_count, zval *params,
                                                 zval *retval) {
  zval function_name;

  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
    return FAILURE;
  }

  ZVAL_STRING(&function_name, method_name);
  if (call_user_function(NULL, &connection->fake_adapter, &function_name, retval,
                         param_count, params) == FAILURE) {
    zval_ptr_dtor(&function_name);
    zend_throw_exception(php_http3_native_exception_ce,
                         "Failed to call fake adapter method", 0);
    return FAILURE;
  }
  zval_ptr_dtor(&function_name);

  if (EG(exception)) {
    return FAILURE;
  }

  return SUCCESS;
}

int php_http3_connection_open_request_stream_id(php_http3_connection *connection,
                                                int64_t *stream_id) {
  zval retval;

  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
    *stream_id = connection->next_stream_id;
    connection->next_stream_id += 4;
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

  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
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

  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
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

  if (!connection->use_fake_adapter || Z_ISUNDEF(connection->fake_adapter)) {
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

  ZVAL_COPY(&connection->quic_connection, quic);
  connection->use_fake_adapter = 0;
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

  array_init(&state);
  add_assoc_bool(&state, "requestHeadersSubmitted", 0);
  add_assoc_bool(&state, "localEnded", 0);
  add_assoc_bool(&state, "remoteHeadersReceived", 0);
  add_assoc_bool(&state, "remoteEnded", 0);
  add_assoc_bool(&state, "terminal", 0);
  add_assoc_bool(&state, "reset", 0);
  add_assoc_null(&state, "resetErrorCode");
  zend_hash_index_update(&connection->stream_states, stream_id, &state);
}

PHP_METHOD(Nghttp3_Http3Connection, pollEvents) {
  array_init(return_value);
}

PHP_METHOD(Nghttp3_Http3Connection, isClosing) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);
  RETURN_BOOL(connection->closing);
}

PHP_METHOD(Nghttp3_Http3Connection, close) {
  php_http3_connection *connection = Z_HTTP3_CONNECTION_P(ZEND_THIS);
  connection->closing = 1;
  connection->state = PHP_HTTP3_CONN_CLOSING;
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
  zend_hash_init(&connection->request_streams, 8, NULL, ZVAL_PTR_DTOR, 0);
  zend_hash_init(&connection->stream_states, 8, NULL, ZVAL_PTR_DTOR, 0);
  connection->state = PHP_HTTP3_CONN_INITIAL;
  connection->next_stream_id = 0;
  connection->use_fake_adapter = 0;
  connection->closing = 0;

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

  zend_hash_destroy(&connection->request_streams);
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
