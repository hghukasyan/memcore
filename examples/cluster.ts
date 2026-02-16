#!/usr/bin/env node
/**
 * Multi-worker example: shared cache with Node.js cluster module.
 * Workers share the same POSIX shared memory cache.
 *
 * Run from repository root: node dist/examples/cluster.js
 * Or: npm run example
 */
import cluster from 'cluster';
import type { Worker } from 'cluster';
import os from 'os';
import * as cache from '../index';

const SHM_NAME = 'memcore_example';
const CACHE_SIZE_MB = 4;
const numCPUs = os.cpus().length;

if (cluster.isPrimary) {
  cache.init(SHM_NAME, CACHE_SIZE_MB);

  console.log(`Primary: cache initialized (${CACHE_SIZE_MB} MB)`);
  cache.set('greeting', 'Hello from primary');
  cache.set('worker_count', String(numCPUs));

  for (let i = 0; i < Math.min(3, numCPUs); i++) {
    cluster.fork();
  }

  cluster.on('exit', (worker: Worker) => {
    console.log(`Worker ${worker.process.pid} exited`);
  });

  setTimeout(() => {
    const s = cache.stats();
    console.log('Primary stats:', s);
    console.log('Primary get(shared_key):', cache.get('shared_key'));
    process.exit(0);
  }, 2500);
} else {
  cache.init(SHM_NAME, CACHE_SIZE_MB);

  const id = cluster.worker!.id;
  const pid = process.pid;

  console.log(`Worker ${id} (PID ${pid}): get(greeting) =`, cache.get('greeting'));

  cache.set(`worker_${id}`, `data from worker ${id}`);
  cache.set('shared_key', `written by worker ${id}`);

  const s = cache.stats();
  console.log(`Worker ${id} stats:`, s);

  if (id === 1) {
    cache.set('deleted_later', 'value');
    console.log('Worker 1 set deleted_later');
  }
  if (id === 2) {
    const ok = cache.del('deleted_later');
    console.log('Worker 2 del(deleted_later):', ok);
  }
}
