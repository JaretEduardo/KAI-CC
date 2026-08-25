// Locates the real kaicc compiler and invokes it (VS CODE COMPILER
// INTEGRATION MILESTONE 2 spec §2). Thin vscode-facing glue only - the
// actual path-resolution policy lives in paths.ts (vscode-free, unit-
// tested) and the actual child-process mechanics live in process.ts
// (also vscode-free, unit-tested); this module just wires
// vscode.ExtensionContext/workspace configuration into that pure logic.
//
// Runtime-library discovery is NOT this module's job: kaicc's own
// NativeLinker already finds `libkai_runtime.a` relative to whichever
// kaicc binary is actually running (see compiler/include/kai/codegen/
// NativeLinker.hpp) - this module only needs to find the right kaicc
// EXECUTABLE (see resolveCompilerPath() in paths.ts); the runtime lookup
// then "just works" as long as the packaged/staged layout keeps `bin/`
// and `lib/kai/` as siblings (see scripts/stage-compiler.mjs).

import * as vscode from 'vscode';

import { CompilerLocateResult, resolveCompilerPath } from './paths';
import { ProcessResult, spawnProcess } from './process';

export function locateCompiler(context: vscode.ExtensionContext): CompilerLocateResult {
    const configuredPath = vscode.workspace.getConfiguration('kai').get<string>('compilerPath');
    return resolveCompilerPath(context.extensionPath, configuredPath, process.platform, process.arch);
}

/** Invokes `kaiccPath sourcePath -o outputPath` - the exact M7 CLI contract. */
export function compileFile(kaiccPath: string, sourcePath: string, outputPath: string): Promise<ProcessResult> {
    return spawnProcess(kaiccPath, [sourcePath, '-o', outputPath]);
}
