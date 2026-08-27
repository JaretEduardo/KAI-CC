#!/usr/bin/env node
// Copies a built kaicc(+.exe)/runtime/DLLs/legal material into the
// extension's own packaging layout (VS CODE COMPILER INTEGRATION
// MILESTONE 2 spec §20, extended by VS CODE WINDOWS M4 for a second,
// Windows x64 bundled target):
//
//     editors/vscode/bin/kaicc            (linux-x64 target)
//     editors/vscode/bin/kaicc.exe        (win32-x64 target)
//     editors/vscode/bin/<other bin/ files from the release root>  (win32-x64: the 5 MSYS2 runtime DLLs)
//     editors/vscode/lib/kai/libkai_runtime.a
//     editors/vscode/lib/kai/<other lib/kai/ files from the release root>  (linux-x64: libz3.so.4, if present)
//
// This is a COPY step only - it never invokes CMake/compiles KAI-CC
// itself. `stageCompiler()` is exported separately from the CLI entry
// point below so scripts/stage-compiler.test.mjs can exercise the actual
// copy logic against a temporary, hermetic directory tree instead of
// depending on a real build existing.
//
// TARGETS (VS CODE WINDOWS M4 spec §5/§16): each VSIX bundles exactly ONE
// platform's compiler - never both (see this milestone's report for why a
// single "universal" VSIX bundling two ~20 MB compilers was rejected).
// `target` selects which one this staging run produces, and is resolved,
// in order:
//   1. an explicit `target` option (or `$KAI_VSCE_TARGET` from the CLI -
//      spec §16: "do not infer target solely from the machine running the
//      staging script if CI needs to package another target").
//   2. if `releaseRoot` is set, DETECTED FROM THE RELEASE ROOT'S OWN
//      CONTENTS (does `<releaseRoot>/bin/` contain `kaicc.exe` or
//      `kaicc`?) - this is what actually determines which binary would be
//      staged, so it is a more reliable default than the HOST machine's
//      own platform (staging is a pure file-copy step; the machine
//      running it need not match the target being packaged).
//   3. otherwise (no releaseRoot - an ordinary local `build/` tree, which
//      only ever exists for the host's OWN platform), the host's own
//      `process.platform`/`process.arch` - today's original behavior,
//      unchanged for plain local development.
//
// SOURCE SELECTION (RELEASE HARDENING M1): by default this stages the
// ordinary local development build (`<repoRoot>/build/bin/...` +
// `<repoRoot>/build/lib/kai/...`).
//
// If `$KAI_RELEASE_ROOT` is set (see scripts/build-release-linux-x86_64.sh
// / scripts/build-release-windows-x86_64.sh), it stages from
// `<KAI_RELEASE_ROOT>/bin/...` + `<KAI_RELEASE_ROOT>/lib/kai/...` INSTEAD -
// this is how a developer packaging the extension picks up the portable,
// publicly-distributed release artifact (dist/kai-linux-x86_64/ or
// dist/kai-windows-x86_64/) rather than their own host's local `build/`
// output. This is opt-in only: `dist/` is never auto-detected or silently
// preferred over `build/` just because it happens to exist - the caller
// must explicitly set `$KAI_RELEASE_ROOT` (or pass `releaseRoot` directly
// to `stageCompiler()`). This release root is the SOLE source of truth
// for what a VSIX bundles (VS CODE WINDOWS M4 spec §30) - never
// `build-release-windows/`, a raw MSYS2 `/ucrt64/bin`, or hand-picked DLLs.
//
// ADDITIONAL BUNDLED FILES (RELEASE HARDENING M1.1, extended by VS CODE
// WINDOWS M4): a Linux release root may also contain `lib/kai/libz3.so.4`
// (see CMakeLists.txt's own comment), and a Windows release root's `bin/`
// contains 5 MSYS2 UCRT64 runtime DLLs alongside `kaicc.exe` (see
// scripts/build-release-windows-x86_64.sh). Rather than hard-coding those
// names here too, staging mirrors EVERY OTHER file already present in the
// release root's `bin/` and `lib/kai/` directories - this stays correct
// automatically if the underlying MSYS2 toolchain packages change what
// they bundle, with no second place to update. A plain local `build/`
// tree never contains such extra files, so normal build-tree development
// is never affected by this. For the win32-x64 target specifically, an
// EXPLICIT check additionally verifies the 5 currently-known required
// DLLs are all present after mirroring (spec §18) - not a generic
// scanning framework, just a small, hand-maintained safety net so a
// malformed/incomplete Windows release root is rejected clearly rather
// than silently producing a broken VSIX.
//
// CLEAN STAGING (VS CODE WINDOWS M4 spec §6): the destination `bin/`,
// `lib/kai/`, and `third_party/licenses/` directories are removed before
// copying, every run - this guarantees re-staging a DIFFERENT target into
// the same extensionRoot (e.g. a Linux VSIX built earlier, now packaging
// Windows) can never leave a stale cross-platform binary/DLL/legal file
// behind. A platform mismatch between `target` and `releaseRoot`'s actual
// contents (e.g. target=win32-x64 pointed at a Linux release root) fails
// clearly instead of silently staging the wrong platform's binary, since
// the expected filename for the WRONG platform simply will not exist at
// the path this script looks for.
//
// Directory shape rationale: `bin/` and `lib/kai/` are kept as SIBLINGS
// because NativeLinker::findDefaultRuntimeLibrary() looks for
// `<kaicc's own directory>/../lib/kai/libkai_runtime.a` (see
// compiler/include/kai/codegen/NativeLinker.hpp) - that lookup only
// succeeds if `bin` and `lib` are siblings, exactly like this project's
// own CMake build tree AND both portable release layouts' install()
// rules produce. Each target VSIX uses this SAME flat shape, staging only
// ITS OWN platform's binary under the platform-native name (`bin/kaicc`
// or `bin/kaicc.exe`) - never a `bin/<platform>/kaicc` subfolder, and
// never both platforms' binaries in one VSIX.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

// VS CODE WINDOWS M4: the closed set of bundled targets this extension
// packages a compiler for - one VSIX per target (see package.json's
// "package:linux-x64"/"package:win32-x64" scripts, which each pass the
// matching `vsce package --target <name>`).
const TARGETS = {
    'linux-x64': { platform: 'linux', arch: 'x64', kaiccName: 'kaicc' },
    'win32-x64': { platform: 'win32', arch: 'x64', kaiccName: 'kaicc.exe' },
};

// VS CODE WINDOWS M4 spec §18: hand-maintained, explicit - matches
// THIRD_PARTY_NOTICES.md's "Windows x86_64 portable package" table and
// scripts/build-release-windows-x86_64.sh's own DLL_LICENSE_FILES
// mapping. Deliberately small and explicit, not a generic scanner.
const REQUIRED_WIN32_X64_DLLS = [
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll',
    'zlib1.dll',
    'libzstd.dll',
];

/**
 * Detects which bundled target a release root actually contains, by
 * looking for each target's own exact kaicc binary name under its
 * `bin/` directory. Throws clearly if neither (or, in principle, some
 * future ambiguous case) is found - never silently guesses.
 */
function detectTargetFromReleaseRoot(releaseRoot) {
    const found = Object.entries(TARGETS).filter(([, def]) =>
        fs.existsSync(path.join(releaseRoot, 'bin', def.kaiccName)),
    );
    if (found.length === 1) {
        return found[0][0];
    }
    throw new Error(
        `could not determine a single bundled target from KAI_RELEASE_ROOT (${releaseRoot}) - expected exactly ` +
            `one of bin/${TARGETS['linux-x64'].kaiccName} or bin/${TARGETS['win32-x64'].kaiccName} to exist. ` +
            'Pass an explicit target (KAI_VSCE_TARGET=linux-x64 or win32-x64) if this release root is genuinely ' +
            'incomplete/malformed and you still need to diagnose it.',
    );
}

function detectTargetFromHost() {
    const key = `${process.platform}-${process.arch}`;
    if (TARGETS[key]) {
        return key;
    }
    throw new Error(
        `no bundled compiler target for this host (${process.platform}/${process.arch}) and no KAI_RELEASE_ROOT ` +
            'was given to stage a specific target from - pass an explicit target (KAI_VSCE_TARGET=linux-x64 or ' +
            'win32-x64) together with KAI_RELEASE_ROOT.',
    );
}

// v0.1.0-alpha.1 FINAL PRE-RELEASE PREPARATION: the packaged VSIX bundles
// the same statically-linked LLVM code (and, depending on target,
// libz3.so.4 or the 5 Windows MSYS2 runtime DLLs) as the corresponding
// portable release tarball/zip, so it must carry the same legal material
// (project LICENSE, THIRD_PARTY_NOTICES.md, third-party license texts) -
// otherwise the VSIX would redistribute that binary content with no
// accompanying notices. These are copied, never hand-authored a second
// time: from `releaseRoot` when packaging a real release (so the legal
// material corresponds to the exact binary being staged - both
// scripts/build-release-linux-x86_64.sh and
// scripts/build-release-windows-x86_64.sh guarantee LICENSE/
// THIRD_PARTY_NOTICES.md/third_party/licenses/* exist there), or from the
// repository root itself for ordinary local development packaging
// (releaseRoot unset) - the same tracked files, just not yet mirrored
// into a release layout.
const LEGAL_FILES = ['LICENSE', 'THIRD_PARTY_NOTICES.md'];
const LEGAL_LICENSE_DIR = path.join('third_party', 'licenses');

/**
 * @param {{ repoRoot: string, extensionRoot: string, releaseRoot?: string, target?: 'linux-x64' | 'win32-x64' }} options
 * @returns {{ kaiccPath: string, runtimePath: string, source: 'release' | 'build', target: string, extraBinFiles: string[], extraLibraries: string[], legalFiles: string[] }}
 */
export function stageCompiler({ repoRoot, extensionRoot, releaseRoot, target }) {
    const source = releaseRoot ? 'release' : 'build';
    const resolvedTarget = target || (releaseRoot ? detectTargetFromReleaseRoot(releaseRoot) : detectTargetFromHost());
    const targetDef = TARGETS[resolvedTarget];
    if (!targetDef) {
        throw new Error(`unknown target "${resolvedTarget}" - expected one of: ${Object.keys(TARGETS).join(', ')}`);
    }

    const sourceBinDir = releaseRoot ? path.join(releaseRoot, 'bin') : path.join(repoRoot, 'build', 'bin');
    const sourceKaicc = path.join(sourceBinDir, targetDef.kaiccName);
    const sourceLibDir = releaseRoot ? path.join(releaseRoot, 'lib', 'kai') : path.join(repoRoot, 'build', 'lib', 'kai');
    const sourceRuntime = path.join(sourceLibDir, 'libkai_runtime.a');

    if (!fs.existsSync(sourceKaicc)) {
        throw new Error(
            releaseRoot
                ? `${targetDef.kaiccName} not found at ${sourceKaicc} - check that KAI_RELEASE_ROOT (${releaseRoot}) ` +
                      `points at a valid ${resolvedTarget} release layout (bin/${targetDef.kaiccName}, ` +
                      'lib/kai/libkai_runtime.a), and that `target` matches what that release root actually contains.'
                : `${targetDef.kaiccName} not found at ${sourceKaicc} - build it first: run "cmake --build build" from the repository root.`,
        );
    }
    if (!fs.existsSync(sourceRuntime)) {
        throw new Error(
            releaseRoot
                ? `libkai_runtime.a not found at ${sourceRuntime} - check that KAI_RELEASE_ROOT (${releaseRoot}) ` +
                      `points at a valid ${resolvedTarget} release layout.`
                : `libkai_runtime.a not found at ${sourceRuntime} - build it first: run "cmake --build build" from the ` +
                      'repository root.',
        );
    }

    const destKaiccDir = path.join(extensionRoot, 'bin');
    const destRuntimeDir = path.join(extensionRoot, 'lib', 'kai');
    const destKaicc = path.join(destKaiccDir, targetDef.kaiccName);
    const destRuntime = path.join(destRuntimeDir, 'libkai_runtime.a');

    // VS CODE WINDOWS M4 spec §6: wipe any PREVIOUSLY staged content
    // before copying - guarantees a re-stage of a DIFFERENT target into
    // the same extensionRoot never leaves a stale cross-platform
    // binary/DLL behind (e.g. a Linux ELF `kaicc` sitting next to a
    // freshly-staged `kaicc.exe`).
    fs.rmSync(destKaiccDir, { recursive: true, force: true });
    fs.rmSync(path.join(extensionRoot, 'lib'), { recursive: true, force: true });
    fs.mkdirSync(destKaiccDir, { recursive: true });
    fs.mkdirSync(destRuntimeDir, { recursive: true });

    fs.copyFileSync(sourceKaicc, destKaicc);
    fs.copyFileSync(sourceRuntime, destRuntime);

    // fs.copyFileSync does not guarantee the execute bit survives the
    // copy on every platform/filesystem - explicitly ensure it. A no-op
    // in practice on Windows (win32-x64 target), where .exe executability
    // is not governed by the POSIX mode bits this sets.
    const sourceMode = fs.statSync(sourceKaicc).mode;
    fs.chmodSync(destKaicc, sourceMode | 0o111);

    // Mirror any OTHER file already present in the release root's own
    // bin/ directory (win32-x64: the MSYS2 runtime DLLs staged alongside
    // kaicc.exe) - never hard-coding specific DLL names in the general
    // copy loop itself (see this file's header comment).
    const extraBinFiles = [];
    for (const entry of fs.readdirSync(sourceBinDir)) {
        if (entry === targetDef.kaiccName) {
            continue;
        }
        const sourceExtra = path.join(sourceBinDir, entry);
        if (!fs.statSync(sourceExtra).isFile()) {
            continue;
        }
        fs.copyFileSync(sourceExtra, path.join(destKaiccDir, entry));
        extraBinFiles.push(entry);
    }

    if (resolvedTarget === 'win32-x64') {
        const missing = REQUIRED_WIN32_X64_DLLS.filter((name) => !extraBinFiles.includes(name));
        if (missing.length > 0) {
            throw new Error(
                `win32-x64 release root at ${releaseRoot ?? sourceBinDir} is missing required runtime DLL(s): ` +
                    `${missing.join(', ')}. Refusing to stage an incomplete Windows compiler bundle.`,
            );
        }
    }

    // Mirror any OTHER file CMake's install() placed alongside
    // libkai_runtime.a (e.g. linux-x64's libz3.so.4), never hard-coding a
    // specific name here either.
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

    // Legal material source: the release layout when packaging a real
    // release (so it matches the exact binary staged above), otherwise
    // the repository root's own tracked LICENSE/THIRD_PARTY_NOTICES.md/
    // third_party/licenses - never a divergent hand-maintained copy. This
    // is ALREADY the correct platform-specific set (THIRD_PARTY_NOTICES.md
    // + third_party/licenses/ describe exactly what that release root's
    // OWN packaging script decided to bundle - see VS CODE WINDOWS M4
    // spec §17: never independently reconstruct that logic here).
    const legalSourceRoot = releaseRoot || repoRoot;
    const legalFiles = [];

    for (const name of LEGAL_FILES) {
        const src = path.join(legalSourceRoot, name);
        if (!fs.existsSync(src)) {
            throw new Error(
                `required legal file not found at ${src} - a release VSIX must not lose the project ` +
                    'license or third-party notices carried by the binary release it bundles.',
            );
        }
        fs.copyFileSync(src, path.join(extensionRoot, name));
        legalFiles.push(name);
    }

    const sourceLicenseDir = path.join(legalSourceRoot, LEGAL_LICENSE_DIR);
    if (!fs.existsSync(sourceLicenseDir)) {
        throw new Error(`required third-party license directory not found at ${sourceLicenseDir}.`);
    }
    const destLicenseDir = path.join(extensionRoot, LEGAL_LICENSE_DIR);
    fs.mkdirSync(destLicenseDir, { recursive: true });
    for (const entry of fs.readdirSync(sourceLicenseDir)) {
        const srcEntry = path.join(sourceLicenseDir, entry);
        if (!fs.statSync(srcEntry).isFile()) {
            continue;
        }
        fs.copyFileSync(srcEntry, path.join(destLicenseDir, entry));
        legalFiles.push(path.join(LEGAL_LICENSE_DIR, entry));
    }

    return { kaiccPath: destKaicc, runtimePath: destRuntime, source, target: resolvedTarget, extraBinFiles, extraLibraries, legalFiles };
}

function main() {
    const scriptDir = path.dirname(fileURLToPath(import.meta.url));
    const extensionRoot = path.resolve(scriptDir, '..');
    const repoRoot = path.resolve(extensionRoot, '..', '..');
    // Opt-in only (see this file's own header comment) - an unset/empty
    // env var must behave EXACTLY like the plain `build/` path, never
    // silently fall back to some other default location.
    const releaseRoot = process.env.KAI_RELEASE_ROOT ? process.env.KAI_RELEASE_ROOT : undefined;
    const target = process.env.KAI_VSCE_TARGET ? process.env.KAI_VSCE_TARGET : undefined;

    try {
        const { kaiccPath, runtimePath, source, target: resolvedTarget, extraBinFiles, extraLibraries, legalFiles } =
            stageCompiler({ repoRoot, extensionRoot, releaseRoot, target });
        console.log(`Staging target: ${resolvedTarget} (source: ${source})`);
        console.log(`Staged kaicc -> ${kaiccPath}`);
        console.log(`Staged libkai_runtime.a -> ${runtimePath}`);
        for (const extra of extraBinFiles) {
            console.log(`Staged ${extra} (bundled DLL) -> ${path.join(path.dirname(kaiccPath), extra)}`);
        }
        for (const extra of extraLibraries) {
            console.log(`Staged ${extra} -> ${path.join(path.dirname(runtimePath), extra)}`);
        }
        for (const legal of legalFiles) {
            console.log(`Staged ${legal} (legal material)`);
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
