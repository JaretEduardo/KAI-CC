// VS CODE MILESTONE 4: "KAI: Use KAI File Icons" command. Thin
// vscode-facing glue over the KAI_ICON_THEME_ID constant in iconTheme.ts -
// mirrors compiler.ts's own split over paths.ts/process.ts.
//
// Deliberately NO automatic activation prompt: KAI File Icons is a
// narrow theme (see fileicons/kai-icon-theme.json's own header comment),
// and switching to it replaces whatever richer icon theme the user
// already has (e.g. Material Icon Theme), falling every non-KAI file/
// folder back to a plain generic icon. That is a global, disruptive
// change this extension should never propose on its own - the user must
// explicitly run this command if they want the bundled theme.

import * as vscode from 'vscode';

import { KAI_ICON_THEME_ID } from './iconTheme';

/** Directly switches the ACTIVE workbench icon theme to KAI File Icons - deterministic, no picker UI. */
async function useKaiFileIcons(): Promise<void> {
    await vscode.workspace
        .getConfiguration()
        .update('workbench.iconTheme', KAI_ICON_THEME_ID, vscode.ConfigurationTarget.Global);
    vscode.window.showInformationMessage('KAI: switched to the KAI File Icons theme.');
}

/** Registers the "KAI: Use KAI File Icons" command (id: kai.useKaiFileIcons). */
export function registerUseKaiFileIconsCommand(): vscode.Disposable {
    return vscode.commands.registerCommand('kai.useKaiFileIcons', () => useKaiFileIcons());
}
