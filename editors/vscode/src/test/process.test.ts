// Plain-Node unit tests for process.ts - exercises real child processes
// (no VS Code API needed, so no framework/mocking is required).
//
// VS CODE WINDOWS M4 CI PORTABILITY FIX ROUND 2: every fixture below
// launches `process.execPath` (the actual running Node binary) with a
// small `-e <script>` snippet, instead of POSIX-only executables like
// `/bin/echo`/`/bin/sh`/`/bin/pwd` - those simply do not exist on
// Windows, so `spawnProcess` correctly reported `failedToStart: true`
// for them there (confirmed: this was a test-fixture bug, not a
// spawnProcess bug - see this round's report). `process.execPath` is
// guaranteed to exist and be executable on every platform Node itself
// runs on, so the SAME fixture/assertions now run unchanged under
// Fedora, GitHub Ubuntu, and GitHub Windows. The dedicated
// shell-metacharacter/argument-literalness/spaces-in-path tests still
// prove the real property they exist to prove (argv passed through
// verbatim, no shell involved) - just via a cross-platform child
// instead of a Unix command.

import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

import { spawnProcess } from '../process';

/** Runs `node -e <script> [...args]` via spawnProcess - the cross-platform child fixture every test below uses. */
function spawnNodeScript(script: string, args: string[] = [], cwd?: string) {
    return spawnProcess(process.execPath, ['-e', script, ...args], cwd);
}

async function testCapturesStdoutAndExitCodeZero(): Promise<void> {
    const result = await spawnNodeScript("process.stdout.write('hello kai\\n')");
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.exitCode, 0);
    assert.strictEqual(result.stdout, 'hello kai\n');
}

async function testCapturesNonZeroExitCode(): Promise<void> {
    const result = await spawnNodeScript('process.exit(7)');
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.exitCode, 7);
}

async function testFailedToStartForMissingExecutable(): Promise<void> {
    // A path that cannot exist on any platform - built with path.join()
    // rather than a POSIX-style "/definitely/..." literal, so this is
    // unambiguous on Windows too (never resolved relative to a "current
    // drive", never confused with a real path).
    const missingPath = path.join(os.tmpdir(), 'definitely-not-a-real-kai-compiler-binary');
    const result = await spawnProcess(missingPath, []);
    assert.strictEqual(result.failedToStart, true);
    assert.strictEqual(result.exitCode, null);
    assert.ok(result.startError && result.startError.length > 0);
}

async function testHonorsWorkingDirectory(): Promise<void> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kai-cwd-test-'));
    try {
        const result = await spawnNodeScript('process.stdout.write(process.cwd())', [], dir);
        assert.strictEqual(result.failedToStart, false);
        assert.strictEqual(result.exitCode, 0);
        // No .trim(): the fixture itself never writes a trailing
        // newline, so this is an exact comparison, not merely tolerant
        // of one.
        assert.strictEqual(result.stdout, dir);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// VS CODE WINDOWS M4 spec §10: a single argument containing a space must
// arrive at the child process AS ONE ARGUMENT, unsplit and unquoted -
// exactly what `shell: false` + an argv array guarantees, and exactly
// what a naive `exec("cmd " + arg)`-style string-concatenation approach
// would get wrong. The child reports back its OWN process.argv[1]
// verbatim; if the shell had (mis)split the argument, it would come back
// as only "hello".
async function testArgumentContainingASpaceIsPassedAsOneArgument(): Promise<void> {
    const result = await spawnNodeScript('process.stdout.write(process.argv[1])', ['hello kai world']);
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.stdout, 'hello kai world');
}

// VS CODE WINDOWS M4 spec §10/§11: simulates a Windows-style path with a
// space in a directory component (e.g. "C:\Users\Test User\...") by using
// a REAL directory whose name contains a space as the child's cwd - this
// runs on Linux too, but exercises the same "no shell string
// interpolation" property that makes spaces-in-paths safe on Windows:
// the path is handed to `spawn()` as a single argv/cwd value, never
// concatenated into a shell command line.
async function testWorkingDirectoryWithSpaceInNameIsHonored(): Promise<void> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kai test dir '));
    try {
        const result = await spawnNodeScript('process.stdout.write(process.cwd())', [], dir);
        assert.strictEqual(result.failedToStart, false);
        assert.strictEqual(result.exitCode, 0);
        assert.strictEqual(result.stdout, dir);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// An argument that itself contains shell metacharacters must be passed
// through literally, never interpreted - proof that no shell is involved
// (a real shell would expand `$HOME`, glob `*`, run `echo pwned`, etc.).
// The child reports back its own process.argv[1] verbatim; a real shell
// would have altered or split it.
async function testArgumentWithShellMetacharactersIsNotInterpreted(): Promise<void> {
    const metacharacterArg = '$HOME && echo pwned; * > /tmp/x';
    const result = await spawnNodeScript('process.stdout.write(process.argv[1])', [metacharacterArg]);
    assert.strictEqual(result.failedToStart, false);
    assert.strictEqual(result.stdout, metacharacterArg);
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
