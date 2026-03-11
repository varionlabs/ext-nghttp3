#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "internal/http3_connection.h"
#include "internal/macros.h"
#include "internal/testing.h"

typedef struct _php_http3_fake_quic_adapter {
  int64_t next_stream_id;
  zval writes;
  zval finished_streams;
  zval reset_calls;
  zval signals;
  zend_object std;
} php_http3_fake_quic_adapter;

#define Z_HTTP3_FAKE_ADAPTER_P(zv)                                              \
  ((php_http3_fake_quic_adapter *)((char *)Z_OBJ_P((zv)) -                      \
                                   XtOffsetOf(php_http3_fake_quic_adapter, std)))

static zend_object_handlers php_http3_fake_adapter_handlers;
static zend_class_entry *php_http3_fake_adapter_ce;
static zend_class_entry *php_http3_connection_factory_ce;

ZEND_BEGIN_ARG_INFO_EX(arginfo_http3_fake_construct, 0, 0, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, startStreamId, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_open_bidi_stream, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_write_stream, 0, 2, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
  ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_finish_stream, 0, 1, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_reset_stream, 0, 1, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_poll_signals, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_inject_readable, 0, 2, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
  ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fin, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_inject_reset, 0, 1, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, streamId, IS_LONG, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_inject_closing, 0, 0, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_inject_closed, 0, 0, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_get_writes, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_get_finished_streams, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_fake_get_reset_calls, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_http3_factory_from_fake, 0, 1,
                                       Varion\\Nghttp3\\Http3Connection, 0)
  ZEND_ARG_OBJ_INFO(0, fake, Varion\\Nghttp3\\Testing\\FakeQuicAdapter, 0)
ZEND_END_ARG_INFO()

static void php_http3_fake_adapter_push_signal(php_http3_fake_quic_adapter *adapter,
                                               php_http3_signal_type type,
                                               int64_t stream_id, uint64_t error_code,
                                               const char *data, size_t data_len,
                                               zend_bool fin, zend_bool closed) {
  zval signal;

  array_init(&signal);
  add_assoc_long(&signal, "type", (zend_long)type);
  if (stream_id >= 0) {
    add_assoc_long(&signal, "streamId", (zend_long)stream_id);
  }
  if (data != NULL) {
    add_assoc_stringl(&signal, "data", data, data_len);
  }
  if (type == PHP_HTTP3_SIGNAL_STREAM_READABLE) {
    add_assoc_bool(&signal, "fin", fin);
  }
  if (type == PHP_HTTP3_SIGNAL_STREAM_RESET ||
      type == PHP_HTTP3_SIGNAL_CONNECTION_CLOSING) {
    add_assoc_long(&signal, "errorCode", (zend_long)error_code);
    if (closed) {
      add_assoc_bool(&signal, "closed", 1);
    }
  }

  add_next_index_zval(&adapter->signals, &signal);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, __construct) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long start_stream_id = 0;

  ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(start_stream_id)
  ZEND_PARSE_PARAMETERS_END();

  adapter->next_stream_id = (int64_t)start_stream_id;
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, openBidirectionalStream) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  int64_t stream_id = adapter->next_stream_id;

  adapter->next_stream_id += 4;
  RETURN_LONG((zend_long)stream_id);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, writeStream) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long stream_id;
  zend_string *data;
  zval write;

  ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_LONG(stream_id)
    Z_PARAM_STR(data)
  ZEND_PARSE_PARAMETERS_END();

  array_init(&write);
  add_assoc_long(&write, "streamId", stream_id);
  add_assoc_str(&write, "data", zend_string_copy(data));
  add_next_index_zval(&adapter->writes, &write);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, finishStream) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long stream_id;

  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_LONG(stream_id)
  ZEND_PARSE_PARAMETERS_END();

  add_next_index_long(&adapter->finished_streams, stream_id);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, resetStream) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long stream_id;
  zend_long error_code = 0;
  zval call;

  ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_LONG(stream_id)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(error_code)
  ZEND_PARSE_PARAMETERS_END();

  array_init(&call);
  add_assoc_long(&call, "streamId", stream_id);
  add_assoc_long(&call, "errorCode", error_code);
  add_next_index_zval(&adapter->reset_calls, &call);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, pollSignals) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);

  RETVAL_COPY(&adapter->signals);
  zval_ptr_dtor(&adapter->signals);
  array_init(&adapter->signals);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, injectReadable) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long stream_id;
  char *data = NULL;
  size_t data_len = 0;
  zend_bool fin = 0;

  ZEND_PARSE_PARAMETERS_START(2, 3)
    Z_PARAM_LONG(stream_id)
    Z_PARAM_STRING(data, data_len)
    Z_PARAM_OPTIONAL
    Z_PARAM_BOOL(fin)
  ZEND_PARSE_PARAMETERS_END();

  php_http3_fake_adapter_push_signal(adapter, PHP_HTTP3_SIGNAL_STREAM_READABLE, stream_id, 0,
                                     data, data_len, fin, 0);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, injectReset) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long stream_id;
  zend_long error_code = 0;

  ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_LONG(stream_id)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(error_code)
  ZEND_PARSE_PARAMETERS_END();

  php_http3_fake_adapter_push_signal(adapter, PHP_HTTP3_SIGNAL_STREAM_RESET, stream_id,
                                     (uint64_t)error_code, NULL, 0, 0, 0);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, injectConnectionClosing) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long error_code = 0;

  ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(error_code)
  ZEND_PARSE_PARAMETERS_END();

  php_http3_fake_adapter_push_signal(adapter, PHP_HTTP3_SIGNAL_CONNECTION_CLOSING, -1,
                                     (uint64_t)error_code, NULL, 0, 0, 0);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, injectConnectionClosed) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  zend_long error_code = 0;

  ZEND_PARSE_PARAMETERS_START(0, 1)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(error_code)
  ZEND_PARSE_PARAMETERS_END();

  php_http3_fake_adapter_push_signal(adapter, PHP_HTTP3_SIGNAL_CONNECTION_CLOSING, -1,
                                     (uint64_t)error_code, NULL, 0, 0, 1);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, getWrites) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  RETURN_COPY(&adapter->writes);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, getFinishedStreams) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  RETURN_COPY(&adapter->finished_streams);
}

PHP_METHOD(Nghttp3_Testing_FakeQuicAdapter, getResetCalls) {
  php_http3_fake_quic_adapter *adapter = Z_HTTP3_FAKE_ADAPTER_P(ZEND_THIS);
  RETURN_COPY(&adapter->reset_calls);
}

PHP_METHOD(Nghttp3_Testing_Http3ConnectionFactory, fromFake) {
  php_http3_connection *connection;
  zval *fake;

  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(fake, php_http3_fake_adapter_ce)
  ZEND_PARSE_PARAMETERS_END();

  object_init_ex(return_value, php_http3_connection_ce);
  connection = Z_HTTP3_CONNECTION_P(return_value);
  ZVAL_COPY(&connection->fake_adapter, fake);
  connection->use_fake_adapter = 1;
  connection->native_h3_enabled = 0;
  connection->native_h3_streams_bound = 0;
  connection->h3_conn = NULL;
  connection->closing = 0;
  connection->close_called = 0;
  connection->state = PHP_HTTP3_CONN_INITIAL;
}

static const zend_function_entry php_http3_fake_adapter_methods[] = {
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, __construct, arginfo_http3_fake_construct,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, openBidirectionalStream,
         arginfo_http3_fake_open_bidi_stream, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, writeStream, arginfo_http3_fake_write_stream,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, finishStream, arginfo_http3_fake_finish_stream,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, resetStream, arginfo_http3_fake_reset_stream,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, pollSignals, arginfo_http3_fake_poll_signals,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, injectReadable, arginfo_http3_fake_inject_readable,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, injectReset, arginfo_http3_fake_inject_reset,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, injectConnectionClosing,
         arginfo_http3_fake_inject_closing, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, injectConnectionClosed,
         arginfo_http3_fake_inject_closed, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, getWrites, arginfo_http3_fake_get_writes,
         ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, getFinishedStreams,
         arginfo_http3_fake_get_finished_streams, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Testing_FakeQuicAdapter, getResetCalls, arginfo_http3_fake_get_reset_calls,
         ZEND_ACC_PUBLIC)
  PHP_FE_END
};

static const zend_function_entry php_http3_connection_factory_methods[] = {
  PHP_ME(Nghttp3_Testing_Http3ConnectionFactory, fromFake, arginfo_http3_factory_from_fake,
         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
  PHP_FE_END
};

static zend_object *php_http3_fake_adapter_create_object(zend_class_entry *ce) {
  php_http3_fake_quic_adapter *adapter;

  adapter = zend_object_alloc(sizeof(*adapter), ce);
  adapter->next_stream_id = 0;
  array_init(&adapter->writes);
  array_init(&adapter->finished_streams);
  array_init(&adapter->reset_calls);
  array_init(&adapter->signals);

  zend_object_std_init(&adapter->std, ce);
  object_properties_init(&adapter->std, ce);
  adapter->std.handlers = &php_http3_fake_adapter_handlers;

  return &adapter->std;
}

static void php_http3_fake_adapter_free_object(zend_object *object) {
  php_http3_fake_quic_adapter *adapter;

  adapter = (php_http3_fake_quic_adapter *)((char *)object -
                                            XtOffsetOf(php_http3_fake_quic_adapter, std));
  zval_ptr_dtor(&adapter->writes);
  zval_ptr_dtor(&adapter->finished_streams);
  zval_ptr_dtor(&adapter->reset_calls);
  zval_ptr_dtor(&adapter->signals);

  zend_object_std_dtor(&adapter->std);
}

int php_nghttp3_testing_init(INIT_FUNC_ARGS) {
  zend_class_entry ce;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Testing", "FakeQuicAdapter",
                      php_http3_fake_adapter_methods);
  php_http3_fake_adapter_ce = zend_register_internal_class(&ce);
  php_http3_fake_adapter_ce->create_object = php_http3_fake_adapter_create_object;
  php_http3_fake_adapter_ce->ce_flags |= ZEND_ACC_FINAL;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Testing", "Http3ConnectionFactory",
                      php_http3_connection_factory_methods);
  php_http3_connection_factory_ce = zend_register_internal_class(&ce);
  php_http3_connection_factory_ce->ce_flags |= ZEND_ACC_FINAL;

  memcpy(&php_http3_fake_adapter_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  php_http3_fake_adapter_handlers.offset =
    XtOffsetOf(php_http3_fake_quic_adapter, std);
  php_http3_fake_adapter_handlers.free_obj = php_http3_fake_adapter_free_object;

  return SUCCESS;
}
