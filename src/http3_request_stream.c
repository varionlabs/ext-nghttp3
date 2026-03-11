#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>

#include "internal/exception.h"
#include "internal/http3_connection.h"
#include "internal/http3_request_stream.h"
#include "internal/macros.h"

zend_class_entry *php_http3_request_stream_ce;
static zend_object_handlers php_http3_request_stream_handlers;

ZEND_BEGIN_ARG_INFO_EX(arginfo_http3_request_stream_construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_request_stream_get_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_request_stream_submit_headers, 0, 1, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_request_stream_submit_data, 0, 1, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_request_stream_end, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_request_stream_reset, 0, 0, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_request_stream_is_closed, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static zend_string *php_http3_encode_headers_payload(zval *headers) {
  smart_str payload = {0};
  zval *pair;

  ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(headers), pair) {
    zval *name_zv;
    zval *value_zv;
    zend_string *name;
    zend_string *value;

    if (Z_TYPE_P(pair) != IS_ARRAY) {
      continue;
    }

    name_zv = zend_hash_index_find(Z_ARRVAL_P(pair), 0);
    value_zv = zend_hash_index_find(Z_ARRVAL_P(pair), 1);
    if (name_zv == NULL || value_zv == NULL) {
      continue;
    }

    name = zval_get_string(name_zv);
    value = zval_get_string(value_zv);

    smart_str_appendl(&payload, ZSTR_VAL(name), ZSTR_LEN(name));
    smart_str_appendc(&payload, ':');
    smart_str_appendl(&payload, ZSTR_VAL(value), ZSTR_LEN(value));
    smart_str_appendc(&payload, '\n');

    zend_string_release(name);
    zend_string_release(value);
  } ZEND_HASH_FOREACH_END();

  smart_str_0(&payload);
  if (payload.s == NULL) {
    return zend_string_init("", 0, 0);
  }

  return payload.s;
}

PHP_METHOD(Nghttp3_Http3RequestStream, __construct) {
  zend_throw_exception(php_http3_invalid_operation_ce,
                       "Http3RequestStream cannot be constructed directly", 0);
}

PHP_METHOD(Nghttp3_Http3RequestStream, getId) {
  php_http3_request_stream *stream = Z_HTTP3_REQUEST_STREAM_P(ZEND_THIS);
  RETURN_LONG((zend_long)stream->stream_id);
}

PHP_METHOD(Nghttp3_Http3RequestStream, submitHeaders) {
  php_http3_request_stream *stream = Z_HTTP3_REQUEST_STREAM_P(ZEND_THIS);
  php_http3_connection *connection;
  zval *headers;
  HashTable *ht;
  zval *pair;
  zend_string *payload;

  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ARRAY(headers)
  ZEND_PARSE_PARAMETERS_END();

  if (stream->closed) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "submitHeaders() is not allowed on a closed stream", 0);
    RETURN_THROWS();
  }
  if (stream->headers_submitted) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "submitHeaders() has already been called", 0);
    RETURN_THROWS();
  }

  ht = Z_ARRVAL_P(headers);
  ZEND_HASH_FOREACH_VAL(ht, pair) {
    if (Z_TYPE_P(pair) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(pair)) != 2) {
      zend_throw_exception(php_http3_invalid_operation_ce,
                           "headers must be [[name, value], ...]", 0);
      RETURN_THROWS();
    }
  } ZEND_HASH_FOREACH_END();

  connection = Z_HTTP3_CONNECTION_P(&stream->connection);
  if (!connection->use_fake_adapter && connection->native_h3_enabled) {
    if (php_http3_connection_submit_request_headers(connection, stream->stream_id, headers) !=
        SUCCESS) {
      RETURN_THROWS();
    }
  } else {
    payload = php_http3_encode_headers_payload(headers);
    if (php_http3_connection_write_stream(connection, stream->stream_id, payload) != SUCCESS) {
      zend_string_release(payload);
      RETURN_THROWS();
    }
    zend_string_release(payload);
  }

  stream->headers_submitted = 1;
}

PHP_METHOD(Nghttp3_Http3RequestStream, submitData) {
  php_http3_request_stream *stream = Z_HTTP3_REQUEST_STREAM_P(ZEND_THIS);
  php_http3_connection *connection;
  char *data = NULL;
  size_t data_len = 0;
  zend_string *payload;

  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(data, data_len)
  ZEND_PARSE_PARAMETERS_END();

  (void)data;
  (void)data_len;

  if (stream->closed) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "submitData() is not allowed on a closed stream", 0);
    RETURN_THROWS();
  }
  if (!stream->headers_submitted) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "submitData() requires submitHeaders() first", 0);
    RETURN_THROWS();
  }
  if (stream->local_ended) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "submitData() is not allowed after end()", 0);
    RETURN_THROWS();
  }

  connection = Z_HTTP3_CONNECTION_P(&stream->connection);
  payload = zend_string_init(data, data_len, 0);
  if (php_http3_connection_write_stream(connection, stream->stream_id, payload) != SUCCESS) {
    zend_string_release(payload);
    RETURN_THROWS();
  }
  zend_string_release(payload);
}

PHP_METHOD(Nghttp3_Http3RequestStream, end) {
  php_http3_request_stream *stream = Z_HTTP3_REQUEST_STREAM_P(ZEND_THIS);
  php_http3_connection *connection;

  if (stream->closed) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "end() is not allowed on a closed stream", 0);
    RETURN_THROWS();
  }
  if (!stream->headers_submitted) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "end() requires submitHeaders() first", 0);
    RETURN_THROWS();
  }
  if (stream->local_ended) {
    zend_throw_exception(php_http3_invalid_operation_ce,
                         "end() has already been called", 0);
    RETURN_THROWS();
  }

  connection = Z_HTTP3_CONNECTION_P(&stream->connection);
  if (php_http3_connection_finish_stream(connection, stream->stream_id) != SUCCESS) {
    RETURN_THROWS();
  }

  stream->local_ended = 1;
  stream->closed = 1;
}

PHP_METHOD(Nghttp3_Http3RequestStream, reset) {
  php_http3_request_stream *stream = Z_HTTP3_REQUEST_STREAM_P(ZEND_THIS);
  php_http3_connection *connection;
  zend_long error_code = 0;

  ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(error_code)
  ZEND_PARSE_PARAMETERS_END();

  connection = Z_HTTP3_CONNECTION_P(&stream->connection);
  if (php_http3_connection_reset_stream(connection, stream->stream_id,
                                        (uint64_t)error_code) != SUCCESS) {
    RETURN_THROWS();
  }

  stream->closed = 1;
}

PHP_METHOD(Nghttp3_Http3RequestStream, isClosed) {
  php_http3_request_stream *stream = Z_HTTP3_REQUEST_STREAM_P(ZEND_THIS);
  RETURN_BOOL(stream->closed);
}

static const zend_function_entry php_http3_request_stream_methods[] = {
  PHP_ME(Nghttp3_Http3RequestStream, __construct, arginfo_http3_request_stream_construct,
         ZEND_ACC_PRIVATE)
  PHP_ME(Nghttp3_Http3RequestStream, getId, arginfo_http3_request_stream_get_id,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3RequestStream, submitHeaders, arginfo_http3_request_stream_submit_headers,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3RequestStream, submitData, arginfo_http3_request_stream_submit_data,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3RequestStream, end, arginfo_http3_request_stream_end, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3RequestStream, reset, arginfo_http3_request_stream_reset, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Http3RequestStream, isClosed, arginfo_http3_request_stream_is_closed,
         ZEND_ACC_PUBLIC)
  PHP_FE_END
};

static zend_object *php_http3_request_stream_create_object(zend_class_entry *ce) {
  php_http3_request_stream *stream;

  stream = zend_object_alloc(sizeof(*stream), ce);
  ZVAL_UNDEF(&stream->connection);
  stream->stream_id = -1;
  stream->closed = 0;
  stream->headers_submitted = 0;
  stream->local_ended = 0;

  zend_object_std_init(&stream->std, ce);
  object_properties_init(&stream->std, ce);
  stream->std.handlers = &php_http3_request_stream_handlers;

  return &stream->std;
}

static void php_http3_request_stream_free_object(zend_object *object) {
  php_http3_request_stream *stream;

  stream = (php_http3_request_stream *)((char *)object -
                                        XtOffsetOf(php_http3_request_stream, std));
  if (!Z_ISUNDEF(stream->connection)) {
    zval_ptr_dtor(&stream->connection);
    ZVAL_UNDEF(&stream->connection);
  }

  zend_object_std_dtor(&stream->std);
}

void php_http3_request_stream_create(zval *return_value, zval *connection, int64_t stream_id) {
  php_http3_request_stream *stream;

  object_init_ex(return_value, php_http3_request_stream_ce);
  stream = Z_HTTP3_REQUEST_STREAM_P(return_value);
  ZVAL_COPY(&stream->connection, connection);
  stream->stream_id = stream_id;
}

int php_nghttp3_request_stream_init(INIT_FUNC_ARGS) {
  zend_class_entry ce;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "Http3RequestStream",
                      php_http3_request_stream_methods);
  php_http3_request_stream_ce = zend_register_internal_class(&ce);
  php_http3_request_stream_ce->create_object = php_http3_request_stream_create_object;
  php_http3_request_stream_ce->ce_flags |= ZEND_ACC_FINAL;

  memcpy(&php_http3_request_stream_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  php_http3_request_stream_handlers.offset = XtOffsetOf(php_http3_request_stream, std);
  php_http3_request_stream_handlers.free_obj = php_http3_request_stream_free_object;

  return SUCCESS;
}
