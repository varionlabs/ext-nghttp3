--TEST--
Phase 4 state rule: createRequestStream is rejected after GOAWAY
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo "skip nghttp3 extension is not loaded";
}
?>
--FILE--
<?php
$fake = new Varion\Nghttp3\Testing\FakeQuicAdapter();
$conn = Varion\Nghttp3\Testing\Http3ConnectionFactory::fromFake($fake);
$conn->createRequestStream();

$fake->injectConnectionClosing(7);
$events = $conn->pollEvents();
echo get_class($events[0]), "\n";
var_dump($conn->isClosing());

try {
    $conn->createRequestStream();
    echo "no-exception\n";
} catch (Varion\Nghttp3\Http3StateException $e) {
    echo "state-exception\n";
}
?>
--EXPECT--
Varion\Nghttp3\Events\GoawayReceived
bool(true)
state-exception
