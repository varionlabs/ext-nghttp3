# nghttp3 PHP拡張 MVP 開発計画

## 1. 前提の再整理 (SPEC 準拠)

- 目的は `ngtcp2` の上に載る HTTP/3 layer を提供すること。transport 実装は `ext-ngtcp2` 側に残す。
- `nghttp3` 拡張は単独でビルド/ロード可能にするが、実運用時は `Varion\Ngtcp2\QuicConnection` と接続して動かす。
- callback は直接 PHP callback を呼ばず、`callback -> internal queue -> pollEvents()` の非同期 pull モデルに統一する。
- UDP ソケット/イベントループは持たない。ユーザーランドが `recvfrom/sendto` とタイマーを管理する前提は維持する。
- public API は `Http3Connection` / `Http3RequestStream` を中心にし、control stream/QPACK stream は公開しない。
- Fake bridge を使った unit test を先に作り、real ngtcp2 連携は integration として後段に分離する。

## 2. 参照実装の取り込み方針

### 2.1 `sample_client.c` から取り込む点

- TLS/QUIC 接続:
  - `setup_tls()`
  - `setup_quic()`
  - `ngtcp2_conn_set_tls_native_handle()`
- HTTP/3 接続:
  - `setup_http3()`
  - `bind_h3_unidirectional_streams()`
- 受信経路:
  - `q_recv_stream_data_cb()` で `nghttp3_conn_read_stream()`
  - `q_acked_stream_data_offset_cb()` で `nghttp3_conn_add_ack_offset()`
  - `q_stream_close_cb()` で `nghttp3_conn_close_stream()`
- 送信経路:
  - `flush_h3_to_quic()` で `nghttp3_conn_writev_stream()`
  - `nghttp3_conn_add_write_offset()` の反映
- フロー制御:
  - DATA payload 分は `recv_data` 側で credit 返却
  - frame/QPACK/control 分は `read_stream` consumed 分で返却

### 2.2 `ext-ngtcp2` から取り込む点

- class 登録とモジュール初期化の分割 (`ngtcp2.c` + `src/*.c`)。
- `internal/types.h` による object/state/event の一元定義。
- callback から event queue に積み、`pollEvents()` で object 化する流れ (`src/event.c`)。
- `tests/*.phpt` を中心に API 契約を固定するテストスタイル。

## 3. 実装対象のファイル分解

`ext-nghttp3/` を新規作成し、以下を初期構成とする。

```text
ext-nghttp3/
  config.m4
  php_nghttp3.h
  nghttp3.c
  README.md
  src/
    http3_connection.c
    http3_request_stream.c
    event.c
    exception.c
    bridge_fake.c
    bridge_ngtcp2.c
    native_client.c
    native_callbacks.c
    queue.c
    headers.c
    internal/
      types.h
      connection.h
      stream.h
      event.h
      exception.h
      bridge.h
      native.h
      queue.h
      headers.h
      macros.h
  tests/
    001_load_extension.phpt
    010_http3_connection_ctor.phpt
    020_create_request_stream.phpt
    030_submit_headers_data_end.phpt
    040_fake_signal_readable_to_events.phpt
    050_terminal_rules.phpt
    060_reset_and_goaway.phpt
    100_integration_single_request.phpt
```

## 4. 最初のコミットで作る skeleton

最初のコミットは「型と境界の固定」に限定する。

- build/load 土台:
  - `config.m4` と `nghttp3.c` を追加し、`php -m` で `nghttp3` が見える状態にする。
- public class:
  - `Varion\Nghttp3\Http3Connection`
  - `Varion\Nghttp3\Http3RequestStream`
  - `Varion\Nghttp3\Event` 階層
  - `Varion\Nghttp3\Http3Exception` 階層
- internal:
  - `QuicBridge` 抽象 (`internal/bridge.h`)
  - `QuicSignal` enum/union (`internal/types.h`)
  - `Http3ConnectionState` / `RequestStreamState`
- API は未実装でもよいが、メソッド署名は SPEC 固定値で登録する。
- 最低テスト:
  - 拡張ロード
  - class 存在
  - メソッド署名

## 5. Phase 1 コード雛形

### 5.1 bridge 境界 (C)

```c
/* src/internal/bridge.h */
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

typedef struct _php_http3_quic_bridge php_http3_quic_bridge;
struct _php_http3_quic_bridge {
  void *ctx;
  int (*open_bidi_stream)(php_http3_quic_bridge *bridge, int64_t *stream_id);
  int (*write_stream)(php_http3_quic_bridge *bridge, int64_t stream_id, zend_string *data);
  int (*finish_stream)(php_http3_quic_bridge *bridge, int64_t stream_id);
  int (*reset_stream)(php_http3_quic_bridge *bridge, int64_t stream_id, uint64_t error_code);
  int (*poll_signals)(php_http3_quic_bridge *bridge, HashTable *out_signals);
  void (*destroy)(php_http3_quic_bridge *bridge);
};
```

### 5.2 connection state (C)

```c
/* src/internal/types.h */
typedef enum _php_http3_connection_state {
  PHP_HTTP3_CONN_INITIAL = 0,
  PHP_HTTP3_CONN_ACTIVE = 1,
  PHP_HTTP3_CONN_GOAWAY_RECEIVED = 2,
  PHP_HTTP3_CONN_CLOSING = 3,
  PHP_HTTP3_CONN_CLOSED = 4
} php_http3_connection_state;

typedef struct _php_http3_request_stream_state {
  zend_bool request_headers_submitted;
  zend_bool local_ended;
  zend_bool remote_headers_received;
  zend_bool remote_ended;
  zend_bool terminal;
  zend_bool reset;
  uint64_t reset_error_code;
} php_http3_request_stream_state;
```

### 5.3 public class 雛形 (PHP API)

```php
namespace Varion\Nghttp3;

final class Http3Connection {
    public function __construct(\Varion\Ngtcp2\QuicConnection $quic) {}
    public function createRequestStream(): Http3RequestStream {}
    public function pollEvents(): array {}
    public function isClosing(): bool {}
    public function close(): void {}
}

final class Http3RequestStream {
    public function getId(): int {}
    public function submitHeaders(array $headers): void {}
    public function submitData(string $data): void {}
    public function end(): void {}
    public function reset(int $errorCode = 0): void {}
    public function isClosed(): bool {}
}
```

## 6. Phase 2 (FakeQuicAdapter) unit test 例

`tests/030_submit_headers_data_end.phpt` 例:

```php
--TEST--
Http3RequestStream submitHeaders/submitData/end pushes writes to fake bridge
--FILE--
<?php
$fake = new Varion\Nghttp3\Testing\FakeQuicAdapter();
$conn = Varion\Nghttp3\Testing\Http3ConnectionFactory::fromFake($fake);

$s = $conn->createRequestStream();
$s->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/'],
]);
$s->submitData("abc");
$s->end();

var_dump($s->getId() >= 0);
var_dump(count($fake->getWrites()) >= 1);
var_dump($fake->getFinishedStreams() === [$s->getId()]);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
```

`tests/040_fake_signal_readable_to_events.phpt` 例:

```php
--TEST--
Readable signal is translated into Headers/Data/RequestCompleted order
--FILE--
<?php
$fake = new Varion\Nghttp3\Testing\FakeQuicAdapter();
$conn = Varion\Nghttp3\Testing\Http3ConnectionFactory::fromFake($fake);
$s = $conn->createRequestStream();
$id = $s->getId();

$fake->injectReadable($id, "...\x00", false); // 実際は HEADERS frame bytes
$fake->injectReadable($id, "body", true);

$events = $conn->pollEvents();
$names = array_map(fn($e) => get_class($e), $events);
echo implode("\n", $names), "\n";
?>
--EXPECTF--
Varion\Nghttp3\Events\HeadersReceived
Varion\Nghttp3\Events\DataReceived
Varion\Nghttp3\Events\RequestCompleted
```

## 7. 実装フェーズ

### Phase 1: 型と骨格

- class/exception/event 登録
- bridge 抽象と signal 型
- internal state skeleton
- `001/010/020` PHPT

### Phase 2: Fake + 送信系 happy path

- `createRequestStream()`
- `submitHeaders()/submitData()/end()`
- fake write/finish 記録
- `030` PHPT

### Phase 3: 受信 + event queue

- fake signal consume
- `nghttp3_conn_read_stream()` 経由で event 化
- `pollEvents()`
- `040` PHPT

### Phase 4: terminal/reset/GOAWAY

- terminal event 1回制御
- `StreamReset` / `GoawayReceived`
- API 誤用例外
- `050/060` PHPT

### Phase 5: real ngtcp2 adapter

- `bridge_ngtcp2.c` 実装
- `Varion\Ngtcp2\QuicConnection` への委譲で open/write/finish/reset/poll
- 単一 request の integration (`100`)

### Phase 6: integration 拡張

- 複数 request stream
- reset/GOAWAY/closing
- 大きめ body

## 8. 実装時の注意点

- transport signal と HTTP/3 event を同じ型にしない。
- callback から Zend API を広範囲に呼ばず、queue push のみに寄せる。
- 1 signal から複数 event が出る場合は `HeadersReceived -> DataReceived -> terminal` 順を厳守する。
- terminal 後の通常 event を抑止し、terminal event を多重発火させない。
- GOAWAY 後の `createRequestStream()` を必ず例外化する。
- `sample_client.c` と同様に `nghttp3_conn_add_ack_offset()` / `nghttp3_conn_add_write_offset()` の反映漏れを防ぐ。
- DATA payload と frame/control bytes の credit 返却経路を混同しない。
- native return code をそのまま public に漏らさず、`Http3Exception` 階層へマップする。

## 9. 後回しにする項目 (MVP外)

- server mode
- push
- QPACK チューニング
- priority
- trailers の高機能 API
- request() などの sugar API
- DTO ベースの高水準 response モデル

## 10. 追加提案 (SPEC の「必要なら」対応)

### 10.1 C 側の内部 struct 案

- `php_http3_connection`:
  - `nghttp3_conn *conn`
  - `php_http3_quic_bridge *bridge`
  - `HashTable request_streams`
  - `HashTable stream_states`
  - `php_http3_event_queue events`
  - `php_http3_connection_state state`
  - `zend_bool closing`
- `php_http3_request_stream`:
  - `zval connection`
  - `int64_t stream_id`

### 10.2 zend_class_entry 登録順案

1. Exception hierarchy
2. Event base hierarchy
3. Event concrete classes
4. `Http3RequestStream`
5. `Http3Connection`
6. Testing namespace (`FakeQuicAdapter` など)

### 10.3 object handlers 最小案

- `create_object`:
  - 構造体確保
  - 初期 state 設定
  - `object_properties_init`
- `free_obj`:
  - queue/HashTable 解放
  - native handle 解放 (`nghttp3_conn_del`, bridge destroy)
  - `zend_object_std_dtor`

### 10.4 PHPUnit / PHPT のテスト分割案

- PHPT:
  - 拡張ロード、型、例外、event 順序、API 契約の固定
- PHPUnit (任意):
  - fake bridge を使ったシナリオを高速反復
  - event 変換ロジックのケース追加を容易化

