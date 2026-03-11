--TEST--
Phase 6 integration: ngtcp2 path propagates reset and goaway events
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
}
if (!extension_loaded('ngtcp2')) {
    echo 'skip ngtcp2 extension is not loaded';
}
?>
--FILE--
<?php

use Varion\Ngtcp2\Address;
use Varion\Ngtcp2\Connection;

$quic = new Connection(new Address('127.0.0.1', 4433));
$http3 = new Varion\Nghttp3\Http3Connection($quic);

$stream = $http3->createRequestStream();
$stream->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/reset'],
]);
$stream->submitData('x');
$stream->reset(123);

$events = $http3->pollEvents();
$reset = null;
foreach ($events as $event) {
    if ($event instanceof Varion\Nghttp3\Events\StreamReset) {
        $reset = $event;
        break;
    }
}

var_dump($reset instanceof Varion\Nghttp3\Events\StreamReset);
var_dump($reset !== null ? $reset->getStreamId() === $stream->getId() : false);
var_dump($reset !== null ? $reset->getErrorCode() === 123 : false);
var_dump($stream->isClosed());

$quic->close(77);
$events = $http3->pollEvents();
$goaway = null;
foreach ($events as $event) {
    if ($event instanceof Varion\Nghttp3\Events\GoawayReceived) {
        $goaway = $event;
        break;
    }
}

var_dump($goaway instanceof Varion\Nghttp3\Events\GoawayReceived);
var_dump($goaway !== null ? $goaway->getErrorCode() === 77 : false);
var_dump($http3->isClosing());

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
