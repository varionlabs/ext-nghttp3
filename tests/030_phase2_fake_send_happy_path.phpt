--TEST--
Phase 2 fake adapter: createRequestStream submitHeaders submitData end
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
$streamId = $stream->getId();

$stream->submitHeaders([
    [':method', 'GET'],
    [':scheme', 'https'],
    [':authority', 'example.com'],
    [':path', '/'],
]);
$stream->submitData('abc');
$stream->end();

$writes = $fake->getWrites();

var_dump($streamId === 0);
var_dump(count($writes) === 2);
var_dump($writes[0]['streamId'] === $streamId);
var_dump(is_string($writes[0]['data']) && strlen($writes[0]['data']) > 0);
var_dump($writes[1]['streamId'] === $streamId);
var_dump($writes[1]['data'] === 'abc');
var_dump($fake->getFinishedStreams() === [$streamId]);
var_dump($stream->isClosed());
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
