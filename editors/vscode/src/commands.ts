// KAI: Build Current File / KAI: Run Current File (VS CODE COMPILER
// INTEGRATION MILESTONE 2 spec §4): user-facing command behavior -
// active-file validation, save-before-compile, invoking compiler.ts, and
// rendering results into the "KAI" OutputChannel / VS Code notifications.
// Owns no child-process/path-resolution logic of its own - that lives in
// compiler.ts/paths.ts.

import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import { compileFile, locateCompiler } from './compiler';
import { computeOutputPath } from './paths';
import { ProcessResult, spawnProcess } from './process';

interface ActiveKaiFile {
    sourcePath: string;
}

/**
 * Validates the active editor per spec §5/§6: an editor must be active,
 * its document's languageId must be "kai", and it must be a real
 * filesystem-backed document (untitled/unsaved buffers are out of scope
 * for this milestone). A dirty document is saved before compiling, since
 * kaicc only ever reads what is actually on disk - we must compile the
 * source the user is really looking at. Shows a concise error message
 * and returns undefined for every failure case; never invokes kaicc on
 * an invalid/unsaved document.
 */
async function getActiveKaiFile(): Promise<ActiveKaiFile | undefined> {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        vscode.window.showErrorMessage('KAI: no active editor.');
        return undefined;
    }

    const document = editor.document;
    if (document.languageId !== 'kai') {
        vscode.window.showErrorMessage('KAI: the active file is not a KAI (.kai) file.');
        return undefined;
    }

    if (document.uri.scheme !== 'file') {
        vscode.window.showErrorMessage('KAI: the active document must be saved to disk before compiling.');
        return undefined;
    }

    if (document.isDirty) {
        const saved = await document.save();
        if (!saved) {
            vscode.window.showErrorMessage('KAI: failed to save the current file before compiling.');
            return undefined;
        }
    }

    return { sourcePath: document.uri.fsPath };
}

function appendProcessOutput(output: vscode.OutputChannel, result: ProcessResult): void {
    if (result.stdout) {
        output.append(result.stdout);
    }
    if (result.stderr) {
        output.append(result.stderr);
    }
}

export interface BuildSuccess {
    sourcePath: string;
    outputPath: string;
}

/**
 * KAI: Build Current File. Returns the produced executable's path on
 * success, undefined on any failure (having already reported it) - this
 * lets runCurrentFile() reuse this exact function rather than
 * duplicating the build implementation (spec §14).
 */
export async function buildCurrentFile(
    context: vscode.ExtensionContext,
    output: vscode.OutputChannel,
): Promise<BuildSuccess | undefined> {
    const active = await getActiveKaiFile();
    if (!active) {
        return undefined;
    }

    const located = locateCompiler(context);
    if (!located.ok) {
        output.appendLine(located.message);
        output.show(true);
        vscode.window.showErrorMessage(`KAI: ${located.message}`);
        return undefined;
    }

    const outputPath = computeOutputPath(active.sourcePath);

    output.clear();
    output.appendLine(`$ ${located.kaiccPath} ${active.sourcePath} -o ${outputPath}`);

    const result = await compileFile(located.kaiccPath, active.sourcePath, outputPath);
    appendProcessOutput(output, result);

    if (result.failedToStart) {
        output.appendLine(`Failed to start the compiler: ${result.startError}`);
        output.show(true);
        vscode.window.showErrorMessage(`KAI: failed to start the compiler (${result.startError}).`);
        return undefined;
    }

    if (result.exitCode !== 0) {
        output.appendLine(`kaicc exited with code ${result.exitCode}`);
        output.show(true);
        vscode.window.showErrorMessage(`KAI: build failed (exit code ${result.exitCode}). See the "KAI" output channel.`);
        return undefined;
    }

    if (!fs.existsSync(outputPath)) {
        output.appendLine(`kaicc reported success but no output executable was found at ${outputPath}`);
        output.show(true);
        vscode.window.showErrorMessage('KAI: build reported success but no output executable was produced.');
        return undefined;
    }

    output.appendLine(`Build succeeded: ${outputPath}`);
    vscode.window.showInformationMessage(`KAI: build succeeded (${path.basename(outputPath)}).`);
    return { sourcePath: active.sourcePath, outputPath };
}

/**
 * KAI: Run Current File (spec §14): build via the SAME buildCurrentFile()
 * used by "KAI: Build Current File", abort if it failed (it already
 * reported why), then launch the produced executable shell-free (§15),
 * with its working directory set to the source file's own directory.
 */
export async function runCurrentFile(context: vscode.ExtensionContext, output: vscode.OutputChannel): Promise<void> {
    const built = await buildCurrentFile(context, output);
    if (!built) {
        return;
    }

    output.appendLine(`$ ${built.outputPath}`);
    const result = await spawnProcess(built.outputPath, [], path.dirname(built.sourcePath));
    appendProcessOutput(output, result);

    if (result.failedToStart) {
        output.appendLine(`Failed to launch the program: ${result.startError}`);
        output.show(true);
        vscode.window.showErrorMessage(`KAI: failed to launch the program (${result.startError}).`);
        return;
    }

    output.appendLine(`Process exited with code ${result.exitCode}`);
    if (result.exitCode !== 0) {
        output.show(true);
        vscode.window.showWarningMessage(`KAI: program exited with code ${result.exitCode}. See the "KAI" output channel.`);
        return;
    }

    vscode.window.showInformationMessage('KAI: run finished (exit code 0).');
}

/** Registers both commands; returns their disposables for context.subscriptions. */
export function registerCommands(context: vscode.ExtensionContext, output: vscode.OutputChannel): vscode.Disposable[] {
    return [
        vscode.commands.registerCommand('kai.buildCurrentFile', () => buildCurrentFile(context, output)),
        vscode.commands.registerCommand('kai.runCurrentFile', () => runCurrentFile(context, output)),
    ];
}
