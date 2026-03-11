#ifndef PHP_NGHTTP3_REQUEST_STREAM_H
#define PHP_NGHTTP3_REQUEST_STREAM_H

#include "php.h"
#include <stdint.h>

extern zend_class_entry *php_http3_request_stream_ce;

int php_nghttp3_request_stream_init(INIT_FUNC_ARGS);
void php_http3_request_stream_create(zval *return_value, zval *connection, int64_t stream_id);

#endif /* PHP_NGHTTP3_REQUEST_STREAM_H */
