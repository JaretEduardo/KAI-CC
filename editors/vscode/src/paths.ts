// Pure, vscode-API-free path/policy logic (VS CODE COMPILER INTEGRATION
// MILESTONE 2 spec §2/§27, extended by VS CODE WINDOWS M4): kept separate
// from compiler.ts so it can be unit-tested with plain Node, without
// needing the `vscode` module to be resolvable (that module only exists
// inside a running VS Code extension host - importing it directly in a
// standalone test process throws).

import * as fs from 'fs';
import * as nodePath from 'path';

export type CompilerLocateResult = { ok: true; kaiccPath: string } | { ok: false; message: string };

// VS CODE WINDOWS M4 spec §3/§4/§7: the explicit, closed set of bundled
// compiler targets this extension ships a compiler for - one VSIX per
// target (see scripts/stage-compiler.mjs), each staging its OWN
// platform's binary under the SAME relative `bin/<name>` path inside the
// extension. Never derived by guessing/appending ".exe" to an arbitrary
// path - each target names its own exact bundled binary filename, matching
// exactly what scripts/build-release-<target>.sh's release root (and
// scripts/stage-compiler.mjs, which copies from it) actually produces.
interface BundledCompilerTarget {
    platform: NodeJS.Platform;
    arch: string;
    /** Relative to the extension's own root, e.g. "bin/kaicc.exe". */
    relativeBinaryPath: string;
}

const BUNDLED_TARGETS: readonly BundledCompilerTarget[] = [
    { platform: 'linux', arch: 'x64', relativeBinaryPath: nodePath.join('bin', 'kaicc') },
    { platform: 'win32', arch: 'x64', relativeBinaryPath: nodePath.join('bin', 'kaicc.exe') },
];

const SUPPORTED_PLATFORMS_DESCRIPTION = 'Linux x64 or Windows x64';

/**
 * Resolves which `kaicc` executable to invoke, per the spec's fixed
 * priority order (§8, unchanged by VS CODE WINDOWS M4 - only the set of
 * supported bundled targets grew):
 *
 *   1. an explicitly configured path (`kai.compilerPath`), if non-empty -
 *      an invalid configured path is a hard, reported error, never a
 *      silent fallback to the bundled compiler (deterministic packaging
 *      behavior, §8).
 *   2. the compiler bundled with the extension, at
 *      `<extensionPath>/bin/kaicc` (Linux x64) or
 *      `<extensionPath>/bin/kaicc.exe` (Windows x64) - the only two
 *      currently-supported platforms (VS CODE WINDOWS M4 spec §3). No
 *      PATH search ever happens here (§8) - an unsupported
 *      platform/arch combination (darwin, arm64, ...) is a clear,
 *      reported error, never a guess.
 *
 * `fileExists` is injectable (defaults to `fs.existsSync`) purely so this
 * function stays testable without touching the real filesystem.
 */
export function resolveCompilerPath(
    extensionPath: string,
    configuredPath: string | undefined,
    platform: NodeJS.Platform,
    arch: string,
    fileExists: (candidate: string) => boolean = fs.existsSync,
): CompilerLocateResult {
    const trimmedConfigured = configuredPath?.trim();
    if (trimmedConfigured) {
        if (!fileExists(trimmedConfigured)) {
            return {
                ok: false,
                message: `Configured "kai.compilerPath" does not exist: ${trimmedConfigured}`,
            };
        }
        return { ok: true, kaiccPath: trimmedConfigured };
    }

    const target = BUNDLED_TARGETS.find((t) => t.platform === platform && t.arch === arch);
    if (!target) {
        return {
            ok: false,
            message:
                `Bundled KAI compiler is currently available only for ${SUPPORTED_PLATFORMS_DESCRIPTION} ` +
                `(this platform reports "${platform}"/"${arch}"). ` +
                'Set "kai.compilerPath" to use a compiler built for this platform.',
        };
    }

    const bundledPath = nodePath.join(extensionPath, target.relativeBinaryPath);
    if (!fileExists(bundledPath)) {
        return {
            ok: false,
            message:
                `Bundled KAI compiler not found at ${bundledPath}. ` +
                'The extension package may be corrupt, or (during development) the compiler has not been ' +
                'staged yet - run "npm run stage-compiler".',
        };
    }
    return { ok: true, kaiccPath: bundledPath };
}

/**
 * The exact same ".exe"-suffix decision as the compiler's own
 * `kai::cli::resolveNativeExecutablePath()` (compiler/src/cli/
 * CompileCommand.cpp) - kept independently here (VS CODE WINDOWS M4
 * spec §8) because the extension must know the REAL final path kaicc
 * will produce BEFORE invoking it (to check the result and to launch it
 * afterward), not just pass a raw basename through. A no-op on every
 * platform except Windows; never double-suffixes a path that already
 * ends in ".exe" (case-insensitively).
 */
function applyNativeExecutableSuffix(requestedOutputPath: string, platform: NodeJS.Platform): string {
    if (platform !== 'win32') {
        return requestedOutputPath;
    }
    if (/\.exe$/i.test(requestedOutputPath)) {
        return requestedOutputPath;
    }
    return `${requestedOutputPath}.exe`;
}

/**
 * Build-output convention (§10, extended by VS CODE WINDOWS M4 spec §8):
 * `/project/src/hello.kai` -> `/project/src/hello` on Linux,
 * `C:\project\src\hello.kai` -> `C:\project\src\hello.exe` on Windows -
 * the source file's own directory, same basename with the `.kai`
 * extension stripped, with the platform's native executable suffix
 * applied via the SAME rule kaicc itself uses. This can never equal the
 * source path itself, so no separate overwrite-the-source guard is
 * needed. Replacing/rebuilding an existing output at that path is
 * accepted default behavior - kaicc itself already handles overwriting
 * its own previous output (M7).
 *
 * `pathImpl` is injectable (defaults to the real, platform-native `path`
 * module) purely for testability: production code always wants the
 * REAL native `path` module (which already resolves to win32 semantics
 * when actually running on Windows, and posix semantics on Linux - never
 * hardcode `path.win32` here), but a unit test running on Linux CI needs
 * to pass `path.win32` explicitly to exercise Windows-style path
 * splitting (backslashes, drive letters, spaces) without a real Windows
 * machine.
 */
export function computeOutputPath(
    sourcePath: string,
    platform: NodeJS.Platform,
    pathImpl: nodePath.PlatformPath = nodePath,
): string {
    const dir = pathImpl.dirname(sourcePath);
    const base = pathImpl.basename(sourcePath, '.kai');
    const requested = pathImpl.join(dir, base);
    return applyNativeExecutableSuffix(requested, platform);
}
