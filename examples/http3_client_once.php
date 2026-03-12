<?php

declare(strict_types=1);

use Varion\Ngtcp2\Address;
use Varion\Ngtcp2\Connection;
use Varion\Ngtcp2\Datagram;
use Varion\Nghttp3\Events\DataReceived;
use Varion\Nghttp3\Events\GoawayReceived;
use Varion\Nghttp3\Events\HeadersReceived;
use Varion\Nghttp3\Events\RequestCompleted;
use Varion\Nghttp3\Events\StreamReset;
use Varion\Nghttp3\Http3Connection;

function resolveWaitTimeoutMs(Connection $quic, int $maxWaitMs = 100): int
{
    if (method_exists($quic, 'getTimeoutAt')) {
        $timeoutAt = $quic->getTimeoutAt();
        if (is_int($timeoutAt)) {
            $nowMs = (int)floor(microtime(true) * 1000);
            $remaining = $timeoutAt - $nowMs;
            if ($remaining < 0) {
                return 0;
            }
            return min($remaining, $maxWaitMs);
        }
    }

    $nextTimeout = $quic->getNextTimeout();
    if (!is_int($nextTimeout)) {
        return $maxWaitMs;
    }
    if ($nextTimeout < 0) {
        return 0;
    }

    return min($nextTimeout, $maxWaitMs);
}

function usage(): void
{
    fwrite(STDERR, <<<TXT
Usage:
  php examples/http3_client_once.php [--host=127.0.0.1] [--port=4433] [--path=/] [--method=GET] [--body=''] [--authority=example.com] [--timeout-ms=8000]

TXT);
}

if (!extension_loaded('ngtcp2') || !extension_loaded('nghttp3')) {
    fwrite(STDERR, "ngtcp2 and nghttp3 extensions must be loaded\n");
    exit(2);
}

$options = getopt('', ['host::', 'port::', 'path::', 'method::', 'body::', 'authority::', 'timeout-ms::', 'help']);
if ($options === false) {
    usage();
    exit(2);
}
if (isset($options['help'])) {
    usage();
    exit(0);
}

$host = is_string($options['host'] ?? null) ? $options['host'] : '127.0.0.1';
$port = (int)($options['port'] ?? 4433);
$path = is_string($options['path'] ?? null) ? $options['path'] : '/';
$method = strtoupper(is_string($options['method'] ?? null) ? $options['method'] : 'GET');
$body = is_string($options['body'] ?? null) ? $options['body'] : '';
$timeoutMs = (int)($options['timeout-ms'] ?? 8000);

if ($port <= 0 || $port > 65535) {
    throw new InvalidArgumentException("invalid --port: {$port}");
}
if ($timeoutMs <= 0) {
    throw new InvalidArgumentException("invalid --timeout-ms: {$timeoutMs}");
}

$authority = is_string($options['authority'] ?? null)
    ? $options['authority']
    : ($port === 443 ? $host : "{$host}:{$port}");

$remote = new Address($host, $port);
$quic = new Connection($remote);
$http3 = new Http3Connection($quic);

$udp = stream_socket_client("udp://{$host}:{$port}", $errno, $errstr, 1, STREAM_CLIENT_CONNECT);
if ($udp === false) {
    throw new RuntimeException("UDP socket error: {$errno} {$errstr}");
}
stream_set_blocking($udp, false);

$stream = $http3->createRequestStream();
$stream->submitHeaders([
    [':method', $method],
    [':scheme', 'https'],
    [':authority', $authority],
    [':path', $path],
    ['user-agent', 'ext-nghttp3-example/0.1'],
]);
if ($body !== '') {
    $stream->submitData($body);
}
$stream->end();

$deadline = microtime(true) + ($timeoutMs / 1000.0);
$responseBody = '';
$receivedHeaders = false;
$completed = false;
$streamResetError = null;
$goawayError = null;

while (microtime(true) < $deadline && !$quic->isClosed() && !$http3->isClosing()) {
    foreach ($quic->drainOutgoingDatagrams() as $outgoing) {
        stream_socket_sendto($udp, $outgoing->getPayload());
    }

    $waitMs = resolveWaitTimeoutMs($quic);
    $sec = intdiv($waitMs, 1000);
    $usec = ($waitMs % 1000) * 1000;

    $read = [$udp];
    $write = null;
    $except = null;
    $ready = stream_select($read, $write, $except, $sec, $usec);
    if ($ready === false) {
        throw new RuntimeException('stream_select failed');
    }

    if ($ready > 0) {
        $packet = stream_socket_recvfrom($udp, 65535, 0, $peer);
        if (is_string($packet) && $packet !== '') {
            $quic->recv(new Datagram($packet, $remote));
        }
    } else {
        $quic->handleTimers();
    }

    foreach ($http3->pollEvents() as $event) {
        if ($event instanceof HeadersReceived) {
            $receivedHeaders = true;
            continue;
        }
        if ($event instanceof DataReceived) {
            $responseBody .= $event->getPayload();
            continue;
        }
        if ($event instanceof RequestCompleted) {
            $completed = true;
            break 2;
        }
        if ($event instanceof StreamReset) {
            $streamResetError = $event->getErrorCode();
            break 2;
        }
        if ($event instanceof GoawayReceived) {
            $goawayError = $event->getErrorCode();
            break 2;
        }
    }
}

fclose($udp);
$http3->close();

if ($streamResetError !== null) {
    fwrite(STDERR, "request stream was reset by peer: error={$streamResetError}\n");
    exit(1);
}
if ($goawayError !== null) {
    fwrite(STDERR, "connection received GOAWAY/closing: error={$goawayError}\n");
    exit(1);
}
if (!$completed) {
    fwrite(STDERR, "request did not complete before timeout ({$timeoutMs}ms)\n");
    exit(1);
}
if (!$receivedHeaders) {
    fwrite(STDERR, "warning: response completed without HeadersReceived event\n");
}

echo $responseBody, PHP_EOL;
