# Settings

The system can be configured in several ways. Some of the configuration that the user can do are:

## Chunk Size

The device streams to the host the requested bytes. Those bytes are sent in fixed chunks.
The chunk size can affect the speed of the whole dump process, and the granularity.

- Default: 4KB
- CLI config command: esp32-dumper set chunk-size=<size_in_bytes>

