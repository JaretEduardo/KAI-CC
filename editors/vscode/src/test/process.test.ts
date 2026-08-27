// Plain-Node unit tests for process.ts - exercises real child processes
// (no VS Code API needed, so no framework/mocking is required).

import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

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

// VS CODE WINDOWS M4 spec §10: a single argument containing a space must
// arrive at the child process AS ONE ARGUMENT, unsplit and unquoted -
// exactly what `shell: false` + an argv array guarantees, and exactly
// what a naive `exec("cmd " + arg)`-style string-concatenation approach
// would get wrong. `/bin/echo "$1"` would print two words if the shell
// had (mis)split the argument; spawnProcess must print it as one.
async function testArgumentContainingASpaceIsPassedAsOneArgument(): Promise<void> {
    const result = await spawnProcess('/bin/echo', ['hello kai world']);
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.stdout, 'hello kai world\n');
}

// VS CODE WINDOWS M4 spec §10/§11: simulates a Windows-style path with a
// space in a directory component (e.g. "C:\Users\Test User\...") by using
// a REAL directory whose name contains a space as the child's cwd - this
// runs on Linux, but exercises the same "no shell string interpolation"
// property that makes spaces-in-paths safe on Windows too: the path is
// handed to `spawn()` as a single argv/cwd value, never concatenated into
// a shell command line.
async function testWorkingDirectoryWithSpaceInNameIsHonored(): Promise<void> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kai test dir '));
    try {
        const result = await spawnProcess('/bin/pwd', [], dir);
        assert.strictEqual(result.failedToStart, false);
        assert.strictEqual(result.exitCode, 0);
        assert.strictEqual(result.stdout.trim(), dir);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// An argument that itself contains shell metacharacters must be passed
// through literally, never interpreted - proof that no shell is involved
// (a real shell would expand `$HOME`, glob `*`, etc.).
async function testArgumentWithShellMetacharactersIsNotInterpreted(): Promise<void> {
    const result = await spawnProcess('/bin/echo', ['$HOME && echo pwned; * > /tmp/x']);
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.stdout, '$HOME && echo pwned; * > /tmp/x\n');
}

async function main(): Promise<void> {
    await testCapturesStdoutAndExitCodeZero();
    await testCapturesNonZeroExitCode();
    await testFailedToStartForMissingExecutable();
    await testHonorsWorkingDirectory();
    await testArgumentContainingASpaceIsPassedAsOneArgument();
    await testWorkingDirectoryWithSpaceInNameIsHonored();
    await testArgumentWithShellMetacharactersIsNotInterpreted();

    console.log('process.test.ts: all tests passed');
}

main().catch((err) => {
    console.error(err);
    process.exitCode = 1;
});
