#ifndef PHP_NGHTTP3_CONNECTION_H
#define PHP_NGHTTP3_CONNECTION_H

#include "php.h"

extern zend_class_entry *php_http3_connection_ce;

int php_nghttp3_connection_init(INIT_FUNC_ARGS);

#endif /* PHP_NGHTTP3_CONNECTION_H */
