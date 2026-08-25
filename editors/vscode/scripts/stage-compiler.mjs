#!/usr/bin/env node
// Copies the CURRENT CMake build artifacts (build/bin/kaicc,
// build/lib/kai/libkai_runtime.a) into the extension's own packaging
// layout (VS CODE COMPILER INTEGRATION MILESTONE 2 spec §20):
//
//     editors/vscode/bin/kaicc
//     editors/vscode/lib/kai/libkai_runtime.a
//
// This is a COPY step only - it never invokes CMake/compiles KAI-CC
// itself (CMake remains solely responsible for producing kaicc/the
// runtime). `stageCompiler()` is exported separately from the CLI
// entry point below so scripts/stage-compiler.test.mjs can exercise the
// actual copy logic against a temporary, hermetic directory tree instead
// of depending on a real build existing.
//
// Directory shape rationale: `bin/` and `lib/kai/` are kept as SIBLINGS
// (not `bin/linux-x64/kaicc` as a first draft of this milestone's spec
// suggested) because NativeLinker::findDefaultRuntimeLibrary() looks for
// `<kaicc's own directory>/../lib/kai/libkai_runtime.a` (see
// compiler/include/kai/codegen/NativeLinker.hpp) - that lookup only
// succeeds if `bin` and `lib` are siblings, exactly like this project's
// own CMake build tree (`build/bin/kaicc` + `build/lib/kai/
// libkai_runtime.a`). Per-platform packages (when other platforms are
// eventually supported) should be produced as separate `vsce package
// --target <platform>` VSIX files, each internally using this SAME flat
// shape - not one VSIX with multiple platform subfolders.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

/**
 * @param {{ repoRoot: string, extensionRoot: string }} options
 * @returns {{ kaiccPath: string, runtimePath: string }}
 */
export function stageCompiler({ repoRoot, extensionRoot }) {
    const sourceKaicc = path.join(repoRoot, 'build', 'bin', 'kaicc');
    const sourceRuntime = path.join(repoRoot, 'build', 'lib', 'kai', 'libkai_runtime.a');

    if (!fs.existsSync(sourceKaicc)) {
        throw new Error(
            `kaicc not found at ${sourceKaicc} - build it first: run "cmake --build build" from the repository root.`,
        );
    }
    if (!fs.existsSync(sourceRuntime)) {
        throw new Error(
            `libkai_runtime.a not found at ${sourceRuntime} - build it first: run "cmake --build build" from the ` +
                'repository root.',
        );
    }

    const destKaiccDir = path.join(extensionRoot, 'bin');
    const destRuntimeDir = path.join(extensionRoot, 'lib', 'kai');
    const destKaicc = path.join(destKaiccDir, 'kaicc');
    const destRuntime = path.join(destRuntimeDir, 'libkai_runtime.a');

    fs.mkdirSync(destKaiccDir, { recursive: true });
    fs.mkdirSync(destRuntimeDir, { recursive: true });

    fs.copyFileSync(sourceKaicc, destKaicc);
    fs.copyFileSync(sourceRuntime, destRuntime);

    // fs.copyFileSync does not guarantee the execute bit survives the
    // copy on every platform/filesystem - explicitly ensure it (Unix).
    const sourceMode = fs.statSync(sourceKaicc).mode;
    fs.chmodSync(destKaicc, sourceMode | 0o111);

    return { kaiccPath: destKaicc, runtimePath: destRuntime };
}

function main() {
    const scriptDir = path.dirname(fileURLToPath(import.meta.url));
    const extensionRoot = path.resolve(scriptDir, '..');
    const repoRoot = path.resolve(extensionRoot, '..', '..');

    try {
        const { kaiccPath, runtimePath } = stageCompiler({ repoRoot, extensionRoot });
        console.log(`Staged kaicc -> ${kaiccPath}`);
        console.log(`Staged libkai_runtime.a -> ${runtimePath}`);
    } catch (err) {
        console.error(`stage-compiler: ${err instanceof Error ? err.message : String(err)}`);
        process.exitCode = 1;
    }
}

const invokedDirectly = process.argv[1] !== undefined && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (invokedDirectly) {
    main();
}
