--TEST--
Phase 3 terminal rule: terminal event is emitted once, then stream readable is suppressed
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
$stream = $conn->createRequestStream();
$id = $stream->getId();

$fake->injectReset($id, 99);
$fake->injectReadable($id, "late-data", true);
$fake->injectConnectionClosing(42);

$events = $conn->pollEvents();
echo count($events), "\n";
echo get_class($events[0]), "\n";
echo $events[0]->getErrorCode(), "\n";
echo get_class($events[1]), "\n";
echo $events[1]->getErrorCode(), "\n";
var_dump($stream->isClosed());
var_dump($conn->isClosing());
echo count($conn->pollEvents()), "\n";
?>
--EXPECT--
2
Varion\Nghttp3\Events\StreamReset
99
Varion\Nghttp3\Events\GoawayReceived
42
bool(true)
bool(true)
0
