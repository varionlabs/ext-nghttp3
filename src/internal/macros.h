#ifndef PHP_NGHTTP3_MACROS_H
#define PHP_NGHTTP3_MACROS_H

#include "types.h"

#define Z_HTTP3_CONNECTION_P(zv)                                                \
  ((php_http3_connection *)((char *)Z_OBJ_P((zv)) -                             \
                            XtOffsetOf(php_http3_connection, std)))

#define Z_HTTP3_REQUEST_STREAM_P(zv)                                            \
  ((php_http3_request_stream *)((char *)Z_OBJ_P((zv)) -                         \
                                XtOffsetOf(php_http3_request_stream, std)))

#define Z_HTTP3_EVENT_OBJ_P(zv)                                                 \
  ((php_http3_event_object *)((char *)Z_OBJ_P((zv)) -                           \
                              XtOffsetOf(php_http3_event_object, std)))

#endif /* PHP_NGHTTP3_MACROS_H */
