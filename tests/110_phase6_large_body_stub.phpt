--TEST--
Phase 6 large body: ngtcp2 readable drain preserves full payload
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
}
?>
--FILE--
<?php

final class StubEvent110 {
    public function __construct(private int $type, private int $streamId = -1) {}
    public function getType(): int { return $this->type; }
    public function getStreamId(): int { return $this->streamId; }
    public function getErrorCode(): int { return 0; }
}

final class StubStream110 {
    private bool $closed = false;
    private string $rx = '';

    public function __construct(private int $id) {}
    public function getId(): int { return $this->id; }
    public function write(string $data): int { return strlen($data); }
    public function end(): void { $this->closed = true; }
    public function reset(int $errorCode): void { $this->closed = true; }
    public function isClosed(): bool { return $this->closed; }

    public function read(int $length = 8192): string {
        if ($this->rx === '') {
            return '';
        }
        $chunk = substr($this->rx, 0, $length);
        $this->rx = (string)substr($this->rx, strlen($chunk));
        return $chunk;
    }

    public function injectReadable(string $data, bool $fin): void {
        $this->rx .= $data;
        if ($fin) {
            $this->closed = true;
        }
    }
}

final class StubConnection110 {
    private int $next = 0;
    private array $streams = [];
    private array $events = [];

    public function openStream(): StubStream110 {
        $stream = new StubStream110($this->next);
        $this->streams[$this->next] = $stream;
        $this->next += 4;
        return $stream;
    }

    public function getStream(int $streamId): ?StubStream110 {
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
        $this->events[] = new StubEvent110(11, $streamId);
    }
}

$quic = new StubConnection110();
$http3 = new Varion\Nghttp3\Http3Connection($quic);
$stream = $http3->createRequestStream();
$id = $stream->getId();

$payload = str_repeat('x', 100000);
$quic->injectReadable($id, $payload, true);

$events = $http3->pollEvents();
var_dump(count($events) === 3);
var_dump(get_class($events[1]) === 'Varion\\Nghttp3\\Events\\DataReceived');
var_dump(strlen($events[1]->getPayload()) === 100000);
var_dump(get_class($events[2]) === 'Varion\\Nghttp3\\Events\\RequestCompleted');
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
