// Activation entry point (VS CODE COMPILER INTEGRATION MILESTONE 2 spec
// §2, extended by VS CODE MILESTONE 3 §5 and VS CODE MILESTONE 4): wiring
// only - creates the shared "KAI" OutputChannel, registers commands (see
// commands.ts), registers KAI Basic IntelliSense completions (see
// completions.ts), and registers the KAI File Icons command (see
// iconThemeCommand.ts - no automatic activation prompt: switching icon
// themes is a global, disruptive change this extension never proposes on
// its own). No compiler/process/path/completion/icon-theme logic lives
// here.

import * as vscode from 'vscode';

import { registerCommands } from './commands';
import { registerCompletions } from './completions';
import { registerUseKaiFileIconsCommand } from './iconThemeCommand';

export function activate(context: vscode.ExtensionContext): void {
    const output = vscode.window.createOutputChannel('KAI');
    context.subscriptions.push(output);
    context.subscriptions.push(...registerCommands(context, output));
    context.subscriptions.push(registerCompletions());
    context.subscriptions.push(registerUseKaiFileIconsCommand());
}

export function deactivate(): void {
    // No global state to tear down - the OutputChannel/commands are
    // disposed automatically via context.subscriptions.
}
