#ifndef PHP_NGHTTP3_TYPES_H
#define PHP_NGHTTP3_TYPES_H

#include "php.h"
#include <stdint.h>

typedef enum _php_http3_connection_state {
  PHP_HTTP3_CONN_INITIAL = 0,
  PHP_HTTP3_CONN_ACTIVE = 1,
  PHP_HTTP3_CONN_GOAWAY_RECEIVED = 2,
  PHP_HTTP3_CONN_CLOSING = 3,
  PHP_HTTP3_CONN_CLOSED = 4
} php_http3_connection_state;

typedef enum _php_http3_event_type {
  PHP_HTTP3_EVENT_HEADERS_RECEIVED = 1,
  PHP_HTTP3_EVENT_DATA_RECEIVED = 2,
  PHP_HTTP3_EVENT_REQUEST_COMPLETED = 3,
  PHP_HTTP3_EVENT_STREAM_RESET = 4,
  PHP_HTTP3_EVENT_GOAWAY_RECEIVED = 5
} php_http3_event_type;

typedef enum _php_http3_signal_type {
  PHP_HTTP3_SIGNAL_STREAM_READABLE = 1,
  PHP_HTTP3_SIGNAL_STREAM_RESET = 2,
  PHP_HTTP3_SIGNAL_CONNECTION_CLOSING = 3
} php_http3_signal_type;

typedef struct _php_http3_signal {
  php_http3_signal_type type;
  int64_t stream_id;
  uint64_t error_code;
  zend_string *data;
  zend_bool fin;
} php_http3_signal;

typedef struct _php_http3_request_stream_state {
  zend_bool request_headers_submitted;
  zend_bool local_ended;
  zend_bool remote_headers_received;
  zend_bool remote_ended;
  zend_bool terminal;
  zend_bool reset;
  uint64_t reset_error_code;
} php_http3_request_stream_state;

typedef struct _php_http3_connection {
  zval quic_connection;
  zval fake_adapter;
  zval event_queue;
  HashTable request_streams;
  HashTable stream_states;
  php_http3_connection_state state;
  int64_t next_stream_id;
  zend_bool use_fake_adapter;
  zend_bool closing;
  zend_object std;
} php_http3_connection;

typedef struct _php_http3_request_stream {
  zval connection;
  int64_t stream_id;
  zend_bool closed;
  zend_bool headers_submitted;
  zend_bool local_ended;
  zend_object std;
} php_http3_request_stream;

typedef struct _php_http3_event_object {
  php_http3_event_type type;
  int64_t stream_id;
  uint64_t error_code;
  zend_string *payload;
  zend_object std;
} php_http3_event_object;

#endif /* PHP_NGHTTP3_TYPES_H */
