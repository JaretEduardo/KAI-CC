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

        const result = stageCompiler({ repoRoot, extensionRoot });

        const expectedKaicc = path.join(extensionRoot, 'bin', 'kaicc');
        const expectedRuntime = path.join(extensionRoot, 'lib', 'kai', 'libkai_runtime.a');

        assert.strictEqual(result.kaiccPath, expectedKaicc);
        assert.strictEqual(result.runtimePath, expectedRuntime);

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
        assert.throws(() => stageCompiler({ repoRoot, extensionRoot }), /kaicc not found/);
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

        const result = stageCompiler({ repoRoot, extensionRoot });

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
            /KAI_RELEASE_ROOT/,
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

        const result = stageCompiler({ repoRoot, extensionRoot });

        assert.deepStrictEqual(result.extraLibraries, []);
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

testStagesBothFilesToExpectedPaths();
testFailsClearlyWhenBuildArtifactsAreMissing();
testStagesFromReleaseRootWhenProvided();
testDefaultSourceIsBuildWhenReleaseRootOmitted();
testFailsClearlyWhenReleaseRootIsInvalid();
testStagesExtraBundledLibraryFromReleaseRoot();
testNoExtraLibrariesStagedFromOrdinaryBuildTree();
testFailsClearlyWhenLegalMaterialIsMissing();

console.log('stage-compiler.test.mjs: all tests passed');
