--TEST--
Phase 3 fake signal: StreamReadable is translated in Headers/Data/RequestCompleted order
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

$fake->injectReadable($id, "hello", true);

$events = $conn->pollEvents();
echo count($events), "\n";
echo get_class($events[0]), "\n";
echo get_class($events[1]), "\n";
echo get_class($events[2]), "\n";
echo $events[1]->getPayload(), "\n";
echo count($conn->pollEvents()), "\n";
?>
--EXPECT--
3
Varion\Nghttp3\Events\HeadersReceived
Varion\Nghttp3\Events\DataReceived
Varion\Nghttp3\Events\RequestCompleted
hello
0
