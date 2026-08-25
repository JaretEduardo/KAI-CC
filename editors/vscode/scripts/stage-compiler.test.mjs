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

testStagesBothFilesToExpectedPaths();
testFailsClearlyWhenBuildArtifactsAreMissing();

console.log('stage-compiler.test.mjs: all tests passed');
