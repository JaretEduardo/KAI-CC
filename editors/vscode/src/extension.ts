// Activation entry point (VS CODE COMPILER INTEGRATION MILESTONE 2 spec
// §2): wiring only - creates the shared "KAI" OutputChannel and registers
// commands (see commands.ts). No compiler/process/path logic lives here.

import * as vscode from 'vscode';

import { registerCommands } from './commands';

export function activate(context: vscode.ExtensionContext): void {
    const output = vscode.window.createOutputChannel('KAI');
    context.subscriptions.push(output);
    context.subscriptions.push(...registerCommands(context, output));
}

export function deactivate(): void {
    // No global state to tear down - the OutputChannel/commands are
    // disposed automatically via context.subscriptions.
}
