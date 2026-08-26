// Plain-Node unit tests for completionData.ts (no VS Code API needed -
// mirrors paths.test.ts/process.test.ts's own convention).

import * as assert from 'assert';

import { BUILTIN_FUNCTIONS, isTypeAnnotationContext, KEYWORDS, PRIMITIVE_TYPES } from '../completionData';

const EXPECTED_KEYWORDS = [
    'fn',
    'let',
    'mut',
    'return',
    'if',
    'else',
    'while',
    'for',
    'in',
    'struct',
    'enum',
    'use',
    'pub',
    'as',
    'true',
    'false',
];

const EXPECTED_PRIMITIVE_TYPES = [
    'i8',
    'i16',
    'i32',
    'i64',
    'u8',
    'u16',
    'u32',
    'u64',
    'f32',
    'f64',
    'bool',
    'char',
    'str',
];

function labelsOf(entries: readonly { label: string }[]): string[] {
    return entries.map((entry) => entry.label);
}

function testKeywordSetMatchesCurrentLexerExactly(): void {
    const labels = labelsOf(KEYWORDS);
    assert.deepStrictEqual([...labels].sort(), [...EXPECTED_KEYWORDS].sort());
    // Every keyword needs a real, non-empty beginner-facing detail.
    for (const entry of KEYWORDS) {
        assert.ok(entry.detail.length > 0, `keyword "${entry.label}" is missing a detail`);
    }
}

function testPrimitiveTypeSetMatchesCurrentTypeKindExactly(): void {
    const labels = labelsOf(PRIMITIVE_TYPES);
    assert.deepStrictEqual([...labels].sort(), [...EXPECTED_PRIMITIVE_TYPES].sort());
}

function testStrIsExposedAsAPrimitiveType(): void {
    // Spellable str + Parameters/Returns MVP: `str` is a spellable
    // source-level type annotation with a real TypeKind::Str backend
    // representation now (see completionData.ts's own header comment) -
    // it must appear here like any other primitive.
    assert.ok(labelsOf(PRIMITIVE_TYPES).includes('str'));
}

function testInternalOnlyTypeKindsAreNeverExposed(): void {
    for (const internalOnly of ['Unresolved', 'Error', 'Unit', 'unresolved', 'error', 'unit']) {
        assert.ok(!labelsOf(PRIMITIVE_TYPES).includes(internalOnly));
    }
}

function testOnlyPrintIsOfferedAsABuiltin(): void {
    const labels = labelsOf(BUILTIN_FUNCTIONS);
    assert.deepStrictEqual(labels, ['print']);
    assert.ok(!labels.includes('panic'), 'panic has no native lowering yet and must not be advertised');
    assert.ok(!labels.includes('assert'), 'assert has no native lowering yet and must not be advertised');
}

function testPrintInsertsAUsefulSnippet(): void {
    const print = BUILTIN_FUNCTIONS.find((entry) => entry.label === 'print');
    assert.ok(print);
    assert.strictEqual(print?.insertText, 'print(${1:value})');
}

function testTypeAnnotationContextDetectsColonAfterLetBinding(): void {
    assert.strictEqual(isTypeAnnotationContext('let x:'), true);
    assert.strictEqual(isTypeAnnotationContext('let x: '), true);
    assert.strictEqual(isTypeAnnotationContext('    let age:  '), true);
}

function testTypeAnnotationContextDetectsColonInParameterList(): void {
    assert.strictEqual(isTypeAnnotationContext('fn f(x:'), true);
    assert.strictEqual(isTypeAnnotationContext('fn f(x: '), true);
}

function testTypeAnnotationContextDetectsArrowReturnType(): void {
    assert.strictEqual(isTypeAnnotationContext('fn f() ->'), true);
    assert.strictEqual(isTypeAnnotationContext('fn f() -> '), true);
}

function testOrdinaryExpressionContextIsNotTypeOnly(): void {
    assert.strictEqual(isTypeAnnotationContext('let x = 1 + '), false);
    assert.strictEqual(isTypeAnnotationContext('if x '), false);
    assert.strictEqual(isTypeAnnotationContext('print('), false);
    assert.strictEqual(isTypeAnnotationContext('return '), false);
    assert.strictEqual(isTypeAnnotationContext(''), false);
}

function main(): void {
    testKeywordSetMatchesCurrentLexerExactly();
    testPrimitiveTypeSetMatchesCurrentTypeKindExactly();
    testStrIsExposedAsAPrimitiveType();
    testInternalOnlyTypeKindsAreNeverExposed();
    testOnlyPrintIsOfferedAsABuiltin();
    testPrintInsertsAUsefulSnippet();
    testTypeAnnotationContextDetectsColonAfterLetBinding();
    testTypeAnnotationContextDetectsColonInParameterList();
    testTypeAnnotationContextDetectsArrowReturnType();
    testOrdinaryExpressionContextIsNotTypeOnly();

    console.log('completionData.test.ts: all tests passed');
}

main();
