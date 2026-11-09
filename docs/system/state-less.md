# Stateless

The device is stateless. Resume state lives on the host.

A dump can be written to a `.part` file plus a sidecar metadata file:

```text
dumps/router.bin.part
dumps/router.bin.json
```

Example metadata:

```json
{
  "protocol": 2,
  "jedec": "010215",
  "size": 4194304,
  "chunk_size": 65536,
  "chunks": [
    {"addr": 0, "bytes": 65536, "crc32": "1A2B3C4D", "ok": true},
    {"addr": 65536, "bytes": 65536, "crc32": "5E6F7A8B", "ok": true},
    {"addr": 131072, "bytes": 65536, "crc32": null, "ok": false}
  ]
}
```
The system has the capability to resume a corrupted dump from its metadata file.
