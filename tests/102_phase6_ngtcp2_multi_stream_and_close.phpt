--TEST--
Phase 6 integration: ngtcp2 path supports multiple request streams and idempotent close
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

$s1 = $http3->createRequestStream();
$s2 = $http3->createRequestStream();

$s1->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/one'],
]);
$s1->submitData('a');
$s1->end();

$s2->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/two'],
]);
$s2->submitData('b');
$s2->end();

$datagrams = $quic->drainOutgoingDatagrams();
$events = $http3->pollEvents();

var_dump($s1->getId() === 0);
var_dump($s2->getId() === 4);
var_dump(is_array($datagrams));
var_dump(is_array($events));
var_dump($s1->isClosed());
var_dump($s2->isClosed());

$http3->close();
$http3->close();
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
