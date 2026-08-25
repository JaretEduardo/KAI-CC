// Plain-Node unit tests for paths.ts (no VS Code API, no test framework -
// mirrors the compiler's own tests/support/check.hpp convention: assert,
// non-zero exit on failure). Run via `node out/test/paths.test.js`.

import * as assert from 'assert';

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
    const result = resolveCompilerPath('/ext', '   ', 'linux', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: '/ext/bin/kaicc' });
}

function testBundledPathUsedOnLinuxX64WhenPresent(): void {
    const result = resolveCompilerPath('/ext', undefined, 'linux', 'x64', alwaysExists);
    assert.deepStrictEqual(result, { ok: true, kaiccPath: '/ext/bin/kaicc' });
}

function testBundledPathMissingFailsCleanly(): void {
    const result = resolveCompilerPath('/ext', undefined, 'linux', 'x64', neverExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /Bundled KAI compiler not found/);
        assert.match(result.message, /stage-compiler/);
    }
}

function testUnsupportedPlatformFailsCleanlyEvenIfFileWouldExist(): void {
    const result = resolveCompilerPath('/ext', undefined, 'win32', 'x64', alwaysExists);
    assert.strictEqual(result.ok, false);
    if (!result.ok) {
        assert.match(result.message, /Linux x64/);
    }
}

function testUnsupportedArchFailsCleanly(): void {
    const result = resolveCompilerPath('/ext', undefined, 'linux', 'arm64', alwaysExists);
    assert.strictEqual(result.ok, false);
}

function testComputeOutputPathStripsKaiExtensionInSameDirectory(): void {
    assert.strictEqual(computeOutputPath('/project/src/hello.kai'), '/project/src/hello');
}

function testComputeOutputPathHandlesNestedDirectoriesAndDots(): void {
    assert.strictEqual(computeOutputPath('/a/b.c/d/my.program.kai'), '/a/b.c/d/my.program');
}

function main(): void {
    testExplicitConfiguredPathUsedWhenItExists();
    testExplicitConfiguredPathFailsCleanlyWhenMissing();
    testWhitespaceOnlyConfiguredPathIsTreatedAsUnset();
    testBundledPathUsedOnLinuxX64WhenPresent();
    testBundledPathMissingFailsCleanly();
    testUnsupportedPlatformFailsCleanlyEvenIfFileWouldExist();
    testUnsupportedArchFailsCleanly();
    testComputeOutputPathStripsKaiExtensionInSameDirectory();
    testComputeOutputPathHandlesNestedDirectoriesAndDots();

    console.log('paths.test.ts: all tests passed');
}

main();
