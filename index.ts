/**
 * JavaScript wrapper for memcore native addon.
 * Lock-free shared in-memory cache across Node.js worker processes.
 * Supports TTL, atomic incr/decr, and eviction when full.
 */

import path from 'path';

const addon = require(path.join(__dirname, '..', 'build', 'Release', 'memcore.node')) as MemcoreAddon;

interface MemcoreAddon {
  init(name: string, sizeMB: number): void;
  set(key: string, value: string, ttlMs?: number): boolean;
  get(key: string): string | null;
  delete(key: string): boolean;
  clear(): void;
  stats(): Stats;
  incr(key: string, delta: number): number;
  decr(key: string, delta: number): number;
  close(): void;
}

export interface Stats {
  capacity: number;
  count: number;
  keyMaxBytes: number;
  valueMaxBytes: number;
}

export const KEY_MAX = 64;
export const VALUE_MAX = 256;

function assertInit(): void {
  try {
    addon.stats();
  } catch {
    throw new Error('Cache not initialized. Call init(name, sizeMB) first.');
  }
}

function ensureString(val: unknown, name: string, maxLen: number): string {
  if (typeof val === 'string') return val;
  if (typeof val !== 'string' && val !== undefined && val !== null)
    throw new TypeError(`${name} must be a string, got ${typeof val}`);
  return String(val);
}

function keyValid(key: unknown, fnName: string): string {
  const k = ensureString(key, 'key', KEY_MAX);
  const len = Buffer.byteLength(k, 'utf8');
  if (len === 0) throw new Error(`${fnName}: key cannot be empty`);
  if (len >= KEY_MAX) throw new Error(`${fnName}: key must be at most ${KEY_MAX - 1} bytes (UTF-8), got ${len}`);
  return k;
}

/**
 * Initialize the shared cache. Must be called before any other operation.
 */
export function init(name: string, sizeMB: number): void {
  if (typeof name !== 'string')
    throw new TypeError(`init(name, sizeMB): name must be a string, got ${typeof name}`);
  if (typeof sizeMB !== 'number' || Number.isNaN(sizeMB))
    throw new TypeError(`init(name, sizeMB): sizeMB must be a number, got ${typeof sizeMB}`);
  const mb = sizeMB | 0;
  if (mb < 1) throw new RangeError(`init(name, sizeMB): sizeMB must be >= 1, got ${mb}`);
  addon.init(name, mb);
}

/**
 * Set a key-value pair with optional TTL.
 * @returns true if set, false if table full or key/value too long
 */
export function set(key: string, value: string, ttlMs?: number): boolean {
  assertInit();
  const k = keyValid(key, 'set');
  const v = ensureString(value, 'value', VALUE_MAX);
  if (Buffer.byteLength(v, 'utf8') >= VALUE_MAX)
    throw new Error(`set: value must be at most ${VALUE_MAX - 1} bytes (UTF-8)`);
  if (ttlMs !== undefined) {
    if (typeof ttlMs !== 'number' || Number.isNaN(ttlMs))
      throw new TypeError('set(key, value, ttlMs): ttlMs must be a number');
    const t = ttlMs | 0;
    if (t < 0) throw new RangeError('set(key, value, ttlMs): ttlMs must be >= 0');
    return addon.set(k, v, t);
  }
  return addon.set(k, v);
}

/**
 * Get value for key. Expired TTL entries are treated as missing.
 */
export function get(key: string): string | null {
  assertInit();
  const k = keyValid(key, 'get');
  return addon.get(k);
}

/**
 * Delete key from cache.
 */
export function del(key: string): boolean {
  assertInit();
  const k = keyValid(key, 'del');
  return addon.delete(k);
}

/**
 * Clear all entries. All processes see the cleared state.
 */
export function clear(): void {
  assertInit();
  addon.clear();
}

/**
 * Get cache statistics.
 */
export function stats(): Stats {
  assertInit();
  return addon.stats();
}

/**
 * Atomic increment. Creates a numeric slot with value delta if key is missing.
 * Key must not exist as a string value.
 */
export function incr(key: string, delta: number | bigint): number {
  assertInit();
  const k = keyValid(key, 'incr');
  if (typeof delta !== 'number' && typeof delta !== 'bigint')
    throw new TypeError('incr(key, delta): delta must be a number or bigint');
  const d = Number(delta) | 0;
  const out = addon.incr(k, d);
  return typeof out === 'number' ? out : Number(out);
}

/**
 * Atomic decrement. Same as incr(key, -delta).
 */
export function decr(key: string, delta: number | bigint): number {
  assertInit();
  const k = keyValid(key, 'decr');
  if (typeof delta !== 'number' && typeof delta !== 'bigint')
    throw new TypeError('decr(key, delta): delta must be a number or bigint');
  const d = Number(delta) | 0;
  const out = addon.decr(k, d);
  return typeof out === 'number' ? out : Number(out);
}

/**
 * Release process-local mapping and file descriptor. Safe to call multiple times.
 * Other processes are unaffected. Call on process exit for clean shutdown.
 */
export function close(): void {
  try {
    addon.close();
  } catch {
    /* already closed or never inited */
  }
}

/* Cleanup on exit so we don't leave the fd open */
if (typeof process === 'object' && process.on) {
  process.on('exit', close);
}
