--TEST--
Phase 4 terminal rule: StreamReset terminal event is emitted once
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

$fake->injectReset($id, 1);
$fake->injectReset($id, 2);
$fake->injectReadable($id, 'after-reset', true);

$events = $conn->pollEvents();
echo count($events), "\n";
echo get_class($events[0]), "\n";
echo $events[0]->getErrorCode(), "\n";
var_dump($stream->isClosed());
echo count($conn->pollEvents()), "\n";
?>
--EXPECT--
1
Varion\Nghttp3\Events\StreamReset
1
bool(true)
0
