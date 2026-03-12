# nghttp3 PHPT test execution

## 1. Default suite (no ngtcp2 extension)

Run all PHPT tests with the bundled `nghttp3` extension.
Integration tests that require `ngtcp2` are skipped.

```bash
NO_INTERACTION=1 REPORT_EXIT_STATUS=1 make test TESTS='tests/*.phpt'
```

## 2. ngtcp2-enabled integration suite

When `ext-ngtcp2` is built locally, load it explicitly and run ngtcp2-path tests.

```bash
NO_INTERACTION=1 REPORT_EXIT_STATUS=1 \
TEST_PHP_ARGS="-n -d extension=/home/masakielastic/develop/ext-nghttp3/ext-ngtcp2/modules/ngtcp2.so" \
make test TESTS='tests/100_phase5_ngtcp2_adapter_happy_path.phpt tests/102_phase6_ngtcp2_multi_stream_and_close.phpt tests/103_phase6_ngtcp2_reset_and_goaway.phpt tests/104_phase7_ngtcp2_open_uni_stream_binding.phpt'
```

## 3. Suggested CI split

- Job A (`nghttp3-default`): run the full suite without `ngtcp2` (expects `100/102/103/104` to SKIP).
- Job B (`nghttp3-with-ngtcp2`): build/load `ext-ngtcp2` and run `100/102/103/104` as required PASS.
