--TEST--
Phase 6 multiple streams: stub QUIC path keeps per-stream ordering and payloads
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
}
?>
--FILE--
<?php

final class StubEvent111 {
    public function __construct(
        private int $type,
        private int $streamId = -1,
        private int $errorCode = 0,
    ) {}

    public function getType(): int { return $this->type; }
    public function getStreamId(): int { return $this->streamId; }
    public function getErrorCode(): int { return $this->errorCode; }
}

final class StubStream111 {
    private bool $closed = false;
    private string $rx = '';
    public string $tx = '';

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
}

final class StubConnection111 {
    private int $next = 0;
    private array $streams = [];
    private array $events = [];

    public function openStream(): StubStream111 {
        $stream = new StubStream111($this->next);
        $this->streams[$this->next] = $stream;
        $this->next += 4;
        return $stream;
    }

    public function getStream(int $streamId): ?StubStream111 {
        return $this->streams[$streamId] ?? null;
    }

    public function pollEvents(): array {
        $events = $this->events;
        $this->events = [];
        return $events;
    }

    public function close(): void {}

    public function injectReadable(int $streamId, string $data, bool $fin): void {
        $this->streams[$streamId]->injectReadable($data, $fin);
        $this->events[] = new StubEvent111(11, $streamId, 0);
    }
}

$quic = new StubConnection111();
$http3 = new Varion\Nghttp3\Http3Connection($quic);

$s1 = $http3->createRequestStream();
$s2 = $http3->createRequestStream();

var_dump($s1->getId() === 0);
var_dump($s2->getId() === 4);

$s1->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/one'],
]);
$s1->submitData('tx1');
$s1->end();

$s2->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/two'],
]);
$s2->submitData('tx2');
$s2->end();

$quic->injectReadable($s2->getId(), 'payload-2', true);
$quic->injectReadable($s1->getId(), 'payload-1', true);

$events = $http3->pollEvents();
var_dump(count($events) === 6);
var_dump(array_map(fn($e) => $e->getStreamId(), $events) === [4, 4, 4, 0, 0, 0]);
var_dump(get_class($events[0]) === 'Varion\\Nghttp3\\Events\\HeadersReceived');
var_dump(get_class($events[1]) === 'Varion\\Nghttp3\\Events\\DataReceived');
var_dump(get_class($events[2]) === 'Varion\\Nghttp3\\Events\\RequestCompleted');
var_dump(get_class($events[3]) === 'Varion\\Nghttp3\\Events\\HeadersReceived');
var_dump(get_class($events[4]) === 'Varion\\Nghttp3\\Events\\DataReceived');
var_dump(get_class($events[5]) === 'Varion\\Nghttp3\\Events\\RequestCompleted');
var_dump($events[1]->getPayload() === 'payload-2');
var_dump($events[4]->getPayload() === 'payload-1');
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
