// VS CODE MILESTONE 4: shared constant for the bundled KAI File Icons
// theme, kept in its own tiny vscode-API-free module (the same pattern
// paths.ts/process.ts/completionData.ts already established) so it stays
// directly unit-testable and reusable from iconThemeCommand.ts.

/** Must match contributes.iconThemes[].id in package.json exactly. */
export const KAI_ICON_THEME_ID = 'kai-file-icons';
