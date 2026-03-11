#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "internal/event.h"
#include "internal/macros.h"

zend_class_entry *php_http3_event_ce;
zend_class_entry *php_http3_connection_event_ce;
zend_class_entry *php_http3_stream_event_ce;
zend_class_entry *php_http3_terminal_stream_event_ce;
zend_class_entry *php_http3_headers_received_ce;
zend_class_entry *php_http3_data_received_ce;
zend_class_entry *php_http3_request_completed_ce;
zend_class_entry *php_http3_stream_reset_ce;
zend_class_entry *php_http3_goaway_received_ce;

static zend_object_handlers php_http3_event_handlers;

ZEND_BEGIN_ARG_INFO_EX(arginfo_http3_event_construct, 0, 0, 1)
  ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, streamId, IS_LONG, 0, "-1")
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, errorCode, IS_LONG, 0, "0")
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, payload, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_event_get_type, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_event_get_stream_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_event_get_error_code, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_http3_event_get_payload, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Nghttp3_Event, __construct) {
  php_http3_event_object *event_obj;
  zend_long type;
  zend_long stream_id = -1;
  zend_long error_code = 0;
  zend_string *payload = NULL;

  ZEND_PARSE_PARAMETERS_START(1, 4)
    Z_PARAM_LONG(type)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(stream_id)
    Z_PARAM_LONG(error_code)
    Z_PARAM_STR(payload)
  ZEND_PARSE_PARAMETERS_END();

  event_obj = Z_HTTP3_EVENT_OBJ_P(ZEND_THIS);
  event_obj->type = (php_http3_event_type)type;
  event_obj->stream_id = stream_id;
  event_obj->error_code = (uint64_t)error_code;

  if (event_obj->payload != NULL) {
    zend_string_release(event_obj->payload);
  }
  if (payload != NULL) {
    event_obj->payload = zend_string_copy(payload);
  } else {
    event_obj->payload = zend_string_init("", 0, 0);
  }
}

PHP_METHOD(Nghttp3_Event, getType) {
  php_http3_event_object *event_obj = Z_HTTP3_EVENT_OBJ_P(ZEND_THIS);
  RETURN_LONG((zend_long)event_obj->type);
}

PHP_METHOD(Nghttp3_Event, getStreamId) {
  php_http3_event_object *event_obj = Z_HTTP3_EVENT_OBJ_P(ZEND_THIS);
  RETURN_LONG((zend_long)event_obj->stream_id);
}

PHP_METHOD(Nghttp3_Event, getErrorCode) {
  php_http3_event_object *event_obj = Z_HTTP3_EVENT_OBJ_P(ZEND_THIS);
  RETURN_LONG((zend_long)event_obj->error_code);
}

PHP_METHOD(Nghttp3_Event, getPayload) {
  php_http3_event_object *event_obj = Z_HTTP3_EVENT_OBJ_P(ZEND_THIS);
  RETURN_STR_COPY(event_obj->payload);
}

static const zend_function_entry php_http3_event_methods[] = {
  PHP_ME(Nghttp3_Event, __construct, arginfo_http3_event_construct, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Event, getType, arginfo_http3_event_get_type, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Event, getStreamId, arginfo_http3_event_get_stream_id, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Event, getErrorCode, arginfo_http3_event_get_error_code, ZEND_ACC_PUBLIC)
  PHP_ME(Nghttp3_Event, getPayload, arginfo_http3_event_get_payload, ZEND_ACC_PUBLIC)
  PHP_FE_END
};

static zend_object *php_http3_event_create_object(zend_class_entry *ce) {
  php_http3_event_object *event_obj;

  event_obj = zend_object_alloc(sizeof(*event_obj), ce);
  event_obj->type = PHP_HTTP3_EVENT_HEADERS_RECEIVED;
  event_obj->stream_id = -1;
  event_obj->error_code = 0;
  event_obj->payload = zend_string_init("", 0, 0);

  zend_object_std_init(&event_obj->std, ce);
  object_properties_init(&event_obj->std, ce);
  event_obj->std.handlers = &php_http3_event_handlers;

  return &event_obj->std;
}

static void php_http3_event_free_object(zend_object *object) {
  php_http3_event_object *event_obj;

  event_obj = (php_http3_event_object *)((char *)object -
                                         XtOffsetOf(php_http3_event_object, std));
  if (event_obj->payload != NULL) {
    zend_string_release(event_obj->payload);
    event_obj->payload = NULL;
  }

  zend_object_std_dtor(&event_obj->std);
}

zend_class_entry *php_http3_event_class_for_type(php_http3_event_type type) {
  switch (type) {
  case PHP_HTTP3_EVENT_HEADERS_RECEIVED:
    return php_http3_headers_received_ce;
  case PHP_HTTP3_EVENT_DATA_RECEIVED:
    return php_http3_data_received_ce;
  case PHP_HTTP3_EVENT_REQUEST_COMPLETED:
    return php_http3_request_completed_ce;
  case PHP_HTTP3_EVENT_STREAM_RESET:
    return php_http3_stream_reset_ce;
  case PHP_HTTP3_EVENT_GOAWAY_RECEIVED:
  default:
    return php_http3_goaway_received_ce;
  }
}

int php_nghttp3_event_init(INIT_FUNC_ARGS) {
  zend_class_entry ce;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "Event", php_http3_event_methods);
  php_http3_event_ce = zend_register_internal_class(&ce);
  php_http3_event_ce->create_object = php_http3_event_create_object;
  php_http3_event_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "ConnectionEvent", NULL);
  php_http3_connection_event_ce =
    zend_register_internal_class_ex(&ce, php_http3_event_ce);
  php_http3_connection_event_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "StreamEvent", NULL);
  php_http3_stream_event_ce =
    zend_register_internal_class_ex(&ce, php_http3_event_ce);
  php_http3_stream_event_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "TerminalStreamEvent", NULL);
  php_http3_terminal_stream_event_ce =
    zend_register_internal_class_ex(&ce, php_http3_stream_event_ce);
  php_http3_terminal_stream_event_ce->ce_flags |= ZEND_ACC_EXPLICIT_ABSTRACT_CLASS;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Events", "HeadersReceived", NULL);
  php_http3_headers_received_ce =
    zend_register_internal_class_ex(&ce, php_http3_stream_event_ce);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Events", "DataReceived", NULL);
  php_http3_data_received_ce =
    zend_register_internal_class_ex(&ce, php_http3_stream_event_ce);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Events", "RequestCompleted", NULL);
  php_http3_request_completed_ce =
    zend_register_internal_class_ex(&ce, php_http3_terminal_stream_event_ce);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Events", "StreamReset", NULL);
  php_http3_stream_reset_ce =
    zend_register_internal_class_ex(&ce, php_http3_terminal_stream_event_ce);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3\\Events", "GoawayReceived", NULL);
  php_http3_goaway_received_ce =
    zend_register_internal_class_ex(&ce, php_http3_connection_event_ce);

  memcpy(&php_http3_event_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  php_http3_event_handlers.offset = XtOffsetOf(php_http3_event_object, std);
  php_http3_event_handlers.free_obj = php_http3_event_free_object;

  zend_declare_class_constant_long(php_http3_event_ce, ZEND_STRL("HEADERS_RECEIVED"),
                                   PHP_HTTP3_EVENT_HEADERS_RECEIVED);
  zend_declare_class_constant_long(php_http3_event_ce, ZEND_STRL("DATA_RECEIVED"),
                                   PHP_HTTP3_EVENT_DATA_RECEIVED);
  zend_declare_class_constant_long(php_http3_event_ce, ZEND_STRL("REQUEST_COMPLETED"),
                                   PHP_HTTP3_EVENT_REQUEST_COMPLETED);
  zend_declare_class_constant_long(php_http3_event_ce, ZEND_STRL("STREAM_RESET"),
                                   PHP_HTTP3_EVENT_STREAM_RESET);
  zend_declare_class_constant_long(php_http3_event_ce, ZEND_STRL("GOAWAY_RECEIVED"),
                                   PHP_HTTP3_EVENT_GOAWAY_RECEIVED);

  return SUCCESS;
}
