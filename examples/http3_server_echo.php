<?php

declare(strict_types=1);

use Varion\Ngtcp2\Address;
use Varion\Ngtcp2\Datagram;
use Varion\Ngtcp2\ServerConfig;
use Varion\Ngtcp2\ServerConnection;
use Varion\Nghttp3\Events\DataReceived;
use Varion\Nghttp3\Events\HeadersReceived;
use Varion\Nghttp3\Events\RequestCompleted;
use Varion\Nghttp3\Events\StreamReset;
use Varion\Nghttp3\Http3Connection;

function usage(): void
{
    fwrite(STDERR, <<<TXT
Usage:
  php examples/http3_server_echo.php [--host=127.0.0.1] [--port=4433]
                                     [--cert=/tmp/nghttp3/server.crt] [--key=/tmp/nghttp3/server.key]
                                     [--alpn=h3] [--prefix='echo: '] [--timeout-ms=30000]

TXT);
}

function ensureCertificate(string $certPath, string $keyPath, string $host): void
{
    if (is_file($certPath) && is_file($keyPath)) {
        return;
    }

    $certDir = dirname($certPath);
    if (!is_dir($certDir) && !mkdir($certDir, 0700, true) && !is_dir($certDir)) {
        throw new RuntimeException("failed to create certificate directory: {$certDir}");
    }

    $keyDir = dirname($keyPath);
    if (!is_dir($keyDir) && !mkdir($keyDir, 0700, true) && !is_dir($keyDir)) {
        throw new RuntimeException("failed to create key directory: {$keyDir}");
    }

    $subject = '/CN=' . $host;
    $command = sprintf(
        '/usr/bin/openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 -subj %s -keyout %s -out %s',
        escapeshellarg($subject),
        escapeshellarg($keyPath),
        escapeshellarg($certPath)
    );
    exec($command . ' 2>/dev/null', $out, $code);
    if ($code !== 0) {
        throw new RuntimeException('openssl certificate generation failed');
    }
}

function parsePeerAddress(string $peer): Address
{
    if (preg_match('/^\[(.+)\]:(\d+)$/', $peer, $m) === 1) {
        return new Address($m[1], (int)$m[2]);
    }

    $pos = strrpos($peer, ':');
    if ($pos === false) {
        throw new RuntimeException("cannot parse peer address: {$peer}");
    }

    return new Address(substr($peer, 0, $pos), (int)substr($peer, $pos + 1));
}

function resolveWaitTimeoutMs(array $sessions, int $defaultMs = 100): int
{
    if ($sessions === []) {
        return $defaultMs;
    }

    $best = $defaultMs;
    foreach ($sessions as $session) {
        $serverConn = $session['conn'] ?? null;
        if (!$serverConn instanceof ServerConnection) {
            continue;
        }

        if (method_exists($serverConn, 'getTimeoutAt')) {
            $timeoutAt = $serverConn->getTimeoutAt();
            if (is_int($timeoutAt)) {
                $nowMs = (int)floor(microtime(true) * 1000);
                $remaining = $timeoutAt - $nowMs;
                if ($remaining < 0) {
                    return 0;
                }
                if ($remaining < $best) {
                    $best = $remaining;
                }
                continue;
            }
        }

        $next = $serverConn->getNextTimeout();
        if (!is_int($next)) {
            continue;
        }
        if ($next < 0) {
            return 0;
        }
        if ($next < $best) {
            $best = $next;
        }
    }

    if ($best < 0) {
        return 0;
    }

    return min($best, $defaultMs);
}

if (!extension_loaded('ngtcp2') || !extension_loaded('nghttp3')) {
    fwrite(STDERR, "ngtcp2 and nghttp3 extensions must be loaded\n");
    exit(2);
}

$options = getopt('', ['host::', 'port::', 'cert::', 'key::', 'alpn::', 'prefix::', 'timeout-ms::', 'help']);
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
$cert = is_string($options['cert'] ?? null) ? $options['cert'] : '/tmp/nghttp3/server.crt';
$key = is_string($options['key'] ?? null) ? $options['key'] : '/tmp/nghttp3/server.key';
$alpn = is_string($options['alpn'] ?? null) ? $options['alpn'] : 'h3';
$prefix = is_string($options['prefix'] ?? null) ? $options['prefix'] : 'echo: ';
$timeoutMs = (int)($options['timeout-ms'] ?? 30000);

if ($port <= 0 || $port > 65535) {
    throw new InvalidArgumentException("invalid --port: {$port}");
}
if ($timeoutMs <= 0) {
    throw new InvalidArgumentException("invalid --timeout-ms: {$timeoutMs}");
}
if (!is_executable('/usr/bin/openssl')) {
    throw new RuntimeException('/usr/bin/openssl is not available');
}

ensureCertificate($cert, $key, $host);

$udp = stream_socket_server("udp://{$host}:{$port}", $errno, $errstr, STREAM_SERVER_BIND);
if ($udp === false) {
    throw new RuntimeException("failed to bind UDP socket: ({$errno}) {$errstr}");
}
stream_set_blocking($udp, false);

fwrite(STDERR, "http3 echo server waiting on {$host}:{$port}\n");

$sessions = [];
$deadline = microtime(true) + ($timeoutMs / 1000.0);

while (microtime(true) < $deadline) {
    $waitMs = resolveWaitTimeoutMs($sessions);
    $sec = intdiv($waitMs, 1000);
    $usec = ($waitMs % 1000) * 1000;
    $read = [$udp];
    $write = null;
    $except = null;
    $ready = stream_select($read, $write, $except, $sec, $usec);
    if ($ready === false) {
        throw new RuntimeException('stream_select failed');
    }

    $from = null;
    $packet = $ready > 0 ? stream_socket_recvfrom($udp, 65535, 0, $from) : false;
    if (is_string($packet) && $packet !== '' && is_string($from) && $from !== '') {
        if (!array_key_exists($from, $sessions)) {
            $remote = parsePeerAddress($from);
            $local = new Address($host, $port);
            $dgram = new Datagram($packet, $remote, $local);

            try {
                $serverConn = ServerConnection::accept(
                    $dgram,
                    (new ServerConfig())
                        ->withCertificate($cert, $key)
                        ->withAlpn($alpn)
                );
                $sessions[$from] = [
                    'conn' => $serverConn,
                    'http3' => new Http3Connection($serverConn),
                    'requestBodies' => [],
                    'responded' => [],
                ];
                fwrite(STDERR, "accepted peer {$from}\n");
            } catch (Throwable $e) {
                fwrite(STDERR, "accept warning for {$from}: {$e->getMessage()}\n");
            }
        } else {
            $session = $sessions[$from];
            $serverConn = $session['conn'] ?? null;
            if (!$serverConn instanceof ServerConnection) {
                continue;
            }

            $remote = parsePeerAddress($from);
            $local = new Address($host, $port);
            $dgram = new Datagram($packet, $remote, $local);

            try {
                $serverConn->recv($dgram);
            } catch (Throwable $e) {
                fwrite(STDERR, "recv warning: {$e->getMessage()}\n");
            }
        }
    }

    $closedPeers = [];
    foreach ($sessions as $peer => &$session) {
        $serverConn = $session['conn'] ?? null;
        $http3 = $session['http3'] ?? null;
        if (!$serverConn instanceof ServerConnection || !$http3 instanceof Http3Connection) {
            continue;
        }

        try {
            $serverConn->handleTimers();
        } catch (Throwable $e) {
            fwrite(STDERR, "timeout warning: {$e->getMessage()}\n");
        }

        $requestBodies = &$session['requestBodies'];
        $responded = &$session['responded'];
        foreach ($http3->pollEvents() as $event) {
            if ($event instanceof DataReceived) {
                $requestBodies[$event->getStreamId()] = ($requestBodies[$event->getStreamId()] ?? '') . $event->getPayload();
                continue;
            }

            if ($event instanceof HeadersReceived) {
                $streamId = $event->getStreamId();
                if (($responded[$streamId] ?? false) === true) {
                    continue;
                }

                $stream = $http3->getRequestStream($streamId);
                if ($stream === null) {
                    continue;
                }

                $body = ($requestBodies[$streamId] ?? '');
                $stream->submitHeaders([
                    [':status', '200'],
                    ['content-type', 'text/plain'],
                ]);
                $stream->submitData($prefix . $body);
                $stream->end();
                $responded[$streamId] = true;
                fwrite(STDERR, "responded peer {$peer} stream {$streamId}\n");
                continue;
            }

            if ($event instanceof RequestCompleted) {
                fwrite(STDERR, "request completed stream {$event->getStreamId()}\n");
                continue;
            }

            if ($event instanceof StreamReset) {
                fwrite(STDERR, "stream reset {$event->getStreamId()} error={$event->getErrorCode()}\n");
            }
        }

        try {
            foreach ($serverConn->drainOutgoingDatagrams() as $outgoing) {
                stream_socket_sendto($udp, $outgoing->getPayload(), 0, $peer);
            }
        } catch (Throwable $e) {
            fwrite(STDERR, "drain warning: {$e->getMessage()}\n");
        }

        if ($serverConn->isClosed()) {
            $closedPeers[] = $peer;
            fwrite(STDERR, "closed peer {$peer}\n");
        }
    }
    unset($session, $requestBodies, $responded);
    foreach ($closedPeers as $closedPeer) {
        unset($sessions[$closedPeer]);
    }
}

if (is_resource($udp)) {
    fclose($udp);
}
