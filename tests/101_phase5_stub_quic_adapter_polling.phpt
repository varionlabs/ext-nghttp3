--TEST--
Phase 5 real-adapter path works with stub QUIC object (open/write/end/drain/close)
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
}
?>
--FILE--
<?php

final class StubNgtcp2Event {
    public function __construct(
        private int $type,
        private int $streamId = -1,
        private int $errorCode = 0,
    ) {}

    public function getType(): int { return $this->type; }
    public function getStreamId(): int { return $this->streamId; }
    public function getErrorCode(): int { return $this->errorCode; }
}

final class StubNgtcp2Stream {
    public string $tx = '';
    public string $rx = '';

    public function __construct(private int $id) {}

    public function getId(): int { return $this->id; }
    public function write(string $data): int {
        $this->tx .= $data;
        return strlen($data);
    }

    public function end(): void {
        $this->closed = true;
    }

    public function reset(int $errorCode): void {
        $this->closed = true;
    }

    public function read(int $length = 8192): string {
        if ($this->rx === '') {
            return '';
        }
        $chunk = substr($this->rx, 0, $length);
        $this->rx = (string)substr($this->rx, strlen($chunk));
        return $chunk;
    }

    public function isClosed(): bool {
        return $this->closed;
    }

    public function injectReadable(string $data, bool $fin): void {
        $this->rx .= $data;
        if ($fin) {
            $this->closed = true;
        }
    }

    private bool $closed = false;
}

final class StubNgtcp2Connection {
    private int $next = 0;
    private array $streams = [];
    private array $events = [];
    public bool $closeCalled = false;

    public function openStream(): StubNgtcp2Stream {
        $stream = new StubNgtcp2Stream($this->next);
        $this->streams[$this->next] = $stream;
        $this->next += 4;
        return $stream;
    }

    public function getStream(int $streamId): ?StubNgtcp2Stream {
        return $this->streams[$streamId] ?? null;
    }

    public function drainEvents(): array {
        $events = $this->events;
        $this->events = [];
        return $events;
    }

    public function close(): void {
        $this->closeCalled = true;
    }

    public function injectReadable(int $streamId, string $data, bool $fin): void {
        $this->streams[$streamId]->injectReadable($data, $fin);
        $this->events[] = new StubNgtcp2Event(11, $streamId, 0);
    }

    public function injectConnectionClosed(int $errorCode): void {
        $this->events[] = new StubNgtcp2Event(2, -1, $errorCode);
    }
}

$quic = new StubNgtcp2Connection();
$http3 = new Varion\Nghttp3\Http3Connection($quic);
$stream = $http3->createRequestStream();
$id = $stream->getId();

$stream->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/'],
]);
$stream->submitData('abc');
$stream->end();

$quicStream = $quic->getStream($id);
var_dump($id === 0);
var_dump($quicStream instanceof StubNgtcp2Stream);
var_dump(str_contains($quicStream->tx, ':method:GET'));
var_dump(str_ends_with($quicStream->tx, 'abc'));

$quic->injectReadable($id, 'resp', true);
$events = $http3->pollEvents();
var_dump(count($events) === 3);
var_dump(get_class($events[0]) === 'Varion\\Nghttp3\\Events\\HeadersReceived');
var_dump(get_class($events[1]) === 'Varion\\Nghttp3\\Events\\DataReceived');
var_dump(get_class($events[2]) === 'Varion\\Nghttp3\\Events\\RequestCompleted');
var_dump($events[1]->getPayload() === 'resp');

$quic->injectConnectionClosed(77);
$events = $http3->pollEvents();
var_dump(count($events) === 1);
var_dump(get_class($events[0]) === 'Varion\\Nghttp3\\Events\\GoawayReceived');
var_dump($events[0]->getErrorCode() === 77);

$http3->close();
var_dump($quic->closeCalled);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
