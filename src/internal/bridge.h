#ifndef PHP_NGHTTP3_BRIDGE_H
#define PHP_NGHTTP3_BRIDGE_H

#include "php.h"
#include <stdint.h>

typedef struct _php_http3_quic_bridge php_http3_quic_bridge;

struct _php_http3_quic_bridge {
  void *ctx;
  int (*open_bidirectional_stream)(php_http3_quic_bridge *bridge, int64_t *stream_id);
  int (*write_stream)(php_http3_quic_bridge *bridge, int64_t stream_id, zend_string *data);
  int (*finish_stream)(php_http3_quic_bridge *bridge, int64_t stream_id);
  int (*reset_stream)(php_http3_quic_bridge *bridge, int64_t stream_id, uint64_t error_code);
  int (*poll_signals)(php_http3_quic_bridge *bridge, HashTable *out_signals);
  void (*destroy)(php_http3_quic_bridge *bridge);
};

#endif /* PHP_NGHTTP3_BRIDGE_H */
