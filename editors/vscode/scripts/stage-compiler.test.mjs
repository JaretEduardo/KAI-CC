// Hermetic test for stage-compiler.mjs: exercises the real copy logic
// against a temporary directory tree, never the actual repository build
// output, so it stays deterministic regardless of whether `build/` has
// been built yet.

import assert from 'assert';
import fs from 'fs';
import os from 'os';
import path from 'path';

import { stageCompiler } from './stage-compiler.mjs';

function makeTempDir(prefix) {
    return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

// v0.1.0-alpha.1: stageCompiler() now also requires LICENSE/
// THIRD_PARTY_NOTICES.md/third_party/licenses/* to exist under whichever
// root it stages from (releaseRoot, or repoRoot when releaseRoot is
// omitted) - every pre-existing test's fake root needs these too, or the
// new legal-file check would fail them for an unrelated reason.
function setupLegalFiles(root, { licenseContent = 'fake-license', noticesContent = 'fake-notices' } = {}) {
    fs.writeFileSync(path.join(root, 'LICENSE'), licenseContent);
    fs.writeFileSync(path.join(root, 'THIRD_PARTY_NOTICES.md'), noticesContent);
    const licenseDir = path.join(root, 'third_party', 'licenses');
    fs.mkdirSync(licenseDir, { recursive: true });
    fs.writeFileSync(path.join(licenseDir, 'LLVM-LICENSE.txt'), 'fake-llvm-license');
    fs.writeFileSync(path.join(licenseDir, 'Z3-LICENSE.txt'), 'fake-z3-license');
}

// VS CODE WINDOWS M4: builds a fake win32-x64 release root
// (bin/kaicc.exe + the 5 required MSYS2 DLLs, lib/kai/libkai_runtime.a,
// legal files) - the Windows analog of the Linux fake-release-root setup
// already used by the pre-existing tests below.
const WIN32_REQUIRED_DLLS = ['libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'zlib1.dll', 'libzstd.dll'];

function setupWin32ReleaseRoot(releaseRoot, { includeAllDlls = true } = {}) {
    const bin = path.join(releaseRoot, 'bin');
    const lib = path.join(releaseRoot, 'lib', 'kai');
    fs.mkdirSync(bin, { recursive: true });
    fs.mkdirSync(lib, { recursive: true });
    fs.writeFileSync(path.join(bin, 'kaicc.exe'), 'fake-windows-kaicc-exe');
    fs.writeFileSync(path.join(lib, 'libkai_runtime.a'), 'fake-archive-contents');
    const dlls = includeAllDlls ? WIN32_REQUIRED_DLLS : WIN32_REQUIRED_DLLS.slice(0, 2);
    for (const dll of dlls) {
        fs.writeFileSync(path.join(bin, dll), `fake-${dll}`);
    }
    setupLegalFiles(releaseRoot);
    return { bin, lib };
}

function setupLinuxReleaseRoot(releaseRoot) {
    const bin = path.join(releaseRoot, 'bin');
    const lib = path.join(releaseRoot, 'lib', 'kai');
    fs.mkdirSync(bin, { recursive: true });
    fs.mkdirSync(lib, { recursive: true });
    fs.writeFileSync(path.join(bin, 'kaicc'), 'fake-linux-kaicc-elf');
    fs.chmodSync(path.join(bin, 'kaicc'), 0o755);
    fs.writeFileSync(path.join(lib, 'libkai_runtime.a'), 'fake-archive-contents');
    setupLegalFiles(releaseRoot);
    return { bin, lib };
}

function testStagesBothFilesToExpectedPaths() {
    const repoRoot = makeTempDir('kai-stage-test-repo-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-');

    try {
        const buildBin = path.join(repoRoot, 'build', 'bin');
        const buildLib = path.join(repoRoot, 'build', 'lib', 'kai');
        fs.mkdirSync(buildBin, { recursive: true });
        fs.mkdirSync(buildLib, { recursive: true });

        const fakeKaiccContent = '#!/bin/sh\necho fake-kaicc\n';
        fs.writeFileSync(path.join(buildBin, 'kaicc'), fakeKaiccContent);
        fs.chmodSync(path.join(buildBin, 'kaicc'), 0o755);
        fs.writeFileSync(path.join(buildLib, 'libkai_runtime.a'), 'fake-archive-contents');
        setupLegalFiles(repoRoot);

        const result = stageCompiler({ repoRoot, extensionRoot, target: 'linux-x64' });

        const expectedKaicc = path.join(extensionRoot, 'bin', 'kaicc');
        const expectedRuntime = path.join(extensionRoot, 'lib', 'kai', 'libkai_runtime.a');

        assert.strictEqual(result.kaiccPath, expectedKaicc);
        assert.strictEqual(result.runtimePath, expectedRuntime);
        assert.strictEqual(result.target, 'linux-x64');

        assert.ok(fs.existsSync(expectedKaicc), 'staged kaicc should exist');
        assert.ok(fs.existsSync(expectedRuntime), 'staged libkai_runtime.a should exist');

        assert.strictEqual(fs.readFileSync(expectedKaicc, 'utf8'), fakeKaiccContent);
        assert.strictEqual(fs.readFileSync(expectedRuntime, 'utf8'), 'fake-archive-contents');

        const mode = fs.statSync(expectedKaicc).mode;
        assert.ok((mode & 0o111) !== 0, 'staged kaicc must remain executable');

        // bin/ and lib/kai/ must be siblings under extensionRoot - this is
        // the exact shape NativeLinker's relative lookup depends on.
        const binDir = path.dirname(expectedKaicc);
        const libDir = path.dirname(path.dirname(expectedRuntime));
        assert.strictEqual(path.dirname(binDir), path.dirname(libDir));
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

function testFailsClearlyWhenBuildArtifactsAreMissing() {
    const repoRoot = makeTempDir('kai-stage-test-repo-empty-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-empty-');

    try {
        assert.throws(() => stageCompiler({ repoRoot, extensionRoot, target: 'linux-x64' }), /kaicc not found/);
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// RELEASE HARDENING M1: when `releaseRoot` is passed, staging must read
// from `<releaseRoot>/bin/kaicc` + `<releaseRoot>/lib/kai/libkai_runtime.a`
// INSTEAD of `<repoRoot>/build/...` - this is how a developer packaging
// the extension picks up the portable Ubuntu-22.04-built artifact rather
// than their own host's local build.
function testStagesFromReleaseRootWhenProvided() {
    const repoRoot = makeTempDir('kai-stage-test-repo-release-');
    const releaseRoot = makeTempDir('kai-stage-test-release-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-release-');

    try {
        // A `build/` tree ALSO exists here, with different content - proves
        // releaseRoot is genuinely preferred, not merely "also works".
        const buildBin = path.join(repoRoot, 'build', 'bin');
        const buildLib = path.join(repoRoot, 'build', 'lib', 'kai');
        fs.mkdirSync(buildBin, { recursive: true });
        fs.mkdirSync(buildLib, { recursive: true });
        fs.writeFileSync(path.join(buildBin, 'kaicc'), 'this-is-the-LOCAL-build-and-must-not-be-staged');
        fs.writeFileSync(path.join(buildLib, 'libkai_runtime.a'), 'local-build-archive');
        setupLegalFiles(repoRoot, { licenseContent: 'this-is-the-REPO-license-and-must-not-be-staged' });

        const releaseBin = path.join(releaseRoot, 'bin');
        const releaseLib = path.join(releaseRoot, 'lib', 'kai');
        fs.mkdirSync(releaseBin, { recursive: true });
        fs.mkdirSync(releaseLib, { recursive: true });
        const portableKaiccContent = 'this-is-the-PORTABLE-release-kaicc';
        fs.writeFileSync(path.join(releaseBin, 'kaicc'), portableKaiccContent);
        fs.chmodSync(path.join(releaseBin, 'kaicc'), 0o755);
        fs.writeFileSync(path.join(releaseLib, 'libkai_runtime.a'), 'portable-release-archive');
        setupLegalFiles(releaseRoot, { licenseContent: 'this-is-the-PORTABLE-release-LICENSE' });

        const result = stageCompiler({ repoRoot, extensionRoot, releaseRoot });

        assert.strictEqual(result.source, 'release');
        // No explicit target given - must be DETECTED from releaseRoot's
        // own contents (bin/kaicc, not bin/kaicc.exe), never from the host.
        assert.strictEqual(result.target, 'linux-x64');
        assert.strictEqual(fs.readFileSync(result.kaiccPath, 'utf8'), portableKaiccContent);
        assert.strictEqual(fs.readFileSync(result.runtimePath, 'utf8'), 'portable-release-archive');

        // v0.1.0-alpha.1: legal material must come from releaseRoot too -
        // never the repo's own copy - proving genuine preference the same
        // way the kaicc/runtime content checks above do.
        assert.strictEqual(
            fs.readFileSync(path.join(extensionRoot, 'LICENSE'), 'utf8'),
            'this-is-the-PORTABLE-release-LICENSE',
        );
        assert.ok(fs.existsSync(path.join(extensionRoot, 'THIRD_PARTY_NOTICES.md')));
        assert.ok(fs.existsSync(path.join(extensionRoot, 'third_party', 'licenses', 'LLVM-LICENSE.txt')));
        assert.ok(fs.existsSync(path.join(extensionRoot, 'third_party', 'licenses', 'Z3-LICENSE.txt')));
        assert.ok(
            result.legalFiles.includes('LICENSE') &&
                result.legalFiles.includes('THIRD_PARTY_NOTICES.md') &&
                result.legalFiles.some((f) => f.endsWith('LLVM-LICENSE.txt')),
            'legalFiles should report every staged legal file',
        );
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// Without releaseRoot, staging must report its source as 'build' - the
// unchanged, pre-existing behavior.
function testDefaultSourceIsBuildWhenReleaseRootOmitted() {
    const repoRoot = makeTempDir('kai-stage-test-repo-default-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-default-');

    try {
        const buildBin = path.join(repoRoot, 'build', 'bin');
        const buildLib = path.join(repoRoot, 'build', 'lib', 'kai');
        fs.mkdirSync(buildBin, { recursive: true });
        fs.mkdirSync(buildLib, { recursive: true });
        fs.writeFileSync(path.join(buildBin, 'kaicc'), 'local-build-kaicc');
        fs.chmodSync(path.join(buildBin, 'kaicc'), 0o755);
        fs.writeFileSync(path.join(buildLib, 'libkai_runtime.a'), 'local-build-archive');
        setupLegalFiles(repoRoot);

        const result = stageCompiler({ repoRoot, extensionRoot, target: 'linux-x64' });

        assert.strictEqual(result.source, 'build');

        // v0.1.0-alpha.1: with no releaseRoot, legal material comes from
        // the repository root's own tracked files instead - ordinary
        // local extension development must not require a release build
        // just to get a valid LICENSE/notices staged.
        assert.ok(fs.existsSync(path.join(extensionRoot, 'LICENSE')));
        assert.ok(fs.existsSync(path.join(extensionRoot, 'THIRD_PARTY_NOTICES.md')));
        assert.ok(fs.existsSync(path.join(extensionRoot, 'third_party', 'licenses', 'LLVM-LICENSE.txt')));
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// A releaseRoot that does not point at a valid layout must fail with a
// clear, releaseRoot-specific message - never silently fall back to
// build/, and never a generic/confusing error.
function testFailsClearlyWhenReleaseRootIsInvalid() {
    const repoRoot = makeTempDir('kai-stage-test-repo-badrelease-');
    const releaseRoot = makeTempDir('kai-stage-test-release-empty-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-badrelease-');

    try {
        assert.throws(
            () => stageCompiler({ repoRoot, extensionRoot, releaseRoot }),
            /could not determine a single bundled target/,
        );
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// RELEASE HARDENING M1.1: a release root built where LLVM was configured
// with Z3 support also contains lib/kai/libz3.so.4 (see CMakeLists.txt) -
// staging must mirror it alongside libkai_runtime.a, by whatever name it
// has, never hard-coded.
function testStagesExtraBundledLibraryFromReleaseRoot() {
    const repoRoot = makeTempDir('kai-stage-test-repo-z3-');
    const releaseRoot = makeTempDir('kai-stage-test-release-z3-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-z3-');

    try {
        const releaseBin = path.join(releaseRoot, 'bin');
        const releaseLib = path.join(releaseRoot, 'lib', 'kai');
        fs.mkdirSync(releaseBin, { recursive: true });
        fs.mkdirSync(releaseLib, { recursive: true });
        fs.writeFileSync(path.join(releaseBin, 'kaicc'), 'portable-kaicc');
        fs.chmodSync(path.join(releaseBin, 'kaicc'), 0o755);
        fs.writeFileSync(path.join(releaseLib, 'libkai_runtime.a'), 'portable-archive');
        fs.writeFileSync(path.join(releaseLib, 'libz3.so.4'), 'fake-z3-contents');
        setupLegalFiles(releaseRoot);

        const result = stageCompiler({ repoRoot, extensionRoot, releaseRoot });

        assert.deepStrictEqual(result.extraLibraries, ['libz3.so.4']);
        const stagedZ3 = path.join(extensionRoot, 'lib', 'kai', 'libz3.so.4');
        assert.ok(fs.existsSync(stagedZ3), 'libz3.so.4 should be staged alongside libkai_runtime.a');
        assert.strictEqual(fs.readFileSync(stagedZ3, 'utf8'), 'fake-z3-contents');
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// A plain local build/ tree never has an extra bundled library - staging
// must report an empty list, not merely "not crash".
function testNoExtraLibrariesStagedFromOrdinaryBuildTree() {
    const repoRoot = makeTempDir('kai-stage-test-repo-noextra-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-noextra-');

    try {
        const buildBin = path.join(repoRoot, 'build', 'bin');
        const buildLib = path.join(repoRoot, 'build', 'lib', 'kai');
        fs.mkdirSync(buildBin, { recursive: true });
        fs.mkdirSync(buildLib, { recursive: true });
        fs.writeFileSync(path.join(buildBin, 'kaicc'), 'local-build-kaicc');
        fs.chmodSync(path.join(buildBin, 'kaicc'), 0o755);
        fs.writeFileSync(path.join(buildLib, 'libkai_runtime.a'), 'local-build-archive');
        setupLegalFiles(repoRoot);

        const result = stageCompiler({ repoRoot, extensionRoot, target: 'linux-x64' });

        assert.deepStrictEqual(result.extraLibraries, []);
        assert.deepStrictEqual(result.extraBinFiles, []);
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// v0.1.0-alpha.1: a release layout missing required legal material must
// fail clearly - a VSIX must never silently lose the project LICENSE or
// third-party notices carried by the binary content it bundles.
function testFailsClearlyWhenLegalMaterialIsMissing() {
    const repoRoot = makeTempDir('kai-stage-test-repo-nolegal-');
    const releaseRoot = makeTempDir('kai-stage-test-release-nolegal-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-nolegal-');

    try {
        const releaseBin = path.join(releaseRoot, 'bin');
        const releaseLib = path.join(releaseRoot, 'lib', 'kai');
        fs.mkdirSync(releaseBin, { recursive: true });
        fs.mkdirSync(releaseLib, { recursive: true });
        fs.writeFileSync(path.join(releaseBin, 'kaicc'), 'portable-kaicc');
        fs.chmodSync(path.join(releaseBin, 'kaicc'), 0o755);
        fs.writeFileSync(path.join(releaseLib, 'libkai_runtime.a'), 'portable-archive');
        // Deliberately no LICENSE/THIRD_PARTY_NOTICES.md/third_party/ here.

        assert.throws(
            () => stageCompiler({ repoRoot, extensionRoot, releaseRoot }),
            /required legal file not found/,
        );
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// ===== VS CODE WINDOWS M4 =====

// The Windows analog of testStagesBothFilesToExpectedPaths: stages
// kaicc.exe + the 5 required DLLs from a fake win32-x64 release root.
function testStagesWin32X64FromReleaseRoot() {
    const repoRoot = makeTempDir('kai-stage-test-repo-win-');
    const releaseRoot = makeTempDir('kai-stage-test-release-win-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-win-');

    try {
        setupWin32ReleaseRoot(releaseRoot);

        const result = stageCompiler({ repoRoot, extensionRoot, releaseRoot });

        // No explicit target given - must be DETECTED from releaseRoot's
        // own contents (bin/kaicc.exe present), never from the host
        // (this test runs on Linux CI/dev machines).
        assert.strictEqual(result.target, 'win32-x64');
        assert.strictEqual(result.kaiccPath, path.join(extensionRoot, 'bin', 'kaicc.exe'));
        assert.ok(fs.existsSync(result.kaiccPath), 'staged kaicc.exe should exist');
        assert.ok(fs.existsSync(path.join(extensionRoot, 'lib', 'kai', 'libkai_runtime.a')));

        for (const dll of WIN32_REQUIRED_DLLS) {
            assert.ok(fs.existsSync(path.join(extensionRoot, 'bin', dll)), `${dll} should be staged into bin/`);
        }
        assert.deepStrictEqual([...result.extraBinFiles].sort(), [...WIN32_REQUIRED_DLLS].sort());

        // §6: must NOT contain any Linux-only payload.
        assert.ok(!fs.existsSync(path.join(extensionRoot, 'bin', 'kaicc')), 'must not stage the Linux ELF kaicc');
        assert.ok(
            !fs.existsSync(path.join(extensionRoot, 'lib', 'kai', 'libz3.so.4')),
            'must not stage Linux-only libz3.so.4 into a Windows target',
        );
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// spec §18: an incomplete Windows release root (missing DLLs) must be
// rejected clearly, never silently packaged.
function testFailsClearlyWhenWin32X64DllsAreMissing() {
    const repoRoot = makeTempDir('kai-stage-test-repo-win-incomplete-');
    const releaseRoot = makeTempDir('kai-stage-test-release-win-incomplete-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-win-incomplete-');

    try {
        setupWin32ReleaseRoot(releaseRoot, { includeAllDlls: false });

        assert.throws(
            () => stageCompiler({ repoRoot, extensionRoot, releaseRoot, target: 'win32-x64' }),
            /missing required runtime DLL/,
        );
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// spec §6/§16: an explicit target that does not match what the release
// root actually contains must fail clearly, never silently stage the
// wrong platform's binary (or nothing at all).
function testFailsClearlyWhenExplicitTargetMismatchesReleaseRoot() {
    const repoRoot = makeTempDir('kai-stage-test-repo-mismatch-');
    const releaseRoot = makeTempDir('kai-stage-test-release-mismatch-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-mismatch-');

    try {
        setupLinuxReleaseRoot(releaseRoot);

        assert.throws(
            () => stageCompiler({ repoRoot, extensionRoot, releaseRoot, target: 'win32-x64' }),
            /kaicc\.exe not found/,
        );
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// spec §6: re-staging a DIFFERENT target into the SAME extensionRoot must
// never leave stale cross-platform files behind (a Linux VSIX build
// directory reused, or vice versa, for a later Windows package run).
function testRestagingDifferentTargetCleansPreviousPlatformFiles() {
    const repoRoot = makeTempDir('kai-stage-test-repo-restage-');
    const linuxReleaseRoot = makeTempDir('kai-stage-test-release-restage-linux-');
    const winReleaseRoot = makeTempDir('kai-stage-test-release-restage-win-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-restage-');

    try {
        setupLinuxReleaseRoot(linuxReleaseRoot);
        const linuxResult = stageCompiler({ repoRoot, extensionRoot, releaseRoot: linuxReleaseRoot });
        assert.strictEqual(linuxResult.target, 'linux-x64');
        assert.ok(fs.existsSync(path.join(extensionRoot, 'bin', 'kaicc')));

        setupWin32ReleaseRoot(winReleaseRoot);
        const winResult = stageCompiler({ repoRoot, extensionRoot, releaseRoot: winReleaseRoot });
        assert.strictEqual(winResult.target, 'win32-x64');

        assert.ok(fs.existsSync(path.join(extensionRoot, 'bin', 'kaicc.exe')), 'kaicc.exe should now be staged');
        assert.ok(
            !fs.existsSync(path.join(extensionRoot, 'bin', 'kaicc')),
            'the STALE Linux kaicc from the previous stage must be removed',
        );
        for (const dll of WIN32_REQUIRED_DLLS) {
            assert.ok(fs.existsSync(path.join(extensionRoot, 'bin', dll)));
        }
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(linuxReleaseRoot, { recursive: true, force: true });
        fs.rmSync(winReleaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

// An explicit target always wins over release-root auto-detection.
function testExplicitTargetIsHonoredOverDetection() {
    const repoRoot = makeTempDir('kai-stage-test-repo-explicit-');
    const releaseRoot = makeTempDir('kai-stage-test-release-explicit-');
    const extensionRoot = makeTempDir('kai-stage-test-ext-explicit-');

    try {
        setupWin32ReleaseRoot(releaseRoot);
        const result = stageCompiler({ repoRoot, extensionRoot, releaseRoot, target: 'win32-x64' });
        assert.strictEqual(result.target, 'win32-x64');
    } finally {
        fs.rmSync(repoRoot, { recursive: true, force: true });
        fs.rmSync(releaseRoot, { recursive: true, force: true });
        fs.rmSync(extensionRoot, { recursive: true, force: true });
    }
}

testStagesBothFilesToExpectedPaths();
testFailsClearlyWhenBuildArtifactsAreMissing();
testStagesFromReleaseRootWhenProvided();
testDefaultSourceIsBuildWhenReleaseRootOmitted();
testFailsClearlyWhenReleaseRootIsInvalid();
testStagesExtraBundledLibraryFromReleaseRoot();
testNoExtraLibrariesStagedFromOrdinaryBuildTree();
testFailsClearlyWhenLegalMaterialIsMissing();
testStagesWin32X64FromReleaseRoot();
testFailsClearlyWhenWin32X64DllsAreMissing();
testFailsClearlyWhenExplicitTargetMismatchesReleaseRoot();
testRestagingDifferentTargetCleansPreviousPlatformFiles();
testExplicitTargetIsHonoredOverDetection();

console.log('stage-compiler.test.mjs: all tests passed');
