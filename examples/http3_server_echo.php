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

$optind = 0;
$options = getopt('', ['address::', 'alpn::', 'prefix::', 'timeout-ms::', 'help'], $optind);
if ($options === false) {
    usage();
    exit(2);
}
if (isset($options['help'])) {
    usage();
    exit(0);
}

if (!extension_loaded('ngtcp2')) {
    fwrite(STDERR, "ngtcp2 extension must be loaded\n");
    exit(2);
}
if (!extension_loaded('nghttp3')) {
    fwrite(STDERR, "nghttp3 extension must be loaded\n");
    exit(2);
}

$args = array_slice($argv, $optind);
if (count($args) !== 1 && count($args) !== 3) {
    usage();
    exit(2);
}

$host = is_string($options['address'] ?? null) ? $options['address'] : '127.0.0.1';
$port = (int)$args[0];
$useProvidedCredentials = count($args) === 3;
$key = $useProvidedCredentials ? $args[1] : '/tmp/nghttp3/server.key';
$cert = $useProvidedCredentials ? $args[2] : '/tmp/nghttp3/server.crt';
$alpn = is_string($options['alpn'] ?? null) ? $options['alpn'] : 'h3';
$prefix = is_string($options['prefix'] ?? null) ? $options['prefix'] : 'echo: ';
$timeoutMs = (int)($options['timeout-ms'] ?? 0);

if ($port <= 0 || $port > 65535) {
    throw new InvalidArgumentException("invalid <PORT>: {$port}");
}
if ($timeoutMs < 0) {
    throw new InvalidArgumentException("invalid --timeout-ms: {$timeoutMs}");
}
if (!is_executable('/usr/bin/openssl')) {
    throw new RuntimeException('/usr/bin/openssl is not available');
}

if ($useProvidedCredentials) {
    if (!is_file($key)) {
        throw new RuntimeException("private key file does not exist: {$key}");
    }
    if (!is_file($cert)) {
        throw new RuntimeException("certificate file does not exist: {$cert}");
    }
} else {
    ensureCertificate($cert, $key, $host);
}

$udp = stream_socket_server("udp://{$host}:{$port}", $errno, $errstr, STREAM_SERVER_BIND);
if ($udp === false) {
    throw new RuntimeException("failed to bind UDP socket: ({$errno}) {$errstr}");
}
stream_set_blocking($udp, false);
$localName = stream_socket_get_name($udp, false);
$localEndpoint = is_string($localName) && $localName !== '' ? $localName : formatEndpoint($host, $port);
$localAddress = Address::fromString($localEndpoint);

fwrite(STDERR, "http3 echo server waiting on {$host}:{$port}\n");

$sessions = [];
$deadline = $timeoutMs > 0 ? microtime(true) + ($timeoutMs / 1000.0) : null;
while ($deadline === null || microtime(true) < $deadline) {
    // 1) Wait for a packet (or timer tick).
    [$packet, $from] = readUdpPacket($udp, $sessions);

    // 2) Apply received packet to a peer session.
    ingestIncomingPacket($sessions, $packet, $from, $localAddress, $cert, $key, $alpn);

    // 3) Advance every session: timers -> HTTP/3 events -> outbound datagrams.
    runAllSessions($sessions, $prefix, $udp);
}

if (is_resource($udp)) {
    fclose($udp);
}

function usage(): void
{
    fwrite(STDERR, <<<TXT
Usage:
  php examples/http3_server_echo.php <PORT> [<PRIVATE_KEY> <CERT>] [--address=ADDR]
                                     [--alpn=h3] [--prefix='echo: '] [--timeout-ms=0]

Port examples:
  php examples/http3_server_echo.php 4433
  php examples/http3_server_echo.php 8443 /tmp/nghttp3/server.key /tmp/nghttp3/server.crt --address=0.0.0.0

HTTP/3 curl test (-v):
  curl -v --http3 --insecure https://127.0.0.1:4433/ -d 'hello'
  curl -v --http3 --insecure https://127.0.0.1:4433/ -d 'again'

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

function formatEndpoint(string $host, int $port): string
{
    if (strpos($host, ':') !== false && strpos($host, '[') !== 0) {
        return "[{$host}]:{$port}";
    }

    return "{$host}:{$port}";
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

function readUdpPacket($udp, array $sessions): array
{
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

    return [$packet, $from];
}

function acceptPeerSession(
    string $packet,
    string $peer,
    Address $local,
    string $cert,
    string $key,
    string $alpn
): ?array {
    $remote = Address::fromString($peer);
    $dgram = new Datagram($packet, $remote, $local);

    try {
        $serverConn = ServerConnection::accept(
            $dgram,
            (new ServerConfig())
                ->withCertificate($cert, $key)
                ->withAlpn($alpn)
        );

        fwrite(STDERR, "accepted peer {$peer}\n");
        return [
            'conn' => $serverConn,
            'http3' => new Http3Connection($serverConn),
            'requestBodies' => [],
            'responded' => [],
        ];
    } catch (Throwable $e) {
        fwrite(STDERR, "accept warning for {$peer}: {$e->getMessage()}\n");
        return null;
    }
}

function recvPeerDatagram(
    array &$session,
    string $packet,
    string $peer,
    Address $local
): void {
    $serverConn = $session['conn'] ?? null;
    if (!$serverConn instanceof ServerConnection) {
        return;
    }

    $remote = Address::fromString($peer);
    $dgram = new Datagram($packet, $remote, $local);

    try {
        $serverConn->recv($dgram);
    } catch (Throwable $e) {
        fwrite(STDERR, "recv warning: {$e->getMessage()}\n");
    }
}

function ingestIncomingPacket(
    array &$sessions,
    $packet,
    $from,
    Address $local,
    string $cert,
    string $key,
    string $alpn
): void {
    if (!is_string($packet) || $packet === '' || !is_string($from) || $from === '') {
        return;
    }

    if (!array_key_exists($from, $sessions)) {
        $session = acceptPeerSession($packet, $from, $local, $cert, $key, $alpn);
        if ($session !== null) {
            $sessions[$from] = $session;
        }
        return;
    }

    recvPeerDatagram($sessions[$from], $packet, $from, $local);
}

function handleSessionEvents(array &$session, string $peer, string $prefix): void
{
    $http3 = $session['http3'] ?? null;
    if (!$http3 instanceof Http3Connection) {
        return;
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
}

function flushOutgoingDatagrams($udp, ServerConnection $serverConn, string $peer): void
{
    try {
        foreach ($serverConn->drainOutgoingDatagrams() as $outgoing) {
            stream_socket_sendto($udp, $outgoing->getPayload(), 0, $peer);
        }
    } catch (Throwable $e) {
        fwrite(STDERR, "drain warning: {$e->getMessage()}\n");
    }
}

function runSessionStep(array &$session, string $peer, string $prefix, $udp): bool
{
    $serverConn = $session['conn'] ?? null;
    if (!$serverConn instanceof ServerConnection) {
        return false;
    }

    try {
        $serverConn->handleTimers();
    } catch (Throwable $e) {
        fwrite(STDERR, "timeout warning: {$e->getMessage()}\n");
    }

    handleSessionEvents($session, $peer, $prefix);
    flushOutgoingDatagrams($udp, $serverConn, $peer);

    if ($serverConn->isClosed()) {
        fwrite(STDERR, "closed peer {$peer}\n");
        return true;
    }

    return false;
}

function runAllSessions(array &$sessions, string $prefix, $udp): void
{
    $closedPeers = [];
    foreach ($sessions as $peer => &$session) {
        if (runSessionStep($session, $peer, $prefix, $udp)) {
            $closedPeers[] = $peer;
        }
    }
    unset($session);

    foreach ($closedPeers as $closedPeer) {
        unset($sessions[$closedPeer]);
    }
}
