#!/usr/bin/env node
/**
 * Quick start: init → set → get → delete → stats.
 *
 * Run from repository root: node dist/examples/quickstart.js
 */
import * as cache from '../index';

cache.init('quickstart', 4);
cache.clear();

cache.set('hello', 'world');
console.log('get("hello"):', cache.get('hello'));

cache.incr('counter', 1);
cache.incr('counter', 1);
console.log('after incr("counter", 1) x2:', cache.get('counter'));

cache.del('hello');
console.log('after del("hello"):', cache.get('hello'));

console.log('stats:', cache.stats());
cache.clear();

console.log('Done.');
