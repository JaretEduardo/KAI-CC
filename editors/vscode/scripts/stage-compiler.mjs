#!/usr/bin/env node
// Copies a built kaicc + libkai_runtime.a into the extension's own
// packaging layout (VS CODE COMPILER INTEGRATION MILESTONE 2 spec §20):
//
//     editors/vscode/bin/kaicc
//     editors/vscode/lib/kai/libkai_runtime.a
//
// This is a COPY step only - it never invokes CMake/compiles KAI-CC
// itself. `stageCompiler()` is exported separately from the CLI entry
// point below so scripts/stage-compiler.test.mjs can exercise the actual
// copy logic against a temporary, hermetic directory tree instead of
// depending on a real build existing.
//
// SOURCE SELECTION (RELEASE HARDENING M1): by default this stages the
// ordinary local development build (`<repoRoot>/build/bin/kaicc` +
// `<repoRoot>/build/lib/kai/libkai_runtime.a`) - unchanged from before.
//
// If `$KAI_RELEASE_ROOT` is set (see scripts/build-release-linux-x86_64.sh),
// it stages from `<KAI_RELEASE_ROOT>/bin/kaicc` +
// `<KAI_RELEASE_ROOT>/lib/kai/libkai_runtime.a` INSTEAD - this is how a
// developer packaging the extension picks up the portable, Ubuntu-22.04-
// built release artifact (dist/kai-linux-x86_64/) rather than their own
// host's local `build/` output, which may be built against a much newer
// glibc/libstdc++ baseline (this project's Fedora development host, for
// example) and would silently make the shipped VSIX non-portable. This is
// opt-in only: `dist/` is never auto-detected or silently preferred over
// `build/` just because it happens to exist - the caller must explicitly
// set `$KAI_RELEASE_ROOT` (or pass `releaseRoot` directly to
// `stageCompiler()`).
//
// ADDITIONAL BUNDLED LIBRARIES (RELEASE HARDENING M1.1): a release layout
// built where LLVM was configured with Z3 support (Ubuntu 22.04's
// apt.llvm.org packages, confirmed) also contains `lib/kai/libz3.so.4`
// (see CMakeLists.txt's own comment - LLVMSupport unconditionally depends
// on it there, and no static libz3.a exists to link instead). Rather than
// hard-coding that one filename here too, staging mirrors EVERY file
// CMake's own install() rules placed in the source `lib/kai/` directory,
// not just the two always-expected ones - this stays correct
// automatically if a future platform/LLVM build bundles some other
// runtime library the same way, with no second place to update. A plain
// local `build/` tree never contains such an extra file (CMake's
// ARCHIVE_OUTPUT_DIRECTORY only ever places libkai_runtime.a there), so
// normal build-tree development is never affected by this.
//
// Directory shape rationale: `bin/` and `lib/kai/` are kept as SIBLINGS
// (not `bin/linux-x64/kaicc` as a first draft of this milestone's spec
// suggested) because NativeLinker::findDefaultRuntimeLibrary() looks for
// `<kaicc's own directory>/../lib/kai/libkai_runtime.a` (see
// compiler/include/kai/codegen/NativeLinker.hpp) - that lookup only
// succeeds if `bin` and `lib` are siblings, exactly like this project's
// own CMake build tree AND the portable release layout CMake's own
// install() rules now produce (see CMakeLists.txt). Per-platform packages
// (when other platforms are eventually supported) should be produced as
// separate `vsce package --target <platform>` VSIX files, each internally
// using this SAME flat shape - not one VSIX with multiple platform
// subfolders.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

/**
 * @param {{ repoRoot: string, extensionRoot: string, releaseRoot?: string }} options
 * @returns {{ kaiccPath: string, runtimePath: string, source: 'release' | 'build', extraLibraries: string[] }}
 */
export function stageCompiler({ repoRoot, extensionRoot, releaseRoot }) {
    const source = releaseRoot ? 'release' : 'build';
    const sourceKaicc = releaseRoot
        ? path.join(releaseRoot, 'bin', 'kaicc')
        : path.join(repoRoot, 'build', 'bin', 'kaicc');
    const sourceLibDir = releaseRoot
        ? path.join(releaseRoot, 'lib', 'kai')
        : path.join(repoRoot, 'build', 'lib', 'kai');
    const sourceRuntime = path.join(sourceLibDir, 'libkai_runtime.a');

    if (!fs.existsSync(sourceKaicc)) {
        throw new Error(
            releaseRoot
                ? `kaicc not found at ${sourceKaicc} - check that KAI_RELEASE_ROOT (${releaseRoot}) points at a ` +
                      'valid release layout produced by scripts/build-release-linux-x86_64.sh (bin/kaicc, ' +
                      'lib/kai/libkai_runtime.a).'
                : `kaicc not found at ${sourceKaicc} - build it first: run "cmake --build build" from the repository root.`,
        );
    }
    if (!fs.existsSync(sourceRuntime)) {
        throw new Error(
            releaseRoot
                ? `libkai_runtime.a not found at ${sourceRuntime} - check that KAI_RELEASE_ROOT (${releaseRoot}) ` +
                      'points at a valid release layout produced by scripts/build-release-linux-x86_64.sh.'
                : `libkai_runtime.a not found at ${sourceRuntime} - build it first: run "cmake --build build" from the ` +
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

    // See this file's own header comment ("ADDITIONAL BUNDLED LIBRARIES")
    // - mirror any OTHER file CMake's install() placed alongside
    // libkai_runtime.a (e.g. libz3.so.4), never hard-coding a specific
    // name here.
    const extraLibraries = [];
    for (const entry of fs.readdirSync(sourceLibDir)) {
        if (entry === 'libkai_runtime.a') {
            continue;
        }
        const sourceExtra = path.join(sourceLibDir, entry);
        if (!fs.statSync(sourceExtra).isFile()) {
            continue;
        }
        fs.copyFileSync(sourceExtra, path.join(destRuntimeDir, entry));
        extraLibraries.push(entry);
    }

    return { kaiccPath: destKaicc, runtimePath: destRuntime, source, extraLibraries };
}

function main() {
    const scriptDir = path.dirname(fileURLToPath(import.meta.url));
    const extensionRoot = path.resolve(scriptDir, '..');
    const repoRoot = path.resolve(extensionRoot, '..', '..');
    // Opt-in only (see this file's own header comment) - an unset/empty
    // env var must behave EXACTLY like the plain `build/` path, never
    // silently fall back to some other default location.
    const releaseRoot = process.env.KAI_RELEASE_ROOT ? process.env.KAI_RELEASE_ROOT : undefined;

    try {
        const { kaiccPath, runtimePath, source, extraLibraries } = stageCompiler({ repoRoot, extensionRoot, releaseRoot });
        console.log(`Staged kaicc (source: ${source}) -> ${kaiccPath}`);
        console.log(`Staged libkai_runtime.a (source: ${source}) -> ${runtimePath}`);
        for (const extra of extraLibraries) {
            console.log(`Staged ${extra} (source: ${source}) -> ${path.join(path.dirname(runtimePath), extra)}`);
        }
        if (source === 'release') {
            console.log(`  KAI_RELEASE_ROOT=${releaseRoot}`);
        }
    } catch (err) {
        console.error(`stage-compiler: ${err instanceof Error ? err.message : String(err)}`);
        process.exitCode = 1;
    }
}

const invokedDirectly = process.argv[1] !== undefined && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (invokedDirectly) {
    main();
}
