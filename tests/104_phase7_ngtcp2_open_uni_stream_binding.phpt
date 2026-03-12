--TEST--
Phase 7 integration: native nghttp3 path binds control/QPACK streams via ngtcp2 openUniStream
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
use Varion\Ngtcp2\Stream;

$quic = new Connection(new Address('127.0.0.1', 4433));
$http3 = new Varion\Nghttp3\Http3Connection($quic);

$s1 = $http3->createRequestStream();
var_dump($s1->getId() === 0);

$s1->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/one'],
]);

$u2 = $quic->getStream(2);
$u6 = $quic->getStream(6);
$u10 = $quic->getStream(10);
var_dump($u2 instanceof Stream);
var_dump($u6 instanceof Stream);
var_dump($u10 instanceof Stream);
var_dump($u2 !== null ? $u2->getId() === 2 : false);
var_dump($u6 !== null ? $u6->getId() === 6 : false);
var_dump($u10 !== null ? $u10->getId() === 10 : false);

$s2 = $http3->createRequestStream();
var_dump($s2->getId() === 4);

$s2->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/two'],
]);

var_dump($quic->getStream(14) === null);

$s1->submitData('a');
$s1->end();
$s2->submitData('b');
$s2->end();

$datagrams = $quic->flush();
$events = $http3->pollEvents();
var_dump(is_array($datagrams));
var_dump(is_array($events));
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
