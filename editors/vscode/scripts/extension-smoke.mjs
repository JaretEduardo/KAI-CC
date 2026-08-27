#!/usr/bin/env node
// VS CODE WINDOWS M4 spec §25: exercises the SAME compiled helper
// functions "KAI: Build Current File"/"KAI: Run Current File" actually
// use (out/paths.js's resolveCompilerPath()/computeOutputPath(),
// out/process.js's spawnProcess()) against a REAL staged bundled
// compiler - not a fake/mocked one, and not merely string-level path
// assertions. Deliberately vscode-API-free (like paths.ts/process.ts
// themselves), so it runs as a plain Node script in CI, on both the
// Linux and Windows bundled-compiler packaging jobs, proving the exact
// same code path the extension itself takes: locate the bundled
// compiler for THIS platform -> compute the output path -> compile a
// real .kai file -> confirm the produced executable exists at the
// EXACT computed path -> run it -> assert its stdout.
//
// Usage:
//   node scripts/extension-smoke.mjs <extensionRoot> <hello.kai path> <expected stdout>
//
// Run only from CI after `npm run compile` + `npm run stage-compiler`
// have staged a real compiler into <extensionRoot>/bin/ - never part of
// the ordinary `npm test` run, since it requires a real bundled
// compiler AND a host C toolchain on PATH to link the compiled program
// (see this milestone's report - the same host-toolchain requirement
// documented in commands.ts's explainExitCode()).

import assert from 'assert';
import fs from 'fs';
import path from 'path';

import { resolveCompilerPath, computeOutputPath } from '../out/paths.js';
import { spawnProcess } from '../out/process.js';

async function main() {
    const [, , extensionRoot, sourcePath, expectedStdout] = process.argv;
    if (!extensionRoot || !sourcePath || expectedStdout === undefined) {
        console.error('usage: node scripts/extension-smoke.mjs <extensionRoot> <source.kai> <expectedStdout>');
        process.exitCode = 1;
        return;
    }

    console.log(`platform=${process.platform} arch=${process.arch}`);

    const located = resolveCompilerPath(path.resolve(extensionRoot), undefined, process.platform, process.arch);
    assert.ok(located.ok, `resolveCompilerPath failed: ${located.ok ? '' : located.message}`);
    console.log(`bundled compiler resolved -> ${located.kaiccPath}`);
    assert.ok(fs.existsSync(located.kaiccPath), 'resolved bundled compiler path must actually exist on disk');

    const outputPath = computeOutputPath(path.resolve(sourcePath), process.platform);
    console.log(`computed output path -> ${outputPath}`);
    if (process.platform === 'win32') {
        assert.ok(/\.exe$/i.test(outputPath), 'computeOutputPath must append .exe on win32');
    }

    fs.rmSync(outputPath, { force: true });

    const compileResult = await spawnProcess(located.kaiccPath, [path.resolve(sourcePath), '-o', outputPath]);
    if (compileResult.stdout) {
        console.log(`kaicc stdout: ${compileResult.stdout}`);
    }
    if (compileResult.stderr) {
        console.log(`kaicc stderr: ${compileResult.stderr}`);
    }
    assert.strictEqual(compileResult.failedToStart, false, 'compiler process must start');
    assert.strictEqual(compileResult.exitCode, 0, `compile must succeed (exit ${compileResult.exitCode})`);
    assert.ok(fs.existsSync(outputPath), `expected compiled executable at exactly ${outputPath}`);
    console.log('compile: OK');

    const runResult = await spawnProcess(outputPath, [], path.dirname(path.resolve(sourcePath)));
    assert.strictEqual(runResult.failedToStart, false, 'compiled program must launch');
    assert.strictEqual(runResult.exitCode, 0, `program must exit 0 (got ${runResult.exitCode})`);
    assert.strictEqual(runResult.stdout, expectedStdout, 'program stdout must match exactly (LF-only, per WINDOWS M1.1)');
    console.log('run: OK');

    fs.rmSync(outputPath, { force: true });
    console.log('extension-smoke: OK');
}

main().catch((err) => {
    console.error(err);
    process.exitCode = 1;
});
