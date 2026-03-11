--TEST--
nghttp3 class and hierarchy skeleton exists
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo "skip nghttp3 extension is not loaded";
}
?>
--FILE--
<?php
$checks = [
    class_exists('Varion\\Nghttp3\\Http3Exception'),
    class_exists('Varion\\Nghttp3\\InvalidHttp3Operation'),
    class_exists('Varion\\Nghttp3\\Http3StateException'),
    class_exists('Varion\\Nghttp3\\NativeNghttp3Exception'),
    class_exists('Varion\\Nghttp3\\Event'),
    class_exists('Varion\\Nghttp3\\ConnectionEvent'),
    class_exists('Varion\\Nghttp3\\StreamEvent'),
    class_exists('Varion\\Nghttp3\\TerminalStreamEvent'),
    class_exists('Varion\\Nghttp3\\Events\\HeadersReceived'),
    class_exists('Varion\\Nghttp3\\Events\\DataReceived'),
    class_exists('Varion\\Nghttp3\\Events\\RequestCompleted'),
    class_exists('Varion\\Nghttp3\\Events\\StreamReset'),
    class_exists('Varion\\Nghttp3\\Events\\GoawayReceived'),
    class_exists('Varion\\Nghttp3\\Http3Connection'),
    class_exists('Varion\\Nghttp3\\Http3RequestStream'),
];

foreach ($checks as $ok) {
    echo $ok ? "1\n" : "0\n";
}
?>
--EXPECT--
1
1
1
1
1
1
1
1
1
1
1
1
1
1
1
