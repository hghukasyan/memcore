# memcore

**Lock-free shared in-memory cache for Node.js** — accessible across worker processes on the same machine. Uses POSIX shared memory, N-API, and atomic operations only. No Redis, no network, no locks.

---

## Why memcore?

- **No external services** — Skip Redis, Memcached, or any daemon. One `npm install` and you have a shared cache; no config, no ports, no network.
- **Sub-microsecond latency** — Data lives in mmap’d shared memory. Gets and sets are in-process; no serialization or TCP round-trips.
- **True cross-process sharing** — Cluster workers share the same cache via POSIX shared memory. Ideal for session storage, rate limiting, and counters without a separate cache server.
- **Lock-free design** — No mutexes or spinlocks in the hot path. Atomic operations and careful memory ordering keep throughput high and latency predictable under contention.
- **Predictable memory** — Fixed size and capacity at init; no runtime allocations in the addon. Easier to reason about and tune for production.
- **Small surface area** — Simple API (`init`, `set`, `get`, `del`, `incr`, `stats`, etc.). No connection pools, no retries, no client libraries to maintain.
- **Benchmarks** — Single-process: ~1.2M set/s and ~1.5M get/s; p50 latency under 1 µs. Run `npm run benchmark` on your machine for real numbers (see [Benchmarks](#benchmarks) below).

**Use memcore when** you need a fast, shared, in-memory cache for multiple Node.js workers on one machine and want to avoid external dependencies and network overhead.

---

## Features

- **Lock-free** — No mutexes; atomic operations and explicit memory ordering only.
- **Cross-process** — Multiple Node.js workers (e.g. `cluster`) share one cache via POSIX shared memory.
- **No external services** — No daemons, no sockets; everything lives in shared memory.
- **Ultra-low latency** — In-process mmap access; typical get/set in sub-microsecond range.
- **Fixed memory footprint** — Size and capacity determined at init; no runtime allocations in the addon.

---

## Installation

```bash
npm install memcore
```

### System requirements

- **Platform:** Linux (primary), macOS (same POSIX APIs; Windows not supported).
- **Node.js:** 18 or higher.
- **Build:** `node-gyp` (usually installed with npm). On Linux: `build-essential`, Python 3; on macOS: Xcode Command Line Tools.

The native addon is built automatically on `npm install` via `node-gyp rebuild`. The JavaScript API is written in TypeScript and provides type definitions (`dist/index.d.ts`) for consumers.

---

## Build from source

If you need to rebuild the addon or recompile TypeScript (e.g. after changing Node version):

```bash
npm run build
# or separately:
npm run build:addon
npm run build:ts
```

The binary is produced at `build/Release/memcore.node`. TypeScript compiles to `dist/`.

---

## Development & Testing

From the repo, install dependencies and build:

```bash
npm install
npm run build
```

Then run tests and examples:

| Command | Description |
|---------|-------------|
| `npm test` | Stress test (1 worker, 5000 ops) |
| `npm run stress` | Stress test (default workers/ops) |
| `npm run stress:stability` | Long-running stability test (60s) |
| `npm run benchmark` | Throughput and latency benchmark |
| `npm run example` | Multi-worker cluster example |

Stress test options: `--workers N`, `--ops N`, `--duration N`, `--stability`, `--shm NAME`, `--size N`, `--seed N`.

---

## Quick start

```javascript
const cache = require('memcore');

cache.init('mycache', 8);   // name, size in MB
cache.set('key', 'value');  // true
cache.get('key');           // 'value'
cache.del('key');           // true
cache.stats();              // { capacity, count, keyMaxBytes, valueMaxBytes }
cache.clear();
```

TypeScript:

```typescript
import * as cache from 'memcore';

cache.init('mycache', 8);
cache.set('key', 'value');
const value = cache.get('key');  // string | null
const s = cache.stats();         // { capacity, count, keyMaxBytes, valueMaxBytes }
```

From the repo: `npm run build` then `node dist/examples/quickstart.js`. Multi-worker: `npm run example` or `node dist/examples/cluster.js`.

---

## Multi-worker example

Using the Node.js **cluster** module so multiple workers share one cache:

```javascript
const cluster = require('cluster');
const cache = require('memcore');

const SHM_NAME = 'myapp_cache';
const CACHE_SIZE_MB = 8;

if (cluster.isPrimary) {
  cache.init(SHM_NAME, CACHE_SIZE_MB);
  cache.set('greeting', 'Hello from primary');
  cluster.fork();
  cluster.fork();
} else {
  cache.init(SHM_NAME, CACHE_SIZE_MB);
  console.log(cache.get('greeting'));  // 'Hello from primary'
  cache.set('worker_' + cluster.worker.id, 'data');
}
```

Run: `npm run build` then `node dist/examples/cluster.js` (from repo), or `npm run example`.

---

## API reference

| Method | Signature | Description |
|--------|-----------|-------------|
| `init` | `init(name, sizeMB)` | Create or attach to POSIX shm segment. `name` becomes `/name`. Call once per process before any other call. |
| `set` | `set(key, value [, ttlMs])` | Insert or update. Returns `true` on success, `false` if table full or key/value too long. Optional `ttlMs` for TTL in milliseconds. |
| `get` | `get(key)` | Returns string value or `null` if not found or expired. |
| `del` | `del(key)` | Remove key. Returns `true` if removed, `false` if not found. |
| `clear` | `clear()` | Set all slots to empty (visible to all processes). |
| `stats` | `stats()` | Returns `{ capacity, count, keyMaxBytes, valueMaxBytes }`. |
| `incr` | `incr(key, delta)` | Atomic increment. Creates numeric slot if missing. Returns new value. |
| `decr` | `decr(key, delta)` | Atomic decrement. Returns new value. |
| `close` | `close()` | Release process-local mapping and fd. Safe to call multiple times; called on process exit by default. |

**Constants:** `cache.KEY_MAX` (64), `cache.VALUE_MAX` (256) — UTF-8 byte limits; keys up to 63 bytes, values up to 255 bytes. Longer strings cause `set` to return `false` or throw from the JS wrapper.

---

## Benchmarks

Single-process throughput and latency (example run; your numbers will vary by CPU and load):

| Operation | Throughput (ops/s) | Latency (avg ms) |
|-----------|--------------------|------------------|
| set       | ~1,200,000         | ~0.0008          |
| get       | ~1,500,000         | ~0.0007          |

Latency percentiles (μs), single process:

|           | p50   | p95   | p99   |
|-----------|-------|-------|-------|
| set       | ~0.6  | ~1.2  | ~2    |
| get       | ~0.5  | ~1.0  | ~1.5  |

**Assumptions:** Linux or macOS, single process or multi-worker on one machine, 16 MB cache, 100-byte values. Run `npm run benchmark` on your hardware for representative results.

---

## Use cases

- **High-frequency backends** — Session or request metadata without network round-trips.
- **Rate limiting** — Atomic `incr`/`decr` across workers.
- **Session storage** — Shared session map for cluster workers.
- **Queue metadata** — Head/tail or counters in shared memory.
- **Real-time systems** — Low-latency state shared across processes.

---

## Limitations

- **Single machine only** — POSIX shared memory is local to the host; no network.
- **Fixed key/value sizes** — Key max 63 bytes UTF-8, value max 255 bytes UTF-8; fixed at design time.
- **No persistence** — Segment is in RAM; process crash or reboot clears data.
- **Linux / macOS** — Developed on Linux; macOS uses same POSIX APIs. Windows is not supported (no POSIX shm).

---

## Safety notes

- **Lifecycle:** Call `init(name, sizeMB)` once per process. The segment persists until all processes unmap it and the segment is removed (e.g. `shm_unlink`). If a process dies after claiming a slot but before committing, that slot can remain `RESERVED` until `clear()` or reinit.
- **Cleanup:** The addon registers an `exit` handler to call `close()`. For long-running servers this releases the fd; the mapping is process-local.
- **Concurrency:** Lock-free algorithms are used; multiple readers and writers are safe. No blocking except during initial init.

---

## License

MIT
