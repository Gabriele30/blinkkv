# BlinkKV

**A tiny in-memory key-value store written in C, optimized for expiring data.**

> Redis is general-purpose. BlinkKV is built for things that disappear.

BlinkKV is an experimental key-value database focused on short-lived data such as sessions, OTPs, rate limits, temporary locks and volatile API cache entries.

The goal is not to replace Redis. The goal is to explore a minimal database design where expiration is a first-class feature.

## Why BlinkKV?

Modern applications generate huge amounts of temporary data:

- session tokens
- login OTPs
- API rate limits
- temporary locks
- cooldowns
- volatile cache entries
- short-lived job state

Most key-value stores support TTL, but they are usually general-purpose systems. BlinkKV starts from a different assumption:

**every key is meant to disappear.**

## Planned features

- In-memory key-value storage
- TTL-first design
- Timing wheel expiration engine
- Simple TCP server
- Minimal text protocol
- CLI client
- Benchmark tool
- Expiration storm benchmark

## Project status

BlinkKV is currently in early development.

The first milestone is a working in-memory store with:

- `SET key value ttl`
- `GET key`
- `DEL key`
- `EXISTS key`
- `TTL key`
- `PING`
- `STATS`

## Build

```bash
make
```

## Run

```bash
./bin/blinkkv-server
```

## Example

```bash
./bin/blinkkv-cli set session:123 "hello" 30000
./bin/blinkkv-cli get session:123
./bin/blinkkv-cli ttl session:123
```

## Roadmap

### v0.1

- Basic in-memory hash table
- TTL support
- Simple expiration loop
- CLI client
- Basic benchmark tool

### v0.2

- Timing wheel expiration engine
- Expiration storm benchmark
- Improved memory stats

### v0.3

- TCP server
- Text protocol
- Concurrent clients

### v0.4

- Binary protocol
- Sharded store
- Performance tuning

### v0.5

- Optional persistence
- Snapshot support
- Docker image

## Philosophy

BlinkKV is intentionally small.

No SQL.  
No complex data structures.  
No clustering in the first versions.  
No attempt to be a Redis clone.

Just fast temporary keys.

## License

MIT