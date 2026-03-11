#ifndef PHP_NGHTTP3_EXCEPTION_H
#define PHP_NGHTTP3_EXCEPTION_H

#include "php.h"

extern zend_class_entry *php_http3_exception_ce;
extern zend_class_entry *php_http3_invalid_operation_ce;
extern zend_class_entry *php_http3_state_exception_ce;
extern zend_class_entry *php_http3_native_exception_ce;

int php_nghttp3_exception_init(INIT_FUNC_ARGS);

#endif /* PHP_NGHTTP3_EXCEPTION_H */
