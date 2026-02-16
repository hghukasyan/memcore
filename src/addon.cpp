/**
 * Lock-free shared in-memory cache - Node.js N-API native addon
 * POSIX shared memory, atomic slot state+generation (ABA mitigation), fixed-size hash table
 *
 * Memory ordering: Writers publish with store(OCCUPIED, release) after writing key/value;
 * readers sync with load(acquire) and only read key/value after seeing OCCUPIED, so no torn
 * reads. CAS uses acq_rel so claim/release are globally ordered. Table-full: after probing
 * all num_slots we return false (bounded loop, no infinite retry).
 *
 * Crash robustness: (1) RESERVED slots are claimable by Set (writer may have crashed before
 * store OCCUPIED), so no slot stays permanently stuck. (2) INIT_IN_PROGRESS has bounded
 * wait; if initializer crashed we CAS it to 0 and re-run ensure_init so reattach always
 * succeeds. (3) Header is published only after slots are cleared and fence; init is
 * idempotent (single CAS winner, others wait or take over on stale state).
 */

#include <node_api.h>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <type_traits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

#define NAPI_VERSION 8


namespace memcore {

constexpr uint32_t MAGIC = 0x4D454D43;  /* "MEMC" */
/* Bump when header/slot layout or constants change; incompatible segments are reinitialized */
constexpr uint32_t LAYOUT_VERSION = 4;
constexpr size_t KEY_MAX = 64;
constexpr size_t VALUE_MAX = 256;
constexpr size_t CACHE_LINE = 64;
/* Slot: 1 cache line hot fields + 1 line key + 4 lines value = 6 lines */
constexpr size_t SLOT_SIZE = 6 * CACHE_LINE;  /* 384 */
constexpr size_t HEADER_SIZE = CACHE_LINE;    /* 64 */

constexpr uint8_t SLOT_TYPE_STRING = 0;
constexpr uint8_t SLOT_TYPE_NUMERIC = 1;

/*
 * LAYOUT DECISIONS (performance)
 * ------------------------------
 * - Cache-line alignment (64B): Header and each Slot are alignas(64) so the segment
 *   base and every slot start on a cache line; avoids extra lines and unaligned atomics.
 * - False sharing: state_gen is the only field on the first cache line of each Slot.
 *   Key and value live on subsequent lines so writers of state_gen do not invalidate
 *   cache lines used by readers of key/value (and vice versa).
 * - Spatial locality: key (64B) and value (256B) are contiguous. A probe loads
 *   state_gen, then key for compare, then value on match; key and value in sequence
 *   improve prefetch and reduce cache misses.
 * - Padding: Explicit _pad_state fills the first line after state_gen; no wasted
 *   padding between key and value. Header uses _pad_header to reach exactly 64 bytes.
 * - Atomics: state_gen is at offset 0 in a 64-aligned Slot → 8-byte aligned for
 *   atomic<uint64_t>. Header's initialized is 1-byte atomic with explicit padding.
 * - Trivially copyable: key and value regions are static_assert trivially copyable.
 *   The whole Slot is not (due to std::atomic); use memcpy only on key/value.
 */

enum SlotState : uint8_t {
  EMPTY = 0,
  RESERVED = 1,
  OCCUPIED = 2,
  DELETED = 3
};

/* state_gen: low 8 bits = state, high 56 bits = generation (avoids ABA on claim/delete) */
[[gnu::always_inline]] inline uint8_t state_from(uint64_t sg) { return (uint8_t)(sg & 0xFFu); }
[[gnu::always_inline]] inline uint64_t gen_from(uint64_t sg) { return sg >> 8; }
[[gnu::always_inline]] inline uint64_t pack_sg(uint8_t state, uint64_t gen) { return (gen << 8) | (uint64_t)state; }

/*
 * Slot layout (6 cache lines):
 *   Line 0: state_gen, expire_at_ms, slot_type, numeric_value (hot atomics + type)
 *   Line 1: key[64]
 *   Lines 2–5: value[256]
 * expire_at_ms: 0 = no TTL; else absolute ms since epoch. Expired => treat as DELETED on read.
 * slot_type: STRING (value[]) or NUMERIC (atomic numeric_value; incr/decr).
 */
struct alignas(CACHE_LINE) Slot {
  std::atomic<uint64_t> state_gen;
  std::atomic<uint64_t> expire_at_ms;   /* 0 = no TTL */
  uint8_t slot_type;                    /* SLOT_TYPE_STRING | SLOT_TYPE_NUMERIC */
  char _pad_type[7];
  std::atomic<int64_t> numeric_value;   /* used when slot_type == NUMERIC */
  char _pad_num[CACHE_LINE - 8 - 8 - 1 - 7 - 8];  /* to 64 */

  char key[KEY_MAX];
  char value[VALUE_MAX];
};
static_assert(sizeof(Slot) == SLOT_SIZE, "Slot must be 6 cache lines (384 bytes)");
static_assert(offsetof(Slot, key) == CACHE_LINE, "key starts at second cache line");
static_assert(offsetof(Slot, value) == 2 * CACHE_LINE, "value starts at third cache line");

/*
 * Header: single cache line. eviction_counter advances on full-table eviction (clock-style).
 */
struct alignas(CACHE_LINE) Header {
  uint32_t magic;
  uint32_t layout_version;
  uint32_t num_slots;
  uint32_t slot_size;
  std::atomic<uint8_t> initialized;
  char _pad_init[7];
  std::atomic<uint64_t> eviction_counter;  /* clock hand for eviction when full */
  char _pad_header[HEADER_SIZE - 4 * sizeof(uint32_t) - 1 - 7 - 8];
};
static_assert(sizeof(Header) == HEADER_SIZE, "Header must be exactly one cache line");

struct Cache {
  int fd;
  void* base;
  size_t total_size;
  Header* header;
  Slot* slots;
  uint32_t num_slots;
};

/* FNV-1a 32-bit: good distribution, cheap, no dependencies. Caller passes length of
 * null-terminated key; loop uses len only to avoid branch and extra load per byte. */
[[gnu::always_inline]] static inline uint32_t hash_key_fnv1a(const char* key, size_t len) {
  const uint32_t FNV_OFFSET = 2166136261u;
  const uint32_t FNV_PRIME = 16777619u;
  uint32_t h = FNV_OFFSET;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)key[i];
    h *= FNV_PRIME;
  }
  return h;
}

/* Second hash for double-hashing probe step; must be nonzero and spread well. */
[[gnu::always_inline]] static inline uint32_t hash_step(uint32_t h, uint32_t num_slots) {
  uint32_t step = (h * 2654435769u) % num_slots;  /* Knuth multiplier */
  return __builtin_expect(step == 0, 0) ? 1u : step;
}

static Cache* g_cache = nullptr;

/* Key equality: compare key_len+1 bytes (includes null). Faster than strncmp(..., KEY_MAX). */
[[gnu::always_inline]] static inline bool key_matches(const char* slot_key, const char* key_buf, size_t key_len) {
  return memcmp(slot_key, key_buf, key_len + 1) == 0;
}

/* Sentinel for initialized: 0 = not inited, 1 = ready, 2 = init in progress (CAS winner is reinitializing) */
constexpr uint8_t INIT_READY = 1;
constexpr uint8_t INIT_IN_PROGRESS = 2;

/* Max spins before assuming initializer crashed (INIT_IN_PROGRESS stuck). Enables reattach after crash. */
constexpr unsigned INIT_WAIT_MAX_SPIN = 10000000u;

/* Reinitialize segment in place. Call only from the process that won the CAS in ensure_init.
 * Header fields then slots then initialized=1; readers only trust header after seeing INIT_READY. */
static void reinitialize_segment(Cache* c) {
  c->header->magic = MAGIC;
  c->header->layout_version = LAYOUT_VERSION;
  c->header->num_slots = c->num_slots;
  c->header->slot_size = (uint32_t)SLOT_SIZE;
  c->header->eviction_counter.store(0, std::memory_order_release);
  for (uint32_t i = 0; i < c->num_slots; i++) {
    Slot* s = &c->slots[i];
    s->state_gen.store(0u, std::memory_order_release);
    s->expire_at_ms.store(0u, std::memory_order_release);
    s->slot_type = SLOT_TYPE_STRING;
    s->numeric_value.store(0, std::memory_order_release);
  }
  std::atomic_thread_fence(std::memory_order_release);
  c->header->initialized.store(INIT_READY, std::memory_order_release);
}

/* Only one process performs reinit; others spin until INIT_READY or take over on stale INIT_IN_PROGRESS. */
static bool ensure_init(Cache* c) {
  uint8_t expected = 0;
  if (c->header->initialized.compare_exchange_strong(expected, INIT_IN_PROGRESS,
                                                     std::memory_order_acq_rel)) {
    reinitialize_segment(c);
    return true;
  }
  if (expected == INIT_READY)
    return true;
  for (unsigned i = 0; i < INIT_WAIT_MAX_SPIN; i++) {
    if (c->header->initialized.load(std::memory_order_acquire) == INIT_READY)
      return true;
    sched_yield();
  }
  /* Stale INIT_IN_PROGRESS: initializer likely crashed. Reset so someone can reinit. */
  expected = INIT_IN_PROGRESS;
  if (c->header->initialized.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
    return ensure_init(c);  /* We reset; retry (we or another will win CAS 0→2 and reinit). */
  return true;  /* Another process reset or set INIT_READY; ensure ready and return. */
}

/* Wait until segment is ready. If INIT_IN_PROGRESS is stuck (initializer crashed), reset and run ensure_init. */
static void wait_initialized(Cache* c) {
  for (unsigned i = 0; i < INIT_WAIT_MAX_SPIN; i++) {
    uint8_t v = c->header->initialized.load(std::memory_order_acquire);
    if (v == INIT_READY)
      return;
    if (v == INIT_IN_PROGRESS && i > 100000u) {
      uint8_t expected = INIT_IN_PROGRESS;
      if (c->header->initialized.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        ensure_init(c);
        return;
      }
    }
    sched_yield();
  }
  /* Last resort: force reset and reinit so reattach after crash always succeeds. */
  uint8_t expected = INIT_IN_PROGRESS;
  c->header->initialized.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
  ensure_init(c);
}

static bool layout_compatible(const Header* h) {
  return h->magic == MAGIC && h->layout_version == LAYOUT_VERSION &&
         h->slot_size == (uint32_t)SLOT_SIZE;
}

static napi_value Init(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 2)
    return nullptr;

  size_t name_len;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &name_len) != napi_ok)
    return nullptr;
  std::string name(name_len + 1, '\0');
  if (napi_get_value_string_utf8(env, args[0], &name[0], (size_t)(name_len + 1), &name_len) != napi_ok)
    return nullptr;
  name.resize(name_len);

  int32_t size_mb = 0;
  if (napi_get_value_int32(env, args[1], &size_mb) != napi_ok || size_mb < 1)
    return nullptr;

  if (g_cache) {
    munmap(g_cache->base, g_cache->total_size);
    close(g_cache->fd);
    g_cache = nullptr;
  }

  std::string shm_name = name[0] == '/' ? name : "/" + name;
  int fd = shm_open(shm_name.c_str(), O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    napi_throw_error(env, nullptr, "shm_open failed");
    return nullptr;
  }

  size_t requested = HEADER_SIZE + (size_t)size_mb * 1024 * 1024;
  requested = (requested / SLOT_SIZE) * SLOT_SIZE;

  struct stat st;
  if (fstat(fd, &st) != 0) {
    if (ftruncate(fd, (off_t)requested) != 0) {
      close(fd);
      shm_unlink(shm_name.c_str());
      napi_throw_error(env, nullptr, "ftruncate failed");
      return nullptr;
    }
    st.st_size = requested;
  }
  size_t actual_size = (size_t)st.st_size;
  if (actual_size == 0) {
    if (ftruncate(fd, (off_t)requested) != 0) {
      close(fd);
      shm_unlink(shm_name.c_str());
      napi_throw_error(env, nullptr, "ftruncate failed");
      return nullptr;
    }
    actual_size = requested;
  } else if (actual_size < requested) {
    close(fd);
    napi_throw_error(env, nullptr, "shared memory size too small");
    return nullptr;
  }
  /* Use actual size (may be page-rounded by OS) but round down to slot boundary */
  size_t total_size = (actual_size / SLOT_SIZE) * SLOT_SIZE;
  if (total_size <= HEADER_SIZE) {
    close(fd);
    napi_throw_error(env, nullptr, "shared memory size too small");
    return nullptr;
  }

  size_t slot_area = total_size - HEADER_SIZE;
  uint32_t num_slots = (uint32_t)(slot_area / SLOT_SIZE);

  void* base = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    close(fd);
    napi_throw_error(env, nullptr, "mmap failed");
    return nullptr;
  }

  Cache* c = new Cache();
  c->fd = fd;
  c->base = base;
  c->total_size = total_size;
  c->header = (Header*)base;
  c->slots = (Slot*)((char*)base + HEADER_SIZE);
  c->num_slots = num_slots;

  if (c->header->magic == 0) {
    /* New segment: only the CAS winner in ensure_init writes header and slots (no race). */
    ensure_init(c);
  } else if (!layout_compatible(c->header)) {
    /* Incompatible layout: one process reinitializes via ensure_init. */
    if (c->header->num_slots > num_slots) {
      munmap(base, total_size);
      close(fd);
      delete c;
      napi_throw_error(env, nullptr, "shared memory size too small for reinit");
      return nullptr;
    }
    c->header->initialized.store(0, std::memory_order_release);
    ensure_init(c);
  } else {
    if (c->header->num_slots > num_slots) {
      munmap(base, total_size);
      close(fd);
      delete c;
      napi_throw_error(env, nullptr, "shared memory size mismatch");
      return nullptr;
    }
    wait_initialized(c);
    c->num_slots = c->header->num_slots;  /* after wait so we see consistent header */
  }

  g_cache = c;

  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

[[gnu::hot]] static napi_value Set(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  size_t argc = 3;
  napi_value args[3];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 2)
    return nullptr;

  size_t key_len, value_len;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &key_len) != napi_ok ||
      napi_get_value_string_utf8(env, args[1], nullptr, 0, &value_len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "set(key, value[, ttlMs]): key and value must be strings");
    return nullptr;
  }
  if (key_len >= KEY_MAX || value_len >= VALUE_MAX) {
    napi_value result;
    napi_get_boolean(env, false, &result);
    return result;
  }

  char key_buf[KEY_MAX], value_buf[VALUE_MAX];
  napi_get_value_string_utf8(env, args[0], key_buf, KEY_MAX, &key_len);
  napi_get_value_string_utf8(env, args[1], value_buf, VALUE_MAX, &value_len);
  key_buf[key_len] = '\0';
  value_buf[value_len] = '\0';

  uint64_t expire_at = 0;
  if (argc >= 3) {
    napi_valuetype t;
    if (napi_typeof(env, args[2], &t) != napi_ok || (t != napi_number && t != napi_undefined)) {
      napi_value result;
      napi_get_boolean(env, false, &result);
      return result;
    }
    if (t == napi_number) {
      int32_t ttl_ms = 0;
      if (napi_get_value_int32(env, args[2], &ttl_ms) != napi_ok || ttl_ms < 0) {
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
      }
      if (ttl_ms > 0)
        expire_at = now_ms() + (uint64_t)ttl_ms;
    }
  }

  Cache* __restrict__ c = g_cache;
  uint32_t num_slots = c->num_slots;
  uint32_t h = hash_key_fnv1a(key_buf, key_len);
  uint32_t step = hash_step(h, num_slots);
  const std::memory_order acq_rel = std::memory_order_acq_rel;
  const std::memory_order release = std::memory_order_release;
  const std::memory_order acquire = std::memory_order_acquire;

  for (int evict_retry = 0; evict_retry < 2; evict_retry++) {
    uint32_t idx = h % num_slots;
    for (uint32_t i = 0; i < num_slots; i++) {
      Slot* slot = &c->slots[idx];
      uint32_t next_idx = idx + step;
      if (next_idx >= num_slots)
        next_idx -= num_slots;
      __builtin_prefetch(&c->slots[next_idx], 0, 3);

      uint64_t sg = slot->state_gen.load(acquire);
      uint8_t s = state_from(sg);

      if (__builtin_expect(s == OCCUPIED, 1)) {
        if (key_matches(slot->key, key_buf, key_len)) {
          uint64_t gen = gen_from(sg);
          uint64_t reserved_sg = pack_sg(RESERVED, gen + 1);
          if (!slot->state_gen.compare_exchange_strong(sg, reserved_sg, acq_rel)) {
            sched_yield();
            idx = next_idx;
            continue;
          }
          memcpy(slot->value, value_buf, value_len + 1);
          slot->expire_at_ms.store(expire_at, std::memory_order_release);
          slot->slot_type = SLOT_TYPE_STRING;
          std::atomic_thread_fence(release);
          slot->state_gen.store(pack_sg(OCCUPIED, gen + 1), release);
          napi_value result;
          napi_get_boolean(env, true, &result);
          return result;
        }
        idx = next_idx;
        continue;
      }
      if (__builtin_expect(s == EMPTY || s == DELETED || s == RESERVED, 0)) {
        uint64_t gen = gen_from(sg);
        uint64_t reserved_sg = pack_sg(RESERVED, gen + 1);
        if (!slot->state_gen.compare_exchange_strong(sg, reserved_sg, acq_rel)) {
          sched_yield();
          idx = next_idx;
          continue;
        }
        memcpy(slot->key, key_buf, key_len + 1);
        memcpy(slot->value, value_buf, value_len + 1);
        slot->expire_at_ms.store(expire_at, std::memory_order_release);
        slot->slot_type = SLOT_TYPE_STRING;
        std::atomic_thread_fence(release);
        slot->state_gen.store(pack_sg(OCCUPIED, gen + 1), release);
        napi_value result;
        napi_get_boolean(env, true, &result);
        return result;
      }
      idx = next_idx;
    }

    /* Table full: try clock-style eviction of one victim slot */
    uint32_t victim_idx = (uint32_t)(c->header->eviction_counter.fetch_add(1, std::memory_order_relaxed) % num_slots);
    Slot* slot = &c->slots[victim_idx];
    uint64_t sg = slot->state_gen.load(acquire);
    if (state_from(sg) == OCCUPIED) {
      uint64_t gen = gen_from(sg);
      uint64_t reserved_sg = pack_sg(RESERVED, gen + 1);
      if (slot->state_gen.compare_exchange_strong(sg, reserved_sg, acq_rel)) {
        memcpy(slot->key, key_buf, key_len + 1);
        memcpy(slot->value, value_buf, value_len + 1);
        slot->expire_at_ms.store(expire_at, std::memory_order_release);
        slot->slot_type = SLOT_TYPE_STRING;
        std::atomic_thread_fence(release);
        slot->state_gen.store(pack_sg(OCCUPIED, gen + 1), release);
        napi_value result;
        napi_get_boolean(env, true, &result);
        return result;
      }
    }
  }

  napi_value result;
  napi_get_boolean(env, false, &result);
  return result;
}

[[gnu::hot]] static napi_value Get(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 1)
    return nullptr;

  size_t key_len;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &key_len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "get(key): key must be a string");
    return nullptr;
  }
  if (key_len >= KEY_MAX) {
    napi_value result;
    napi_get_null(env, &result);
    return result;
  }

  char key_buf[KEY_MAX];
  napi_get_value_string_utf8(env, args[0], key_buf, KEY_MAX, &key_len);
  key_buf[key_len] = '\0';

  uint64_t now = now_ms();
  Cache* __restrict__ c = g_cache;
  uint32_t num_slots = c->num_slots;
  uint32_t h = hash_key_fnv1a(key_buf, key_len);
  uint32_t step = hash_step(h, num_slots);
  uint32_t idx = h % num_slots;
  const std::memory_order acquire = std::memory_order_acquire;

  for (uint32_t i = 0; i < num_slots; i++) {
    Slot* slot = &c->slots[idx];
    uint32_t next_idx = idx + step;
    if (next_idx >= num_slots)
      next_idx -= num_slots;
    __builtin_prefetch(&c->slots[next_idx], 0, 3);

    uint64_t sg = slot->state_gen.load(acquire);
    uint8_t s = state_from(sg);

    if (__builtin_expect(s == EMPTY, 0))
      break;
    if (__builtin_expect(s == OCCUPIED, 1)) {
      if (key_matches(slot->key, key_buf, key_len)) {
        uint64_t exp = slot->expire_at_ms.load(acquire);
        if (exp != 0 && now >= exp)
          break;  /* expired => treat as not found */
        if (slot->slot_type == SLOT_TYPE_NUMERIC) {
          int64_t v = slot->numeric_value.load(acquire);
          char num_buf[24];
          int n = snprintf(num_buf, sizeof(num_buf), "%" PRId64, v);
          napi_value result;
          napi_create_string_utf8(env, num_buf, (size_t)n, &result);
          return result;
        }
        __builtin_prefetch(slot->value, 0, 3);
        char value_buf[VALUE_MAX];
        memcpy(value_buf, slot->value, VALUE_MAX);
        napi_value result;
        napi_create_string_utf8(env, value_buf, NAPI_AUTO_LENGTH, &result);
        return result;
      }
      idx = next_idx;
      continue;
    }
    idx = next_idx;
  }

  napi_value result;
  napi_get_null(env, &result);
  return result;
}

[[gnu::hot]] static napi_value Delete(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 1)
    return nullptr;

  size_t key_len;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &key_len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "delete(key): key must be a string");
    return nullptr;
  }
  if (key_len >= KEY_MAX) {
    napi_value result;
    napi_get_boolean(env, false, &result);
    return result;
  }

  char key_buf[KEY_MAX];
  napi_get_value_string_utf8(env, args[0], key_buf, KEY_MAX, &key_len);
  key_buf[key_len] = '\0';

  Cache* __restrict__ c = g_cache;
  uint32_t num_slots = c->num_slots;
  uint32_t h = hash_key_fnv1a(key_buf, key_len);
  uint32_t step = hash_step(h, num_slots);
  uint32_t idx = h % num_slots;
  const std::memory_order acquire = std::memory_order_acquire;
  const std::memory_order acq_rel = std::memory_order_acq_rel;

  for (uint32_t i = 0; i < num_slots; i++) {
    Slot* slot = &c->slots[idx];
    uint32_t next_idx = idx + step;
    if (next_idx >= num_slots)
      next_idx -= num_slots;
    __builtin_prefetch(&c->slots[next_idx], 0, 3);

    uint64_t sg = slot->state_gen.load(acquire);
    uint8_t s = state_from(sg);

    if (__builtin_expect(s == EMPTY, 0))
      break;
    if (__builtin_expect(s == OCCUPIED, 1)) {
      if (key_matches(slot->key, key_buf, key_len)) {
        uint64_t gen = gen_from(sg);
        uint64_t deleted_sg = pack_sg(DELETED, gen + 1);
        if (slot->state_gen.compare_exchange_strong(sg, deleted_sg, acq_rel)) {
          napi_value result;
          napi_get_boolean(env, true, &result);
          return result;
        }
        sched_yield();
      }
      idx = next_idx;
      continue;
    }
    idx = next_idx;
  }

  napi_value result;
  napi_get_boolean(env, false, &result);
  return result;
}

/* Lock-free add to numeric slot; create with value delta if missing. Returns new value. */
static napi_value IncrDecrImpl(napi_env env, const char* key_buf, size_t key_len, int64_t delta) {
  Cache* __restrict__ c = g_cache;
  uint32_t num_slots = c->num_slots;
  uint32_t h = hash_key_fnv1a(key_buf, key_len);
  uint32_t step = hash_step(h, num_slots);
  uint32_t idx = h % num_slots;
  const std::memory_order acq_rel = std::memory_order_acq_rel;
  const std::memory_order release = std::memory_order_release;
  const std::memory_order acquire = std::memory_order_acquire;

  for (int evict_retry = 0; evict_retry < 2; evict_retry++) {
    idx = h % num_slots;
    for (uint32_t i = 0; i < num_slots; i++) {
      Slot* slot = &c->slots[idx];
      uint32_t next_idx = idx + step;
      if (next_idx >= num_slots)
        next_idx -= num_slots;
      __builtin_prefetch(&c->slots[next_idx], 0, 3);

      uint64_t sg = slot->state_gen.load(acquire);
      uint8_t s = state_from(sg);

      if (s == OCCUPIED) {
        if (key_matches(slot->key, key_buf, key_len)) {
          if (slot->slot_type != SLOT_TYPE_NUMERIC) {
            napi_throw_type_error(env, nullptr, "incr/decr: key exists as string value");
            return nullptr;
          }
          int64_t new_val = slot->numeric_value.fetch_add(delta, acq_rel) + delta;
          napi_value result;
          napi_create_int64(env, new_val, &result);
          return result;
        }
        idx = next_idx;
        continue;
      }
      if (s == EMPTY || s == DELETED || s == RESERVED) {
        uint64_t gen = gen_from(sg);
        uint64_t reserved_sg = pack_sg(RESERVED, gen + 1);
        if (!slot->state_gen.compare_exchange_strong(sg, reserved_sg, acq_rel)) {
          sched_yield();
          idx = next_idx;
          continue;
        }
        memcpy(slot->key, key_buf, key_len + 1);
        slot->expire_at_ms.store(0, std::memory_order_release);
        slot->slot_type = SLOT_TYPE_NUMERIC;
        slot->numeric_value.store(delta, std::memory_order_release);
        std::atomic_thread_fence(release);
        slot->state_gen.store(pack_sg(OCCUPIED, gen + 1), release);
        napi_value result;
        napi_create_int64(env, delta, &result);
        return result;
      }
      idx = next_idx;
    }

    uint32_t victim_idx = (uint32_t)(c->header->eviction_counter.fetch_add(1, std::memory_order_relaxed) % num_slots);
    Slot* slot = &c->slots[victim_idx];
    uint64_t sg = slot->state_gen.load(acquire);
    if (state_from(sg) == OCCUPIED) {
      uint64_t gen = gen_from(sg);
      uint64_t reserved_sg = pack_sg(RESERVED, gen + 1);
      if (slot->state_gen.compare_exchange_strong(sg, reserved_sg, acq_rel)) {
        memcpy(slot->key, key_buf, key_len + 1);
        slot->expire_at_ms.store(0, std::memory_order_release);
        slot->slot_type = SLOT_TYPE_NUMERIC;
        slot->numeric_value.store(delta, std::memory_order_release);
        std::atomic_thread_fence(release);
        slot->state_gen.store(pack_sg(OCCUPIED, gen + 1), release);
        napi_value result;
        napi_create_int64(env, delta, &result);
        return result;
      }
    }
  }

  napi_throw_error(env, nullptr, "incr/decr: table full and eviction failed");
  return nullptr;
}

static napi_value Incr(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  size_t argc = 2;
  napi_value args[2];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 2)
    return nullptr;

  size_t key_len;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &key_len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "incr(key, delta): key must be a string");
    return nullptr;
  }
  if (key_len >= KEY_MAX) {
    napi_throw_type_error(env, nullptr, "incr(key, delta): key too long");
    return nullptr;
  }
  int64_t delta = 0;
  if (napi_get_value_int64(env, args[1], &delta) != napi_ok) {
    napi_throw_type_error(env, nullptr, "incr(key, delta): delta must be an integer");
    return nullptr;
  }

  char key_buf[KEY_MAX];
  napi_get_value_string_utf8(env, args[0], key_buf, KEY_MAX, &key_len);
  key_buf[key_len] = '\0';

  return IncrDecrImpl(env, key_buf, key_len, delta);
}

static napi_value Decr(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  size_t argc = 2;
  napi_value args[2];
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 2)
    return nullptr;

  size_t key_len;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &key_len) != napi_ok) {
    napi_throw_type_error(env, nullptr, "decr(key, delta): key must be a string");
    return nullptr;
  }
  if (key_len >= KEY_MAX) {
    napi_throw_type_error(env, nullptr, "decr(key, delta): key too long");
    return nullptr;
  }
  int64_t delta = 0;
  if (napi_get_value_int64(env, args[1], &delta) != napi_ok) {
    napi_throw_type_error(env, nullptr, "decr(key, delta): delta must be an integer");
    return nullptr;
  }
  int64_t neg_delta = (delta == INT64_MIN) ? INT64_MAX : -delta;

  char key_buf[KEY_MAX];
  napi_get_value_string_utf8(env, args[0], key_buf, KEY_MAX, &key_len);
  key_buf[key_len] = '\0';

  return IncrDecrImpl(env, key_buf, key_len, neg_delta);
}

static napi_value Clear(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  Cache* c = g_cache;
  for (uint32_t i = 0; i < c->num_slots; i++)
    c->slots[i].state_gen.store(0u, std::memory_order_release);

  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value Stats(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_throw_error(env, nullptr, "cache not initialized");
    return nullptr;
  }
  Cache* c = g_cache;
  uint32_t count = 0;
  for (uint32_t i = 0; i < c->num_slots; i++) {
    if (state_from(c->slots[i].state_gen.load(std::memory_order_acquire)) == OCCUPIED)
      count++;
  }

  napi_value obj, capacity_val, count_val, key_max_val, value_max_val;
  napi_create_object(env, &obj);
  napi_create_uint32(env, c->num_slots, &capacity_val);
  napi_create_uint32(env, count, &count_val);
  napi_create_uint32(env, (uint32_t)KEY_MAX, &key_max_val);
  napi_create_uint32(env, (uint32_t)VALUE_MAX, &value_max_val);

  napi_set_named_property(env, obj, "capacity", capacity_val);
  napi_set_named_property(env, obj, "count", count_val);
  napi_set_named_property(env, obj, "keyMaxBytes", key_max_val);
  napi_set_named_property(env, obj, "valueMaxBytes", value_max_val);

  return obj;
}

static napi_value Close(napi_env env, napi_callback_info info) {
  if (!g_cache) {
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
  }
  Cache* c = g_cache;
  munmap(c->base, c->total_size);
  close(c->fd);
  delete c;
  g_cache = nullptr;
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value InitModule(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
    { "init", nullptr, Init, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "set", nullptr, Set, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "get", nullptr, Get, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "delete", nullptr, Delete, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "clear", nullptr, Clear, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "stats", nullptr, Stats, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "close", nullptr, Close, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "incr", nullptr, Incr, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "decr", nullptr, Decr, nullptr, nullptr, nullptr, napi_default, nullptr },
  };
  napi_define_properties(env, exports, 9, desc);
  return exports;
}

} // namespace memcore

NAPI_MODULE(NODE_GYP_MODULE_NAME, memcore::InitModule)
