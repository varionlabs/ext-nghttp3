--TEST--
Phase 6 reset/GOAWAY/closing: stub QUIC path enforces terminal and close semantics
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
}
?>
--FILE--
<?php

final class StubEvent112 {
    public function __construct(
        private int $type,
        private int $streamId = -1,
        private int $errorCode = 0,
    ) {}

    public function getType(): int { return $this->type; }
    public function getStreamId(): int { return $this->streamId; }
    public function getErrorCode(): int { return $this->errorCode; }
}

final class StubStream112 {
    private bool $closed = false;
    private string $rx = '';

    public function __construct(private int $id) {}

    public function getId(): int { return $this->id; }

    public function write(string $data): int {
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

final class StubConnection112 {
    private int $next = 0;
    private array $streams = [];
    private array $events = [];
    public int $closeCalls = 0;

    public function openStream(): StubStream112 {
        $stream = new StubStream112($this->next);
        $this->streams[$this->next] = $stream;
        $this->next += 4;
        return $stream;
    }

    public function getStream(int $streamId): ?StubStream112 {
        return $this->streams[$streamId] ?? null;
    }

    public function pollEvents(): array {
        $events = $this->events;
        $this->events = [];
        return $events;
    }

    public function close(): void {
        $this->closeCalls++;
    }

    public function injectReadable(int $streamId, string $data, bool $fin): void {
        $this->streams[$streamId]->injectReadable($data, $fin);
        $this->events[] = new StubEvent112(11, $streamId, 0);
    }

    public function injectReset(int $streamId, int $errorCode): void {
        $this->events[] = new StubEvent112(14, $streamId, $errorCode);
    }

    public function injectConnectionDraining(int $errorCode): void {
        $this->events[] = new StubEvent112(3, -1, $errorCode);
    }

    public function injectConnectionClosed(int $errorCode): void {
        $this->events[] = new StubEvent112(2, -1, $errorCode);
    }
}

$quic = new StubConnection112();
$http3 = new Varion\Nghttp3\Http3Connection($quic);
$stream = $http3->createRequestStream();
$id = $stream->getId();

$quic->injectReset($id, 321);
$events = $http3->pollEvents();
var_dump(count($events) === 1);
var_dump(get_class($events[0]) === 'Varion\\Nghttp3\\Events\\StreamReset');
var_dump($events[0]->getErrorCode() === 321);
var_dump($stream->isClosed());

$quic->injectReadable($id, 'late-data', true);
var_dump(count($http3->pollEvents()) === 0);

$quic->injectConnectionDraining(77);
$events = $http3->pollEvents();
var_dump(count($events) === 1);
var_dump(get_class($events[0]) === 'Varion\\Nghttp3\\Events\\GoawayReceived');
var_dump($events[0]->getErrorCode() === 77);
var_dump($http3->isClosing());

$stateError = false;
try {
    $http3->createRequestStream();
} catch (Varion\Nghttp3\Http3StateException $e) {
    $stateError = true;
}
var_dump($stateError);

$http3->close();
$http3->close();
var_dump($quic->closeCalls === 1);

$quic->injectConnectionClosed(88);
var_dump(count($http3->pollEvents()) === 0);

$stateError = false;
try {
    $http3->createRequestStream();
} catch (Varion\Nghttp3\Http3StateException $e) {
    $stateError = true;
}
var_dump($stateError);
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
