# nghttp3 examples

## Requirements for a minimal HTTP/3 client script

1. Load both extensions in the same PHP process:
   - `ngtcp2.so`
   - `nghttp3.so`
2. Use a `Varion\Ngtcp2\Connection` as QUIC transport and wrap it with `Varion\Nghttp3\Http3Connection`.
3. Drive network I/O in userland (Sans-I/O):
   - `recv(Datagram)` on incoming UDP packets
   - `flush()` for outgoing QUIC datagrams
   - `getNextTimeout()/onTimeout()` for timer progression
4. Build request headers as `[[name, value], ...]` pairs:
   - Include pseudo-headers (`:method`, `:scheme`, `:authority`, `:path`)
5. Consume HTTP/3 events via `Http3Connection::pollEvents()`:
   - `HeadersReceived`
   - `DataReceived`
   - `RequestCompleted`
   - `StreamReset`
   - `GoawayReceived`
6. Target server must actually support HTTP/3 over QUIC (UDP) and ALPN `h3`.

## Example: one-shot HTTP/3 request

`http3_client_once.php` sends one request and exits after completion or timeout.

```sh
php -n \
  -d extension=$(pwd)/modules/nghttp3.so \
  -d extension=$(pwd)/ext-ngtcp2/modules/ngtcp2.so \
  examples/http3_client_once.php \
  --host=127.0.0.1 --port=4433 --path=/
```

Optional flags:

- `--method=GET`
- `--body=''`
- `--authority=example.com`
- `--timeout-ms=8000`

## Notes

- `ext-ngtcp2/examples/server_minimal.php` is a QUIC transport example and is not an HTTP/3 application server.
- For response parsing with `Http3Connection`, use an HTTP/3-capable peer.
