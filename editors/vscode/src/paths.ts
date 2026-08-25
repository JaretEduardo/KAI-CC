// Pure, vscode-API-free path/policy logic (VS CODE COMPILER INTEGRATION
// MILESTONE 2 spec §2/§27): kept separate from compiler.ts so it can be
// unit-tested with plain Node, without needing the `vscode` module to be
// resolvable (that module only exists inside a running VS Code extension
// host - importing it directly in a standalone test process throws).

import * as fs from 'fs';
import * as path from 'path';

export type CompilerLocateResult = { ok: true; kaiccPath: string } | { ok: false; message: string };

/**
 * Resolves which `kaicc` executable to invoke, per the spec's fixed
 * priority order (§8):
 *
 *   1. an explicitly configured path (`kai.compilerPath`), if non-empty -
 *      an invalid configured path is a hard, reported error, never a
 *      silent fallback to the bundled compiler (deterministic packaging
 *      behavior, §8).
 *   2. the compiler bundled with the extension, at
 *      `<extensionPath>/bin/kaicc` - ONLY on Linux x64, the sole
 *      currently-supported platform (§19). No PATH search ever happens
 *      here (§8).
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

    if (platform !== 'linux' || arch !== 'x64') {
        return {
            ok: false,
            message:
                'Bundled KAI compiler is currently available only for Linux x64. ' +
                'Set "kai.compilerPath" to use a compiler built for this platform.',
        };
    }

    const bundledPath = path.join(extensionPath, 'bin', 'kaicc');
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
 * Build-output convention (§10): `/project/src/hello.kai` ->
 * `/project/src/hello` - the source file's own directory, same basename
 * with the `.kai` extension stripped. This can never equal the source
 * path itself (the stripped extension always makes the two differ), so
 * no separate overwrite-the-source guard is needed. Replacing/rebuilding
 * an existing output at that path is accepted default behavior - kaicc
 * itself already handles overwriting its own previous output (M7).
 */
export function computeOutputPath(sourcePath: string): string {
    const dir = path.dirname(sourcePath);
    const base = path.basename(sourcePath, '.kai');
    return path.join(dir, base);
}
