// VS CODE MILESTONE 3: KAI Basic IntelliSense - builds actual
// vscode.CompletionItem objects from the pure metadata/context logic in
// completionData.ts, and registers the provider. Thin vscode-facing glue
// only, mirroring compiler.ts's own split over paths.ts/process.ts.

import * as vscode from 'vscode';

import {
    BUILTIN_FUNCTIONS,
    BuiltinCompletionMetadata,
    CompletionMetadata,
    isTypeAnnotationContext,
    KEYWORDS,
    PRIMITIVE_TYPES,
} from './completionData';

function makeKeywordItem(entry: CompletionMetadata, promoted: boolean): vscode.CompletionItem {
    const item = new vscode.CompletionItem(entry.label, vscode.CompletionItemKind.Keyword);
    item.detail = entry.detail;
    if (promoted) {
        // Rank below promoted types (see makeTypeItem) rather than
        // interleaving alphabetically with them right after `:`/`->`.
        item.sortText = `1_${entry.label}`;
    }
    return item;
}

function makeTypeItem(entry: CompletionMetadata, promoted: boolean): vscode.CompletionItem {
    const item = new vscode.CompletionItem(entry.label, vscode.CompletionItemKind.Class);
    item.detail = entry.detail;
    if (promoted) {
        item.sortText = `0_${entry.label}`;
    }
    return item;
}

function makeBuiltinItem(entry: BuiltinCompletionMetadata): vscode.CompletionItem {
    const item = new vscode.CompletionItem(entry.label, vscode.CompletionItemKind.Function);
    item.detail = entry.detail;
    item.insertText = new vscode.SnippetString(entry.insertText);
    return item;
}

/** Builds the full completion list; `promoteTypes` ranks primitive types above keywords (see isTypeAnnotationContext). */
export function buildCompletionItems(promoteTypes: boolean): vscode.CompletionItem[] {
    return [
        ...PRIMITIVE_TYPES.map((entry) => makeTypeItem(entry, promoteTypes)),
        ...KEYWORDS.map((entry) => makeKeywordItem(entry, promoteTypes)),
        ...BUILTIN_FUNCTIONS.map((entry) => makeBuiltinItem(entry)),
    ];
}

/** Registers the KAI completion provider for `.kai` documents (§3: ordinary extension API, no LSP). */
export function registerCompletions(): vscode.Disposable {
    return vscode.languages.registerCompletionItemProvider(
        { language: 'kai' },
        {
            provideCompletionItems(document: vscode.TextDocument, position: vscode.Position) {
                const linePrefix = document.lineAt(position.line).text.substring(0, position.character);
                return buildCompletionItems(isTypeAnnotationContext(linePrefix));
            },
        },
        ':', // trigger character (§14): pop suggestions right after a type-annotation colon
    );
}
