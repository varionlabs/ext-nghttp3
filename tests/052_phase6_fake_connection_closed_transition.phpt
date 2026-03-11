--TEST--
Phase 6 fake adapter: connection closed signal transitions to closed semantics
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo 'skip nghttp3 extension is not loaded';
}
?>
--FILE--
<?php
$fake = new Varion\Nghttp3\Testing\FakeQuicAdapter();
$conn = Varion\Nghttp3\Testing\Http3ConnectionFactory::fromFake($fake);
$conn->createRequestStream();

$fake->injectConnectionClosed(55);
$events = $conn->pollEvents();
var_dump(count($events) === 1);
var_dump(get_class($events[0]) === 'Varion\\Nghttp3\\Events\\GoawayReceived');
var_dump($events[0]->getErrorCode() === 55);
var_dump($conn->isClosing());

$fake->injectConnectionClosing(99);
var_dump(count($conn->pollEvents()) === 0);

$stateError = false;
try {
    $conn->createRequestStream();
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
