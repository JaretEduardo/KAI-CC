// Plain-Node unit tests for paths.ts (no VS Code API, no test framework -
// mirrors the compiler's own tests/support/check.hpp convention: assert,
// non-zero exit on failure). Run via `node out/test/paths.test.js`.

import * as assert from 'assert';
import * as path from 'path';

import { computeOutputPath, resolveCompilerPath } from '../paths';

function alwaysExists(_candidate: string): boolean {
    return true;
}

function neverExists(_candidate: string): boolean {
    return false;
}

function testExplicitConfiguredPathUsedWhenItExists(): void {
    const result = resolveCompilerPath('/ext', '/custom/kaicc', 'linux', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: '/custom/kaicc' });
}

function testExplicitConfiguredPathFailsCleanlyWhenMissing(): void {
    const result = resolveCompilerPath('/ext', '/custom/kaicc', 'linux', 'x64', neverExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /does not exist/);
        assert.match(result.message, /\/custom\/kaicc/);
    }
}

function testWhitespaceOnlyConfiguredPathIsTreatedAsUnset(): void {
    // A configured-but-blank setting must not be treated as "explicitly
    // configured" - it should fall through to bundled-compiler lookup.
    //
    // VS CODE WINDOWS M4 FIRST REAL CI FAILURE FIX: the expected value
    // MUST be built with path.join(), not a hand-written "/ext/bin/kaicc"
    // POSIX literal. resolveCompilerPath() joins extensionPath with the
    // bundled binary's relative path using the REAL, platform-native
    // `path` module (see paths.ts) - on a real Windows host that module
    // is path.win32, which normalizes ANY input (even one that "looks
    // POSIX", like the '/ext' fixture below) to backslash-separated
    // output. A literal forward-slash expected string is therefore only
    // ever correct on a POSIX test host; using path.join() on both sides
    // keeps the assertion correct on whichever host actually runs it -
    // this was a test-authoring bug, not a resolveCompilerPath() bug (see
    // this milestone's report for the full analysis).
    const result = resolveCompilerPath('/ext', '   ', 'linux', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: path.join('/ext', 'bin', 'kaicc') });
}

function testBundledPathUsedOnLinuxX64WhenPresent(): void {
    const result = resolveCompilerPath('/ext', undefined, 'linux', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: path.join('/ext', 'bin', 'kaicc') });
}

function testBundledPathMissingFailsCleanly(): void {
    const result = resolveCompilerPath('/ext', undefined, 'linux', 'x64', neverExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /Bundled KAI compiler not found/);
        assert.match(result.message, /stage-compiler/);
    }
}

// VS CODE WINDOWS M4 spec §3/§7: win32/x64 must resolve to the bundled
// kaicc.exe (a distinct binary NAME, never derived by appending ".exe" to
// the Linux path at call time).
function testBundledPathUsedOnWin32X64WhenPresent(): void {
    const result = resolveCompilerPath('/ext', undefined, 'win32', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: path.join('/ext', 'bin', 'kaicc.exe') });
}

function testBundledPathMissingOnWin32X64FailsCleanly(): void {
    const result = resolveCompilerPath('/ext', undefined, 'win32', 'x64', neverExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /Bundled KAI compiler not found/);
        assert.match(result.message, /kaicc\.exe/);
    }
}

// Explicit configured path still wins on Windows, exactly like Linux -
// the priority order does not change per platform.
function testExplicitConfiguredPathWinsOnWindowsToo(): void {
    const result = resolveCompilerPath('/ext', 'C:\\Users\\dev\\kaicc.exe', 'win32', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: 'C:\\Users\\dev\\kaicc.exe' });
}

function testUnsupportedPlatformFailsCleanlyEvenIfFileWouldExist(): void {
    const result = resolveCompilerPath('/ext', undefined, 'darwin', 'x64', alwaysExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /Linux x64 or Windows x64/);
    }
}

function testUnsupportedArchFailsCleanly(): void {
    const result = resolveCompilerPath('/ext', undefined, 'linux', 'arm64', alwaysExists);
    assert.strictEqual(result.ok, false);
}

// VS CODE WINDOWS M4 spec §3: win32/arm64 is explicitly unsupported too -
// only win32/x64 has a bundled target.
function testWin32Arm64FailsCleanly(): void {
    const result = resolveCompilerPath('/ext', undefined, 'win32', 'arm64', alwaysExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /Linux x64 or Windows x64/);
    }
}

// VS CODE WINDOWS M4 FIRST REAL CI FAILURE FIX: these two tests call
// computeOutputPath() WITHOUT an explicit `pathImpl` override, so they
// exercise the REAL default (the platform-native `path` module) - on a
// real Windows test host that default is path.win32, which normalizes
// path.join()'s output to backslashes even when fed a forward-slash
// input (path.dirname()/path.basename() do NOT normalize separators,
// but path.join() - used internally by computeOutputPath() - always
// does). A hand-written forward-slash literal fixture/expectation would
// therefore only ever be self-consistent on a POSIX host. Building BOTH
// the source-path fixture AND the expected value via this file's own
// imported (also platform-native) `path.join()` keeps these assertions
// correct on whichever host actually runs them, while still exercising
// the real ".kai"-stripping/nested-directory/dot-in-directory-name logic
// the tests are actually about.
function testComputeOutputPathStripsKaiExtensionInSameDirectory(): void {
    const sourcePath = path.join('project', 'src', 'hello.kai');
    const expected = path.join('project', 'src', 'hello');
    assert.strictEqual(computeOutputPath(sourcePath, 'linux'), expected);
}

function testComputeOutputPathHandlesNestedDirectoriesAndDots(): void {
    const sourcePath = path.join('a', 'b.c', 'd', 'my.program.kai');
    const expected = path.join('a', 'b.c', 'd', 'my.program');
    assert.strictEqual(computeOutputPath(sourcePath, 'linux'), expected);
}

// VS CODE WINDOWS M4 spec §8: on win32, the computed output path must
// carry the SAME ".exe" suffix kaicc.exe itself will actually produce
// (kai::cli::resolveNativeExecutablePath()'s C++ counterpart) - otherwise
// the extension's own post-build existsSync() check and the subsequent
// "Run Current File" launch target would look for the wrong filename.
function testComputeOutputPathAppendsExeOnWindows(): void {
    assert.strictEqual(computeOutputPath('/project/src/hello.kai', 'win32', path.posix), '/project/src/hello.exe');
}

function testComputeOutputPathNeverDoubleSuffixesExe(): void {
    assert.strictEqual(computeOutputPath('/project/src/hello.exe.kai', 'win32', path.posix), '/project/src/hello.exe');
}

function testComputeOutputPathDoesNotAppendExeOnLinuxEvenForExeNamedSource(): void {
    // Same host-native-fixture reasoning as the two tests above - no
    // explicit pathImpl override here either.
    const sourcePath = path.join('project', 'src', 'hello.exe.kai');
    const expected = path.join('project', 'src', 'hello.exe');
    assert.strictEqual(computeOutputPath(sourcePath, 'linux'), expected);
}

// VS CODE WINDOWS M4 spec §10/§11: exercise REAL Windows-style path
// splitting (backslashes, a drive letter, and a directory name containing
// a space) via `path.win32` explicitly - this test runs on Linux CI/dev
// machines, where the platform-native `path` module is POSIX and would
// mis-split a backslash-separated string, so the Windows-specific
// behavior can only be exercised by passing `path.win32` in explicitly
// (production code never hardcodes it - see paths.ts's own doc comment).
function testComputeOutputPathOnWindowsStylePathWithSpaces(): void {
    const result = computeOutputPath('C:\\Users\\Test User\\Project\\hello.kai', 'win32', path.win32);
    assert.strictEqual(result, 'C:\\Users\\Test User\\Project\\hello.exe');
}

function testComputeOutputPathOnWindowsStylePathUnderProgramFiles(): void {
    const result = computeOutputPath('C:\\Program Files\\KAI Projects\\demo.kai', 'win32', path.win32);
    assert.strictEqual(result, 'C:\\Program Files\\KAI Projects\\demo.exe');
}

function main(): void {
    testExplicitConfiguredPathUsedWhenItExists();
    testExplicitConfiguredPathFailsCleanlyWhenMissing();
    testWhitespaceOnlyConfiguredPathIsTreatedAsUnset();
    testBundledPathUsedOnLinuxX64WhenPresent();
    testBundledPathMissingFailsCleanly();
    testBundledPathUsedOnWin32X64WhenPresent();
    testBundledPathMissingOnWin32X64FailsCleanly();
    testExplicitConfiguredPathWinsOnWindowsToo();
    testUnsupportedPlatformFailsCleanlyEvenIfFileWouldExist();
    testUnsupportedArchFailsCleanly();
    testWin32Arm64FailsCleanly();
    testComputeOutputPathStripsKaiExtensionInSameDirectory();
    testComputeOutputPathHandlesNestedDirectoriesAndDots();
    testComputeOutputPathAppendsExeOnWindows();
    testComputeOutputPathNeverDoubleSuffixesExe();
    testComputeOutputPathDoesNotAppendExeOnLinuxEvenForExeNamedSource();
    testComputeOutputPathOnWindowsStylePathWithSpaces();
    testComputeOutputPathOnWindowsStylePathUnderProgramFiles();

    console.log('paths.test.ts: all tests passed');
}

main();
