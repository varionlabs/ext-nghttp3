--TEST--
Phase 8 integration: Http3Connection can parse request on ServerConnection and send response
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
    return;
}
if (!extension_loaded('ngtcp2')) {
    echo 'skip ngtcp2 extension is not loaded';
    return;
}
if (!is_executable('/usr/bin/openssl')) {
    echo 'skip /usr/bin/openssl is not available';
    return;
}
$probe = @stream_socket_server('udp://127.0.0.1:0', $errno, $errstr, STREAM_SERVER_BIND);
if ($probe === false) {
    echo 'skip udp bind is not available in this environment';
    return;
}
fclose($probe);
?>
--FILE--
<?php

declare(strict_types=1);

use Varion\Ngtcp2\Address;
use Varion\Ngtcp2\Connection;
use Varion\Ngtcp2\Datagram;
use Varion\Ngtcp2\ServerConnection;
use Varion\Nghttp3\Events\DataReceived;
use Varion\Nghttp3\Events\HeadersReceived;
use Varion\Nghttp3\Events\RequestCompleted;

function parseAddr(string $peer): Address
{
    if (preg_match('/^\[(.+)\]:(\d+)$/', $peer, $m) === 1) {
        return new Address($m[1], (int)$m[2]);
    }
    $pos = strrpos($peer, ':');
    if ($pos === false) {
        throw new RuntimeException("cannot parse address: {$peer}");
    }
    return new Address(substr($peer, 0, $pos), (int)substr($peer, $pos + 1));
}

$dir = sys_get_temp_dir() . '/nghttp3-server-' . bin2hex(random_bytes(4));
$cert = $dir . '/server.crt';
$key = $dir . '/server.key';
$serverSock = null;
$clientSock = null;

if (!mkdir($dir, 0700, true) && !is_dir($dir)) {
    throw new RuntimeException("failed to create temp dir: {$dir}");
}

try {
    $cmd = sprintf(
        '/usr/bin/openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 1 -subj %s -keyout %s -out %s',
        escapeshellarg('/CN=127.0.0.1'),
        escapeshellarg($key),
        escapeshellarg($cert)
    );
    exec($cmd . ' 2>/dev/null', $out, $rc);
    if ($rc !== 0) {
        throw new RuntimeException('openssl certificate generation failed');
    }

    $serverSock = stream_socket_server('udp://127.0.0.1:0', $errno, $errstr, STREAM_SERVER_BIND);
    if ($serverSock === false) {
        throw new RuntimeException("failed to bind UDP server socket: ({$errno}) {$errstr}");
    }
    stream_set_blocking($serverSock, false);

    $serverName = stream_socket_get_name($serverSock, false);
    if (!is_string($serverName) || $serverName === '') {
        throw new RuntimeException('failed to resolve UDP server socket name');
    }
    $serverAddr = parseAddr($serverName);

    $clientSock = stream_socket_client(
        sprintf('udp://%s:%d', $serverAddr->getHost(), $serverAddr->getPort()),
        $errno,
        $errstr,
        1,
        STREAM_CLIENT_CONNECT
    );
    if ($clientSock === false) {
        throw new RuntimeException("failed to connect UDP client socket: ({$errno}) {$errstr}");
    }
    stream_set_blocking($clientSock, false);

    $clientConn = new Connection($serverAddr);
    $clientHttp3 = new Varion\Nghttp3\Http3Connection($clientConn);
    $clientStream = $clientHttp3->createRequestStream();
    $clientStream->submitHeaders([
        [':method', 'POST'],
        [':scheme', 'https'],
        [':authority', 'example.com'],
        [':path', '/echo'],
    ]);
    $clientStream->submitData('hello');
    $clientStream->end();

    $serverConn = null;
    $serverHttp3 = null;
    $serverPeer = null;

    $serverRequestPayload = '';
    $serverRequestCompleted = false;
    $serverResponded = false;
    $clientResponsePayload = '';
    $clientResponseCompleted = false;

    $deadline = microtime(true) + 8.0;
    while (microtime(true) < $deadline && !$clientConn->isClosed()) {
        foreach ($clientConn->flush() as $dgram) {
            stream_socket_sendto($clientSock, $dgram->getPayload());
        }

        if ($serverConn instanceof ServerConnection && is_string($serverPeer) && $serverPeer !== '') {
            foreach ($serverConn->flush() as $dgram) {
                stream_socket_sendto($serverSock, $dgram->getPayload(), 0, $serverPeer);
            }
        }

        $packet = stream_socket_recvfrom($serverSock, 65535, 0, $peer);
        if (is_string($packet) && $packet !== '' && is_string($peer) && $peer !== '') {
            $serverPeer = $peer;
            $remote = parseAddr($peer);
            $local = $serverAddr;
            $dgram = new Datagram($packet, $remote, $local);

            if (!$serverConn instanceof ServerConnection) {
                $serverConn = ServerConnection::accept($dgram, $local, [
                    'certFile' => $cert,
                    'keyFile' => $key,
                    'alpn' => 'h3',
                ]);
                $serverHttp3 = new Varion\Nghttp3\Http3Connection($serverConn);
            } else {
                $serverConn->recv($dgram);
            }
        } elseif ($serverConn instanceof ServerConnection) {
            $serverConn->onTimeout();
        }

        $cpkt = stream_socket_recvfrom($clientSock, 65535, 0, $peer2);
        if (is_string($cpkt) && $cpkt !== '') {
            $clientConn->recv(new Datagram($cpkt, $serverAddr));
        } else {
            $clientConn->onTimeout();
        }

        if ($serverHttp3 instanceof Varion\Nghttp3\Http3Connection) {
            foreach ($serverHttp3->pollEvents() as $event) {
                if ($event instanceof DataReceived) {
                    $serverRequestPayload .= $event->getPayload();
                    continue;
                }

                if ($event instanceof HeadersReceived && !$serverResponded) {
                    $stream = $serverHttp3->getRequestStream($event->getStreamId());
                    if ($stream !== null) {
                        $stream->submitHeaders([
                            [':status', '200'],
                            ['content-type', 'text/plain'],
                        ]);
                        $stream->submitData('pong');
                        $stream->end();
                        $serverResponded = true;
                    }
                    continue;
                }

                if ($event instanceof RequestCompleted) {
                    $serverRequestCompleted = true;
                }
            }
        }

        foreach ($clientHttp3->pollEvents() as $event) {
            if ($event instanceof DataReceived) {
                $clientResponsePayload .= $event->getPayload();
                continue;
            }

            if ($event instanceof RequestCompleted) {
                $clientResponseCompleted = true;
                break 2;
            }
        }
    }

    var_dump($serverResponded);
    var_dump($serverRequestCompleted);
    var_dump($serverRequestPayload === 'hello');
    var_dump($clientResponseCompleted);
    var_dump($clientResponsePayload === 'pong');
} finally {
    if (is_resource($clientSock)) {
        fclose($clientSock);
    }
    if (is_resource($serverSock)) {
        fclose($serverSock);
    }
    @unlink($cert);
    @unlink($key);
    @rmdir($dir);
}
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
