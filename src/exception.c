#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <Zend/zend_exceptions.h>
#include <ext/spl/spl_exceptions.h>

#include "internal/exception.h"

zend_class_entry *php_http3_exception_ce;
zend_class_entry *php_http3_invalid_operation_ce;
zend_class_entry *php_http3_state_exception_ce;
zend_class_entry *php_http3_native_exception_ce;

int php_nghttp3_exception_init(INIT_FUNC_ARGS) {
  zend_class_entry ce;

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "Http3Exception", NULL);
  php_http3_exception_ce =
    zend_register_internal_class_ex(&ce, spl_ce_RuntimeException);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "InvalidHttp3Operation", NULL);
  php_http3_invalid_operation_ce =
    zend_register_internal_class_ex(&ce, php_http3_exception_ce);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "Http3StateException", NULL);
  php_http3_state_exception_ce =
    zend_register_internal_class_ex(&ce, php_http3_exception_ce);

  INIT_NS_CLASS_ENTRY(ce, "Varion\\Nghttp3", "NativeNghttp3Exception", NULL);
  php_http3_native_exception_ce =
    zend_register_internal_class_ex(&ce, php_http3_exception_ce);

  return SUCCESS;
}
