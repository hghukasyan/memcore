#!/usr/bin/env node
/**
 * Stress test for memcore: correctness and performance.
 *
 * - Multi-process via Node cluster
 * - High-concurrency randomized set/get/delete
 * - Correctness: pre-populate verification + read-your-writes
 * - Throughput and latency metrics
 * - Optional long-running stability mode (--stability)
 *
 * Usage (from repo root):
 *   node dist/test/stress-test.js [options]
 *   node dist/test/stress-test.js --workers 4 --ops 100000
 *   node dist/test/stress-test.js --stability --duration 60
 *
 * Options:
 *   --workers N     Number of worker processes (default: CPU count)
 *   --ops N         Operations per worker in normal run (default: 50000)
 *   --duration N    Seconds for stability run (default: 60)
 *   --stability     Long-running stability test
 *   --shm NAME      Shared memory name (default: memcore_stress)
 *   --size N        Cache size in MB (default: 8)
 *   --seed N        Random seed for reproducibility
 */

import cluster from 'cluster';
import type { Worker } from 'cluster';
import os from 'os';
import * as cache from '../index';

const SHM_NAME = 'memcore_stress';
const CACHE_SIZE_MB = 8;
const KEY_PREFIX = 'k';
const VALUE_PREFIX = 'v';

const { KEY_MAX, VALUE_MAX } = cache;

interface StressOpts {
  workers: number;
  ops: number;
  duration: number;
  stability: boolean;
  shm: string;
  size: number;
  seed: number;
}

function parseArgs(): StressOpts {
  const args = process.argv.slice(2);
  const opts: StressOpts = {
    workers: os.cpus().length,
    ops: 50000,
    duration: 60,
    stability: false,
    shm: SHM_NAME,
    size: CACHE_SIZE_MB,
    seed: (Date.now() >>> 0) ^ (Math.random() * 0x100000000),
  };
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--workers' && args[i + 1] != null) opts.workers = parseInt(args[i + 1], 10);
    else if (args[i] === '--ops' && args[i + 1] != null) opts.ops = parseInt(args[i + 1], 10);
    else if (args[i] === '--duration' && args[i + 1] != null) opts.duration = parseInt(args[i + 1], 10);
    else if (args[i] === '--stability') opts.stability = true;
    else if (args[i] === '--shm' && args[i + 1] != null) opts.shm = args[i + 1];
    else if (args[i] === '--size' && args[i + 1] != null) opts.size = parseInt(args[i + 1], 10);
    else if (args[i] === '--seed' && args[i + 1] != null) opts.seed = parseInt(args[i + 1], 10);
  }
  return opts;
}

function makeKey(i: number): string {
  return KEY_PREFIX + String(i);
}

function makeValue(i: number): string {
  const v = VALUE_PREFIX + String(i);
  return v.length <= VALUE_MAX - 1 ? v : v.slice(0, VALUE_MAX - 1);
}

function xorshift32(seed: number): () => number {
  let s = seed >>> 0;
  return function next() {
    s ^= s << 13;
    s ^= s >>> 17;
    s ^= s << 5;
    return (s >>> 0) % 0x100000000;
  };
}

function percentile(sorted: number[], p: number): number {
  if (sorted.length === 0) return 0;
  const idx = Math.ceil((p / 100) * sorted.length) - 1;
  return sorted[Math.max(0, idx)];
}

interface WorkerOpts extends StressOpts {
  workerId: number;
  numWorkers: number;
  prePopulateKeys: number;
}

interface PrimaryResult {
  wallMs: number;
  errors: Array<{ worker: number; error: string }>;
  latencies: number[];
  throughputs: number[];
  prePopulateKeys: number;
  numWorkers: number;
  opts: StressOpts;
}

function runPrimary(opts: StressOpts): Promise<PrimaryResult> {
  cache.init(opts.shm, opts.size);
  cache.clear();

  const numWorkers = Math.max(1, Math.min(opts.workers, 64));
  const prePopulateKeys = Math.min(10000, (cache.stats().capacity || 10000) - 100);

  for (let i = 0; i < prePopulateKeys; i++) {
    cache.set(makeKey(i), makeValue(i));
  }
  console.log('Pre-populated', prePopulateKeys, 'keys, starting', numWorkers, 'workers...');

  return new Promise((resolve, reject) => {
    let workersDone = 0;
    const errors: Array<{ worker: number; error: string }> = [];
    const latencies: number[] = [];
    const throughputs: number[] = [];
    const startWall = Date.now();

    cluster.on('message', (worker: Worker, msg: { type?: string; error?: string; latencies?: number[]; throughput?: number }) => {
      if (msg.type === 'done') {
        workersDone++;
        if (msg.error) errors.push({ worker: worker.id!, error: msg.error });
        if (msg.latencies) {
          for (let i = 0; i < msg.latencies.length; i++) latencies.push(msg.latencies[i]);
        }
        if (msg.throughput != null) throughputs.push(msg.throughput);
        if (workersDone === numWorkers) {
          const wallMs = Date.now() - startWall;
          resolve({
            wallMs,
            errors,
            latencies,
            throughputs,
            prePopulateKeys,
            numWorkers,
            opts,
          });
        }
      }
    });

    cluster.on('exit', (worker: Worker, code: number | null) => {
      if (code !== 0 && workersDone < numWorkers) {
        reject(new Error(`Worker ${worker.id} exited with code ${code}`));
      }
    });

    for (let i = 0; i < numWorkers; i++) {
      cluster.fork({
        STRESS_WORKER: '1',
        STRESS_OPTS: JSON.stringify({
          ...opts,
          workerId: i,
          numWorkers,
          prePopulateKeys,
        } as WorkerOpts),
      });
    }
  });
}

function runWorker(): void {
  const opts: WorkerOpts = JSON.parse(process.env.STRESS_OPTS || '{}');
  cache.init(opts.shm, opts.size);

  const rand = xorshift32(opts.seed + opts.workerId * 7919);
  const keySpace = Math.max(100, opts.prePopulateKeys * 2);
  const latencies: number[] = [];
  const nsPerMs = 1e6;

  function randomKey(): string {
    return makeKey(rand() % keySpace);
  }

  function measure<T>(fn: () => T): T {
    const t0 = process.hrtime.bigint();
    const result = fn();
    const t1 = process.hrtime.bigint();
    latencies.push(Number(t1 - t0) / nsPerMs);
    return result;
  }

  if (opts.stability) {
    const deadline = Date.now() + opts.duration * 1000;
    let ops = 0;
    let errors = 0;
    const workerOnlyKey = KEY_PREFIX + 'w' + opts.workerId;
    while (Date.now() < deadline) {
      const op = rand() % 3;
      const k = op === 0 ? workerOnlyKey : randomKey();
      if (op === 0) {
        const v = makeValue(rand());
        measure(() => cache.set(k, v));
        const got = measure(() => cache.get(k));
        if (got !== v) errors++;
      } else if (op === 1) {
        measure(() => cache.get(k));
      } else {
        measure(() => cache.del(k));
      }
      ops++;
    }
    const elapsed = (opts.duration * 1000) / 1000;
    process.send!({
      type: 'done',
      throughput: ops / elapsed,
      latencies,
      error: errors ? `read-your-writes errors: ${errors}` : null,
    });
    return;
  }

  let readErrors = 0;
  const verifyCount = Math.min(5000, opts.prePopulateKeys);
  for (let i = 0; i < verifyCount; i++) {
    const idx = rand() % opts.prePopulateKeys;
    const k = makeKey(idx);
    const expected = makeValue(idx);
    const got = cache.get(k);
    if (got !== expected) {
      readErrors++;
    }
  }

  if (readErrors > 0) {
    process.send!({ type: 'done', error: `pre-populate read errors: ${readErrors}`, latencies: [] });
    return;
  }

  const targetOps = opts.ops;
  let setGetErrors = 0;
  let delGetErrors = 0;
  const opWeights = [40, 45, 15];
  const totalWeight = opWeights[0] + opWeights[1] + opWeights[2];

  for (let n = 0; n < targetOps; n++) {
    const r = rand() % totalWeight;
    let acc = 0;
    let op = 0;
    for (let i = 0; i < 3; i++) {
      acc += opWeights[i];
      if (r < acc) {
        op = i;
        break;
      }
    }

    const k = randomKey();
    if (op === 0) {
      const v = makeValue(rand());
      measure(() => cache.set(k, v));
      const got = measure(() => cache.get(k));
      if (got !== v) setGetErrors++;
    } else if (op === 1) {
      measure(() => cache.get(k));
    } else {
      measure(() => cache.del(k));
      const got = measure(() => cache.get(k));
      if (got !== null) delGetErrors++;
    }
  }

  const elapsedMs = latencies.length ? latencies.reduce((a, b) => a + b, 0) : 1;
  const totalOps = latencies.length;
  const throughput = totalOps / (elapsedMs / 1000);

  const errMsg = [setGetErrors && `set-get errors: ${setGetErrors}`, delGetErrors && `del-get errors: ${delGetErrors}`]
    .filter(Boolean)
    .join('; ') || null;

  process.send!({
    type: 'done',
    latencies,
    throughput,
    error: errMsg,
  });
}

function main(): void {
  const opts = parseArgs();

  if (opts.stability) {
    opts.ops = 0;
  }

  if (cluster.isPrimary) {
    console.log('memcore stress test');
    console.log('  workers:', opts.workers, '  stability:', opts.stability, '  duration:', opts.duration, 's  ops/worker:', opts.ops);
    console.log('  shm:', opts.shm, '  size:', opts.size, 'MB  seed:', opts.seed);
    console.log('');

    runPrimary(opts)
      .then((result) => {
        const { wallMs, errors, latencies, throughputs, numWorkers } = result;

        if (errors.length) {
          console.error('Correctness errors:');
          errors.forEach((e) => console.error('  Worker', e.worker, e.error));
        }

        const totalOps = latencies.length;
        const totalThroughput = totalOps / (wallMs / 1000);
        console.log('Results');
        console.log('  Wall time:     ', (wallMs / 1000).toFixed(2), 's');
        console.log('  Total ops:     ', totalOps);
        console.log('  Total throughput:', (totalThroughput | 0), 'ops/s');
        if (throughputs.length) {
          const avgWorker = throughputs.reduce((a, b) => a + b, 0) / throughputs.length;
          console.log('  Avg worker throughput:', (avgWorker | 0), 'ops/s');
        }

        if (latencies.length > 0) {
          const sorted = [...latencies].sort((a, b) => a - b);
          console.log('  Latency (ms):  p50:', percentile(sorted, 50).toFixed(3), '  p95:', percentile(sorted, 95).toFixed(3), '  p99:', percentile(sorted, 99).toFixed(3));
        }

        process.exit(errors.length ? 1 : 0);
      })
      .catch((err) => {
        console.error(err);
        process.exit(1);
      });
  } else {
    runWorker();
  }
}

main();
