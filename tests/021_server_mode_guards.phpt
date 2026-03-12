--TEST--
Http3Connection server mode disallows createRequestStream and supports getRequestStream lookup
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo "skip nghttp3 extension is not loaded";
}
if (extension_loaded('ngtcp2')) {
    echo "skip this test uses a local stub for Varion\\\\Ngtcp2\\\\ServerConnection";
}
?>
--FILE--
<?php

namespace Varion\Ngtcp2 {
    final class ServerConnection
    {
        public bool $closed = false;

        public function pollEvents(): array
        {
            return [];
        }

        public function getStream(int $streamId): ?object
        {
            return null;
        }

        public function close(): void
        {
            $this->closed = true;
        }
    }
}

namespace {
    $quic = new \Varion\Ngtcp2\ServerConnection();
    $http3 = new Varion\Nghttp3\Http3Connection($quic);

    $stateError = false;
    try {
        $http3->createRequestStream();
    } catch (Varion\Nghttp3\Http3StateException $e) {
        $stateError = true;
    }

    var_dump($stateError);
    var_dump($http3->getRequestStream(0) === null);

    $http3->close();
    var_dump($quic->closed);
}
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
