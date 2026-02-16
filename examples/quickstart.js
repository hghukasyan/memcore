#!/usr/bin/env node
/**
 * Quick start: init → set → get → delete → stats.
 *
 * Run from repository root: node examples/quickstart.js
 */
const path = require('path');
const cache = require(path.join(__dirname, '..', 'index.js'));

cache.init('quickstart', 4);
cache.clear();

cache.set('hello', 'world');
console.log('get("hello"):', cache.get('hello'));

cache.incr('counter', 1);  // creates numeric slot if missing
cache.incr('counter', 1);
console.log('after incr("counter", 1) x2:', cache.get('counter'));

cache.del('hello');
console.log('after del("hello"):', cache.get('hello'));

console.log('stats:', cache.stats());
cache.clear();

console.log('Done.');
