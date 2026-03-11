#ifndef PHP_NGHTTP3_CONNECTION_H
#define PHP_NGHTTP3_CONNECTION_H

#include "php.h"
#include "types.h"

extern zend_class_entry *php_http3_connection_ce;

int php_nghttp3_connection_init(INIT_FUNC_ARGS);
int php_http3_connection_open_request_stream_id(php_http3_connection *connection,
                                                int64_t *stream_id);
int php_http3_connection_write_stream(php_http3_connection *connection, int64_t stream_id,
                                      zend_string *data);
int php_http3_connection_finish_stream(php_http3_connection *connection, int64_t stream_id);
int php_http3_connection_reset_stream(php_http3_connection *connection, int64_t stream_id,
                                      uint64_t error_code);

#endif /* PHP_NGHTTP3_CONNECTION_H */
