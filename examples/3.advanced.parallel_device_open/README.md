# Parallel device open

This hardware diagnostic opens Orbbec devices concurrently through the public C API. It does not
start streams or change device configuration.

```text
ob_parallel_device_open --all
ob_parallel_device_open --index 0 --copies 4
ob_parallel_device_open --address 192.168.1.10 --port 8090 --copies 4
ob_parallel_device_open --address 192.168.1.10 --port 8090 --copies 4 \
  --disable-network-enumeration
ob_parallel_device_open --default-pipeline --copies 4
ob_parallel_device_open --access-mismatch 0
```

All worker calls use a shared start gate. Returned handles remain alive until every worker finishes,
so same-device tests exercise the SDK's live-device cache.
