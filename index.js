/**
 * JavaScript wrapper for memcore native addon.
 * Lock-free shared in-memory cache across Node.js worker processes.
 * Supports TTL, atomic incr/decr, and eviction when full.
 */

const addon = require('./build/Release/memcore.node');

const KEY_MAX = 64;
const VALUE_MAX = 256;

function assertInit() {
  try {
    addon.stats();
  } catch (_) {
    throw new Error('Cache not initialized. Call init(name, sizeMB) first.');
  }
}

function ensureString(val, name, maxLen) {
  if (typeof val === 'string') return val;
  if (typeof val !== 'string' && val !== undefined && val !== null)
    throw new TypeError(`${name} must be a string, got ${typeof val}`);
  return String(val);
}

function keyValid(key, fnName) {
  const k = ensureString(key, 'key', KEY_MAX);
  const len = Buffer.byteLength(k, 'utf8');
  if (len === 0) throw new Error(`${fnName}: key cannot be empty`);
  if (len >= KEY_MAX) throw new Error(`${fnName}: key must be at most ${KEY_MAX - 1} bytes (UTF-8), got ${len}`);
  return k;
}

/**
 * Initialize the shared cache. Must be called before any other operation.
 * @param {string} name - POSIX shared memory name (e.g. 'mycache' -> /mycache)
 * @param {number} sizeMB - Size of the cache in megabytes (integer >= 1)
 */
function init(name, sizeMB) {
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
 * @param {string} key - Key (max 63 bytes UTF-8)
 * @param {string} value - Value (max 255 bytes UTF-8)
 * @param {number} [ttlMs] - Optional TTL in milliseconds; entry expires after ttlMs
 * @returns {boolean} true if set, false if table full or key/value too long
 */
function set(key, value, ttlMs) {
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
 * @param {string} key - Key
 * @returns {string|null} value or null if not found / expired
 */
function get(key) {
  assertInit();
  const k = keyValid(key, 'get');
  return addon.get(k);
}

/**
 * Delete key from cache.
 * @param {string} key - Key
 * @returns {boolean} true if deleted, false if not found
 */
function del(key) {
  assertInit();
  const k = keyValid(key, 'del');
  return addon.delete(k);
}

/**
 * Clear all entries. All processes see the cleared state.
 */
function clear() {
  assertInit();
  addon.clear();
}

/**
 * Get cache statistics.
 * @returns {{ capacity: number, count: number, keyMaxBytes: number, valueMaxBytes: number }}
 */
function stats() {
  assertInit();
  return addon.stats();
}

/**
 * Atomic increment. Creates a numeric slot with value delta if key is missing.
 * Key must not exist as a string value.
 * @param {string} key - Key
 * @param {number} delta - Integer delta (64-bit safe)
 * @returns {number} new value after increment
 */
function incr(key, delta) {
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
 * @param {string} key - Key
 * @param {number} delta - Integer delta (64-bit safe)
 * @returns {number} new value after decrement
 */
function decr(key, delta) {
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
function close() {
  try {
    addon.close();
  } catch (_) {
    /* already closed or never inited */
  }
}

/* Cleanup on exit so we don't leave the fd open */
if (typeof process === 'object' && process.on) {
  process.on('exit', close);
}

module.exports = {
  init,
  set,
  get,
  del,
  clear,
  stats,
  incr,
  decr,
  close,
  KEY_MAX,
  VALUE_MAX,
};
