PHP_ARG_ENABLE([nghttp3],
  [whether to enable nghttp3 support],
  [AS_HELP_STRING([--enable-nghttp3], [Enable nghttp3 extension])],
  [no])

if test "$PHP_NGHTTP3" != "no"; then
  AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
  if test "$PKG_CONFIG" = "no"; then
    AC_MSG_ERROR([pkg-config is required to build nghttp3 extension])
  fi

  if ! $PKG_CONFIG --exists libnghttp3; then
    AC_MSG_ERROR([Missing dependency: libnghttp3 is required])
  fi

  NGHTTP3_CFLAGS=`$PKG_CONFIG --cflags libnghttp3`
  NGHTTP3_LIBS=`$PKG_CONFIG --libs libnghttp3`

  PHP_EVAL_INCLINE([$NGHTTP3_CFLAGS])
  PHP_EVAL_LIBLINE([$NGHTTP3_LIBS], [NGHTTP3_SHARED_LIBADD])
  PHP_SUBST([NGHTTP3_SHARED_LIBADD])

  PHP_NEW_EXTENSION([nghttp3], [
    nghttp3.c
    src/exception.c
    src/event.c
    src/http3_request_stream.c
    src/http3_connection.c
    src/testing.c
  ], [$ext_shared])
fi
