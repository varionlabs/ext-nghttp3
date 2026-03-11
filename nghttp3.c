#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_nghttp3.h"

#include <ext/standard/info.h>

#include "src/internal/event.h"
#include "src/internal/exception.h"
#include "src/internal/http3_connection.h"
#include "src/internal/http3_request_stream.h"

PHP_MINIT_FUNCTION(nghttp3);
PHP_MSHUTDOWN_FUNCTION(nghttp3);
PHP_MINFO_FUNCTION(nghttp3);

zend_module_entry nghttp3_module_entry = {
  STANDARD_MODULE_HEADER,
  "nghttp3",
  NULL,
  PHP_MINIT(nghttp3),
  PHP_MSHUTDOWN(nghttp3),
  NULL,
  NULL,
  PHP_MINFO(nghttp3),
  PHP_NGHTTP3_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NGHTTP3
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(nghttp3)
#endif

PHP_MINIT_FUNCTION(nghttp3) {
  if (php_nghttp3_exception_init(INIT_FUNC_ARGS_PASSTHRU) != SUCCESS) {
    return FAILURE;
  }

  if (php_nghttp3_event_init(INIT_FUNC_ARGS_PASSTHRU) != SUCCESS) {
    return FAILURE;
  }

  if (php_nghttp3_request_stream_init(INIT_FUNC_ARGS_PASSTHRU) != SUCCESS) {
    return FAILURE;
  }

  if (php_nghttp3_connection_init(INIT_FUNC_ARGS_PASSTHRU) != SUCCESS) {
    return FAILURE;
  }

  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(nghttp3) {
  return SUCCESS;
}

PHP_MINFO_FUNCTION(nghttp3) {
  php_info_print_table_start();
  php_info_print_table_row(2, "nghttp3 support", "enabled");
  php_info_print_table_row(2, "Version", PHP_NGHTTP3_VERSION);
  php_info_print_table_end();
}
