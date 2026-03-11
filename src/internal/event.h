#ifndef PHP_NGHTTP3_EVENT_H
#define PHP_NGHTTP3_EVENT_H

#include "php.h"
#include "types.h"

extern zend_class_entry *php_http3_event_ce;
extern zend_class_entry *php_http3_connection_event_ce;
extern zend_class_entry *php_http3_stream_event_ce;
extern zend_class_entry *php_http3_terminal_stream_event_ce;
extern zend_class_entry *php_http3_headers_received_ce;
extern zend_class_entry *php_http3_data_received_ce;
extern zend_class_entry *php_http3_request_completed_ce;
extern zend_class_entry *php_http3_stream_reset_ce;
extern zend_class_entry *php_http3_goaway_received_ce;

int php_nghttp3_event_init(INIT_FUNC_ARGS);
zend_class_entry *php_http3_event_class_for_type(php_http3_event_type type);

#endif /* PHP_NGHTTP3_EVENT_H */
