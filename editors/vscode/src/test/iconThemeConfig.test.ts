// Plain-Node static-consistency checks over package.json's icon/
// iconThemes wiring and fileicons/kai-icon-theme.json itself (no VS Code
// API needed - just JSON + filesystem checks, mirroring snippets.test.ts's
// own convention from Milestone 3). Catches drift/typos between the
// extension manifest, the icon theme definition, and the actual asset
// files on disk.

import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';

import { KAI_ICON_THEME_ID } from '../iconTheme';

const EXTENSION_ROOT = path.join(__dirname, '..', '..');

interface PackageJson {
    icon?: string;
    contributes?: {
        iconThemes?: Array<{ id: string; label: string; path: string }>;
        commands?: Array<{ command: string; title: string; category?: string }>;
    };
}

interface IconThemeJson {
    iconDefinitions: Record<string, { iconPath: string }>;
    file?: string;
    folder?: string;
    folderExpanded?: string;
    fileExtensions?: Record<string, string>;
    languageIds?: Record<string, string>;
}

function loadPackageJson(): PackageJson {
    return JSON.parse(fs.readFileSync(path.join(EXTENSION_ROOT, 'package.json'), 'utf8')) as PackageJson;
}

function testExtensionIconFieldPointsToARealPngFile(): void {
    const pkg = loadPackageJson();
    assert.ok(pkg.icon, 'package.json must set "icon"');
    assert.match(pkg.icon as string, /\.png$/, 'the extension "icon" field must be a PNG (VS Code requirement)');

    const iconPath = path.join(EXTENSION_ROOT, pkg.icon as string);
    assert.ok(fs.existsSync(iconPath), `extension icon not found at ${iconPath}`);
}

function testIconThemeContributionMatchesTheStableId(): void {
    const pkg = loadPackageJson();
    const themes = pkg.contributes?.iconThemes ?? [];
    const kaiTheme = themes.find((theme) => theme.id === KAI_ICON_THEME_ID);
    assert.ok(kaiTheme, `contributes.iconThemes must include an entry with id "${KAI_ICON_THEME_ID}"`);

    const themePath = path.join(EXTENSION_ROOT, kaiTheme!.path);
    assert.ok(fs.existsSync(themePath), `icon theme file not found at ${themePath}`);
}

function testUseKaiFileIconsCommandIsContributed(): void {
    const pkg = loadPackageJson();
    const commands = pkg.contributes?.commands ?? [];
    assert.ok(commands.some((cmd) => cmd.command === 'kai.useKaiFileIcons'), 'kai.useKaiFileIcons must be contributed');
}

function loadIconThemeJson(): { themeDir: string; theme: IconThemeJson } {
    const pkg = loadPackageJson();
    const kaiTheme = (pkg.contributes?.iconThemes ?? []).find((theme) => theme.id === KAI_ICON_THEME_ID)!;
    const themeFilePath = path.join(EXTENSION_ROOT, kaiTheme.path);
    const theme = JSON.parse(fs.readFileSync(themeFilePath, 'utf8')) as IconThemeJson;
    return { themeDir: path.dirname(themeFilePath), theme };
}

function testKaiFileExtensionAndLanguageIdMapToTheKaiIconDefinition(): void {
    const { theme } = loadIconThemeJson();
    assert.strictEqual(theme.fileExtensions?.kai, '_kaiFile');
    assert.strictEqual(theme.languageIds?.kai, '_kaiFile');
    assert.ok(theme.iconDefinitions['_kaiFile'], 'iconDefinitions must define "_kaiFile"');
}

function testNarrowThemeStillHasFileAndFolderFallbacks(): void {
    // §Narrow scope: this theme only NEEDS to provide the KAI file icon,
    // but omitting file/folder defaults entirely would leave every
    // non-.kai file/folder icon-less while this theme is active - a
    // small fallback keeps the theme usable without overbuilding it.
    const { theme } = loadIconThemeJson();
    assert.ok(theme.file && theme.iconDefinitions[theme.file], 'a default "file" icon definition must exist');
    assert.ok(theme.folder && theme.iconDefinitions[theme.folder], 'a default "folder" icon definition must exist');
    assert.ok(
        theme.folderExpanded && theme.iconDefinitions[theme.folderExpanded],
        'a default "folderExpanded" icon definition must exist',
    );
}

function testEveryReferencedIconAssetActuallyExistsOnDisk(): void {
    const { themeDir, theme } = loadIconThemeJson();
    for (const [id, definition] of Object.entries(theme.iconDefinitions)) {
        const assetPath = path.join(themeDir, definition.iconPath);
        assert.ok(fs.existsSync(assetPath), `icon definition "${id}" references a missing file: ${assetPath}`);
    }
}

function main(): void {
    testExtensionIconFieldPointsToARealPngFile();
    testIconThemeContributionMatchesTheStableId();
    testUseKaiFileIconsCommandIsContributed();
    testKaiFileExtensionAndLanguageIdMapToTheKaiIconDefinition();
    testNarrowThemeStillHasFileAndFolderFallbacks();
    testEveryReferencedIconAssetActuallyExistsOnDisk();

    console.log('iconThemeConfig.test.ts: all tests passed');
}

main();
