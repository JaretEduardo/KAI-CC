// Activation entry point (VS CODE COMPILER INTEGRATION MILESTONE 2 spec
// §2, extended by VS CODE MILESTONE 3 §5): wiring only - creates the
// shared "KAI" OutputChannel, registers commands (see commands.ts), and
// registers KAI Basic IntelliSense completions (see completions.ts). No
// compiler/process/path/completion logic lives here.

import * as vscode from 'vscode';

import { registerCommands } from './commands';
import { registerCompletions } from './completions';

export function activate(context: vscode.ExtensionContext): void {
    const output = vscode.window.createOutputChannel('KAI');
    context.subscriptions.push(output);
    context.subscriptions.push(...registerCommands(context, output));
    context.subscriptions.push(registerCompletions());
}

export function deactivate(): void {
    // No global state to tear down - the OutputChannel/commands are
    // disposed automatically via context.subscriptions.
}
