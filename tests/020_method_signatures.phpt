--TEST--
Http3Connection and Http3RequestStream method signatures match MVP skeleton
--SKIPIF--
<?php
if (!extension_loaded('nghttp3')) {
    echo "skip nghttp3 extension is not loaded";
}
?>
--FILE--
<?php
$ctor = new ReflectionMethod('Varion\\Nghttp3\\Http3Connection', '__construct');
$ctorParam = $ctor->getParameters()[0];
echo $ctorParam->getType()->getName(), "\n";

$create = new ReflectionMethod('Varion\\Nghttp3\\Http3Connection', 'createRequestStream');
echo $create->getReturnType()->getName(), "\n";

$poll = new ReflectionMethod('Varion\\Nghttp3\\Http3Connection', 'pollEvents');
echo $poll->getReturnType()->getName(), "\n";

$isClosing = new ReflectionMethod('Varion\\Nghttp3\\Http3Connection', 'isClosing');
echo $isClosing->getReturnType()->getName(), "\n";

$close = new ReflectionMethod('Varion\\Nghttp3\\Http3Connection', 'close');
echo $close->getReturnType()->getName(), "\n";

$getId = new ReflectionMethod('Varion\\Nghttp3\\Http3RequestStream', 'getId');
echo $getId->getReturnType()->getName(), "\n";

$submitHeaders = new ReflectionMethod('Varion\\Nghttp3\\Http3RequestStream', 'submitHeaders');
echo $submitHeaders->getParameters()[0]->getType()->getName(), "\n";
echo $submitHeaders->getReturnType()->getName(), "\n";

$submitData = new ReflectionMethod('Varion\\Nghttp3\\Http3RequestStream', 'submitData');
echo $submitData->getParameters()[0]->getType()->getName(), "\n";
echo $submitData->getReturnType()->getName(), "\n";

$end = new ReflectionMethod('Varion\\Nghttp3\\Http3RequestStream', 'end');
echo $end->getReturnType()->getName(), "\n";

$reset = new ReflectionMethod('Varion\\Nghttp3\\Http3RequestStream', 'reset');
echo $reset->getParameters()[0]->isOptional() ? "1\n" : "0\n";
echo $reset->getReturnType()->getName(), "\n";

$isClosed = new ReflectionMethod('Varion\\Nghttp3\\Http3RequestStream', 'isClosed');
echo $isClosed->getReturnType()->getName(), "\n";
?>
--EXPECT--
Varion\Ngtcp2\QuicConnection
Varion\Nghttp3\Http3RequestStream
array
bool
void
int
array
void
string
void
void
1
void
bool
