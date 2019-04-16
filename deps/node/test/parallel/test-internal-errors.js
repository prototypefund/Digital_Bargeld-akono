// Flags: --expose-internals
'use strict';
const common = require('../common');
const {
  hijackStdout,
  restoreStdout,
} = require('../common/hijackstdio');

const assert = require('assert');
const errors = require('internal/errors');

// Turn off ANSI color formatting for this test file.
const { inspect } = require('util');
inspect.defaultOptions.colors = false;

errors.E('TEST_ERROR_1', 'Error for testing purposes: %s',
         Error, TypeError, RangeError);
errors.E('TEST_ERROR_2', (a, b) => `${a} ${b}`, Error);

{
  const err = new errors.codes.TEST_ERROR_1('test');
  assert(err instanceof Error);
  assert.strictEqual(err.name, 'Error [TEST_ERROR_1]');
  assert.strictEqual(err.message, 'Error for testing purposes: test');
  assert.strictEqual(err.code, 'TEST_ERROR_1');
}

{
  const err = new errors.codes.TEST_ERROR_1.TypeError('test');
  assert(err instanceof TypeError);
  assert.strictEqual(err.name, 'TypeError [TEST_ERROR_1]');
  assert.strictEqual(err.message, 'Error for testing purposes: test');
  assert.strictEqual(err.code, 'TEST_ERROR_1');
}

{
  const err = new errors.codes.TEST_ERROR_1.RangeError('test');
  assert(err instanceof RangeError);
  assert.strictEqual(err.name, 'RangeError [TEST_ERROR_1]');
  assert.strictEqual(err.message, 'Error for testing purposes: test');
  assert.strictEqual(err.code, 'TEST_ERROR_1');
}

{
  const err = new errors.codes.TEST_ERROR_2('abc', 'xyz');
  assert(err instanceof Error);
  assert.strictEqual(err.name, 'Error [TEST_ERROR_2]');
  assert.strictEqual(err.message, 'abc xyz');
  assert.strictEqual(err.code, 'TEST_ERROR_2');
}

{
  assert.throws(
    () => new errors.codes.TEST_ERROR_1(),
    {
      message: 'Code: TEST_ERROR_1; The provided arguments ' +
               'length (0) does not match the required ones (1).'
    }
  );
}

// Tests for common.expectsError
common.expectsError(() => {
  throw new errors.codes.TEST_ERROR_1.TypeError('a');
}, { code: 'TEST_ERROR_1' });
common.expectsError(() => {
  throw new errors.codes.TEST_ERROR_1.TypeError('a');
}, { code: 'TEST_ERROR_1',
     type: TypeError,
     message: /^Error for testing/ });
common.expectsError(() => {
  throw new errors.codes.TEST_ERROR_1.TypeError('a');
}, { code: 'TEST_ERROR_1', type: TypeError });
common.expectsError(() => {
  throw new errors.codes.TEST_ERROR_1.TypeError('a');
}, {
  code: 'TEST_ERROR_1',
  type: TypeError,
  message: 'Error for testing purposes: a'
});

common.expectsError(() => {
  common.expectsError(() => {
    throw new errors.codes.TEST_ERROR_1.TypeError('a');
  }, { code: 'TEST_ERROR_1', type: RangeError });
}, {
  code: 'ERR_ASSERTION',
  message: /\+   type: \[Function: TypeError]\n-   type: \[Function: RangeError]/
});

common.expectsError(() => {
  common.expectsError(() => {
    throw new errors.codes.TEST_ERROR_1.TypeError('a');
  }, { code: 'TEST_ERROR_1',
       type: TypeError,
       message: /^Error for testing 2/ });
}, {
  code: 'ERR_ASSERTION',
  type: assert.AssertionError,
  message: /\+   message: 'Error for testing purposes: a',\n-   message: \/\^Error/
});

// Test that `code` property is mutable and that changing it does not change the
// name.
{
  const myError = new errors.codes.TEST_ERROR_1('foo');
  assert.strictEqual(myError.code, 'TEST_ERROR_1');
  assert.strictEqual(myError.hasOwnProperty('code'), false);
  assert.strictEqual(myError.hasOwnProperty('name'), false);
  assert.deepStrictEqual(Object.keys(myError), []);
  const initialName = myError.name;
  myError.code = 'FHQWHGADS';
  assert.strictEqual(myError.code, 'FHQWHGADS');
  assert.strictEqual(myError.name, initialName);
  assert.deepStrictEqual(Object.keys(myError), ['code']);
  assert.ok(myError.name.includes('TEST_ERROR_1'));
  assert.ok(!myError.name.includes('FHQWHGADS'));
}

// Test that `name` is mutable and that changing it alters `toString()` but not
// `console.log()` results, which is the behavior of `Error` objects in the
// browser. Note that `name` becomes enumerable after being assigned.
{
  const myError = new errors.codes.TEST_ERROR_1('foo');
  assert.deepStrictEqual(Object.keys(myError), []);
  const initialToString = myError.toString();

  myError.name = 'Fhqwhgads';
  assert.deepStrictEqual(Object.keys(myError), ['name']);
  assert.notStrictEqual(myError.toString(), initialToString);
}

// Test that `message` is mutable and that changing it alters `toString()` but
// not `console.log()` results, which is the behavior of `Error` objects in the
// browser. Note that `message` remains non-enumerable after being assigned.
{
  let initialConsoleLog = '';
  hijackStdout((data) => { initialConsoleLog += data; });
  const myError = new errors.codes.TEST_ERROR_1('foo');
  assert.deepStrictEqual(Object.keys(myError), []);
  const initialToString = myError.toString();
  console.log(myError);
  assert.notStrictEqual(initialConsoleLog, '');

  restoreStdout();

  let subsequentConsoleLog = '';
  hijackStdout((data) => { subsequentConsoleLog += data; });
  myError.message = 'Fhqwhgads';
  assert.deepStrictEqual(Object.keys(myError), []);
  assert.notStrictEqual(myError.toString(), initialToString);
  console.log(myError);
  assert.strictEqual(subsequentConsoleLog, initialConsoleLog);

  restoreStdout();
}