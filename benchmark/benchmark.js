#!/usr/bin/env node
/**
 * Simple throughput and latency benchmark for memcore.
 * Run from repository root: node benchmark/benchmark.js
 * Or: npm run benchmark
 */
const path = require('path');
const cache = require(path.join(__dirname, '..', 'index.js'));

const SHM_NAME = 'memcore_bench';
const CACHE_SIZE_MB = 16;
const WARMUP = 5000;
const RUN = 100000;

function ns() {
  const [s, n] = process.hrtime();
  return s * 1e9 + n;
}

function run(name, fn, iterations) {
  const start = ns();
  for (let i = 0; i < iterations; i++) fn(i);
  const elapsed = (ns() - start) / 1e6;
  const opsPerSec = (iterations / elapsed) * 1000;
  const avgMs = elapsed / iterations;
  return { name, opsPerSec: opsPerSec | 0, avgMs, elapsed };
}

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  const idx = Math.ceil((p / 100) * sorted.length) - 1;
  return sorted[Math.max(0, idx)];
}

function main() {
  cache.init(SHM_NAME, CACHE_SIZE_MB);
  cache.clear();

  const keys = Array.from({ length: 1000 }, (_, i) => `key_${i}`);
  const value = 'v'.repeat(100);

  // Warmup
  run('warmup set', (i) => cache.set(keys[i % keys.length], value), WARMUP);
  run('warmup get', (i) => cache.get(keys[i % keys.length]), WARMUP);

  // Set throughput
  const setResult = run('set', (i) => cache.set(keys[i % keys.length], value + i), RUN);
  // Get throughput
  const getResult = run('get', (i) => cache.get(keys[i % keys.length]), RUN);

  // Latency samples (microseconds)
  const setLatencies = [];
  const getLatencies = [];
  for (let i = 0; i < 10000; i++) {
    const k = keys[i % keys.length];
    const t0 = ns();
    cache.set(k, value + i);
    setLatencies.push((ns() - t0) / 1000);
    const t1 = ns();
    cache.get(k);
    getLatencies.push((ns() - t1) / 1000);
  }
  setLatencies.sort((a, b) => a - b);
  getLatencies.sort((a, b) => a - b);

  console.log('');
  console.log('memcore benchmark (single process)');
  console.log('  Cache:', CACHE_SIZE_MB, 'MB,', RUN, 'ops per run');
  console.log('');
  console.log('Operation | Throughput (ops/s) | Avg latency (ms)');
  console.log('----------|--------------------|------------------');
  console.log('set       | %s | %s', String(setResult.opsPerSec).padStart(18), setResult.avgMs.toFixed(4));
  console.log('get       | %s | %s', String(getResult.opsPerSec).padStart(18), getResult.avgMs.toFixed(4));
  console.log('');
  console.log('Latency percentiles (μs):');
  console.log('          | p50    | p95    | p99');
  console.log('----------|--------|--------|--------');
  console.log('set       | %s | %s | %s', String(percentile(setLatencies, 50).toFixed(1)).padStart(6), String(percentile(setLatencies, 95).toFixed(1)).padStart(6), percentile(setLatencies, 99).toFixed(1));
  console.log('get       | %s | %s | %s', String(percentile(getLatencies, 50).toFixed(1)).padStart(6), String(percentile(getLatencies, 95).toFixed(1)).padStart(6), percentile(getLatencies, 99).toFixed(1));
  console.log('');
}

main();
