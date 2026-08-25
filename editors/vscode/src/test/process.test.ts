// Plain-Node unit tests for process.ts - exercises real child processes
// (no VS Code API needed, so no framework/mocking is required).

import * as assert from 'assert';

import { spawnProcess } from '../process';

async function testCapturesStdoutAndExitCodeZero(): Promise<void> {
    const result = await spawnProcess('/bin/echo', ['hello', 'kai']);
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.exitCode, 0);
    assert.strictEqual(result.stdout, 'hello kai\n');
}

async function testCapturesNonZeroExitCode(): Promise<void> {
    const result = await spawnProcess('/bin/sh', ['-c', 'exit 7'], undefined);
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.exitCode, 7);
}

async function testFailedToStartForMissingExecutable(): Promise<void> {
    const result = await spawnProcess('/definitely/not/a/real/kai-compiler-binary', []);
    assert.strictEqual(result.failedToStart, true);
    assert.strictEqual(result.exitCode, null);
    assert.ok(result.startError && result.startError.length > 0);
}

async function testHonorsWorkingDirectory(): Promise<void> {
    const result = await spawnProcess('/bin/pwd', [], '/tmp');
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.exitCode, 0);
    assert.strictEqual(result.stdout.trim(), '/tmp');
}

async function main(): Promise<void> {
    await testCapturesStdoutAndExitCodeZero();
    await testCapturesNonZeroExitCode();
    await testFailedToStartForMissingExecutable();
    await testHonorsWorkingDirectory();

    console.log('process.test.ts: all tests passed');
}

main().catch((err) => {
    console.error(err);
    process.exitCode = 1;
});
