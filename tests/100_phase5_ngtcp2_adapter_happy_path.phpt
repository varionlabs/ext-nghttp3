--TEST--
Phase 5 integration: Http3Connection can drive request send path via Varion\\Ngtcp2\\Connection
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
    [':path', '/'],
]);
$stream->submitData('abc');
$stream->end();

$datagrams = $quic->flush();
$events = $http3->pollEvents();

var_dump($stream->getId() === 0);
var_dump(is_array($datagrams));
var_dump(is_array($events));
var_dump($stream->isClosed());
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
