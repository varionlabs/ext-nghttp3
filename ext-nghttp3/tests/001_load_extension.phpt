--TEST--
nghttp3 extension is loadable
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo "skip nghttp3 extension is not loaded";
}
?>
--FILE--
<?php
echo extension_loaded('nghttp3') ? "ok\n" : "ng\n";
?>
--EXPECT--
ok
