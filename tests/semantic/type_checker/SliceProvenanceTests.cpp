// KAI LANGUAGE M11A: TypeChecker coverage for restricted Slice PROVENANCE
// (External/Local/Unknown) and the safe-return rule it enables
// (SemanticErrorKind::EscapingLocalSlice). Provenance itself is
// deliberately compiler-internal state (SemanticModel::sliceProvenanceOf()/
// setSliceProvenance()/... are TypeChecker-friend-only private methods -
// see SemanticModel.hpp's own doc comment on SliceProvenance) with no
// public query surface, so every test here observes it the only way a
// caller ever can: by returning a Slice-typed expression from a function
// declared to return Slice, and checking whether that specific return is
// accepted (zero errors) or rejected with EscapingLocalSlice - never a
// generic TypeMismatch. `slice(...)`/`len(...)` call-shape/argument
// validation itself is covered separately in SliceBuiltinTests.cpp; Slice
// TYPE resolution in ../SliceTypeTests.cpp; this file is provenance/
// return-safety only.

#include "semantic/type_checker/TypeCheckerTestSupport.hpp"

using namespace kai::test::type_checker;

namespace {

void expectAcceptedReturn(const Checked& result) {
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().empty());
}

void expectEscapingLocalSlice(const Checked& result) {
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::EscapingLocalSlice);
    }
}

// --- Simple valid returns (spec: direct/copied/double-copied Slice-
// parameter forwarding - a Slice PARAMETER is External, and provenance
// follows a plain copy/rebinding exactly). ---

// A. Direct forwarding: `return xs` where `xs` is itself the Slice
// parameter.
void testDirectParameterForwardingIsSafeReturn() {
    SourceManager sm;
    expectAcceptedReturn(analyzeAndCheck(sm, "fn f(xs: [i32]) -> [i32] {\n    return xs\n}"));
}

// B. Copied once: `let s = xs; return s`.
void testSingleCopyOfParameterIsSafeReturn() {
    SourceManager sm;
    expectAcceptedReturn(
        analyzeAndCheck(sm, "fn f(xs: [i32]) -> [i32] {\n    let s = xs\n    return s\n}"));
}

// C. Copied twice: `let s = xs; let t = s; return t` - provenance
// propagates through an arbitrary chain of plain copies, not just one hop.
void testDoubleCopyOfParameterIsSafeReturn() {
    SourceManager sm;
    expectAcceptedReturn(analyzeAndCheck(
        sm, "fn f(xs: [i32]) -> [i32] {\n    let s = xs\n    let t = s\n    return t\n}"));
}

// --- Invalid returns (spec A-E). ---

// A. `return slice(values)` for a LOCAL fixed array - Local, not External.
void testLocalArraySliceDirectReturnRejected() {
    SourceManager sm;
    expectEscapingLocalSlice(
        analyzeAndCheck(sm, "fn bad() -> [i32] {\n    let values = [1, 2, 3]\n    return slice(values)\n}"));
}

// B. Same, but via an intermediate local copy - provenance follows the
// copy, so `s` is Local too.
void testLocalArraySliceViaIntermediateCopyReturnRejected() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(
        sm,
        "fn bad() -> [i32] {\n    let values = [1, 2, 3]\n    let s = slice(values)\n    return s\n}"));
}

// C. `slice(fixedArrayParameter)` returned - KAI LANGUAGE M8's fixed-array
// parameters are passed BY VALUE (copied into callee-owned storage), so
// slicing one is exactly as Local as slicing a local array - this must
// NOT be treated like a Slice parameter (which is External).
void testFixedArrayParameterSliceReturnRejected() {
    SourceManager sm;
    expectEscapingLocalSlice(
        analyzeAndCheck(sm, "fn bad(values: [i32; 3]) -> [i32] {\n    return slice(values)\n}"));
}

// D. An External binding reassigned to a Local value before it is
// returned - reassignment through straight-line code must update
// provenance sequentially, not keep remembering the binding's initial
// (now stale) provenance.
void testExternalReassignedToLocalThenReturnedRejected() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(xs: [i32]) -> [i32] {\n"
                                              "    let values = [1, 2, 3]\n"
                                              "    mut s = xs\n"
                                              "    s = slice(values)\n"
                                              "    return s\n"
                                              "}"));
}

// E. Unknown after a branch merge - an if with no `else` reassigns `s` to
// Local in the taken branch, but "no branch taken" (a real possible
// outcome, since there's no `else`) leaves `s` at its entry provenance
// (External) - a mixed merge result is Unknown, and Unknown is rejected
// exactly like Local (never returnable, only External is).
void testUnknownAfterBranchMergeReturnRejected() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(xs: [i32], cond: bool) -> [i32] {\n"
                                              "    let values = [1, 2, 3]\n"
                                              "    mut s = xs\n"
                                              "    if cond {\n"
                                              "        s = slice(values)\n"
                                              "    }\n"
                                              "    return s\n"
                                              "}"));
}

// F. An arbitrary Slice-returning function CALL is always Unknown - M11A
// does no interprocedural inference, even though `helper` itself only
// ever safely forwards its own External parameter.
void testArbitraryFunctionCallResultIsUnknown() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn helper(xs: [i32]) -> [i32] {\n"
                                              "    return xs\n"
                                              "}\n"
                                              "fn bad(xs: [i32]) -> [i32] {\n"
                                              "    return helper(xs)\n"
                                              "}"));
}

// G. KAI LANGUAGE M11B spec §9: `Unknown` means "cannot prove safe to
// ESCAPE", never "invalid value" - an ordinary LOCAL, non-escaping use of
// an Unknown-provenance Slice-returning call result (reading an element,
// never returning the value itself) must remain perfectly valid.
void testLocalUseOfUnknownCallResultIsValid() {
    SourceManager sm;
    expectAcceptedReturn(analyzeAndCheck(sm,
                                          "fn helper(xs: [i32]) -> [i32] {\n"
                                          "    return xs\n"
                                          "}\n"
                                          "fn firstOf(xs: [i32]) -> i32 {\n"
                                          "    let s = helper(xs)\n"
                                          "    return s[0]\n"
                                          "}"));
}

// --- Control-flow soundness scenarios (must not be overfit to the
// examples above). ---

// A. if/else merge to Unknown: one branch reassigns to Local, the other
// leaves the binding External - a genuinely mixed merge, not merely a
// "some branch touched it" rule (that coarser rule is loop-only, see scenario
// C below).
void testIfElseMixedBranchesMergeToUnknownRejectsReturn() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(xs: [i32], ys: [i32], cond: bool) -> [i32] {\n"
                                              "    let values = [1, 2, 3]\n"
                                              "    mut s = xs\n"
                                              "    if cond {\n"
                                              "        s = slice(values)\n"
                                              "    } else {\n"
                                              "        s = ys\n"
                                              "    }\n"
                                              "    return s\n"
                                              "}"));
}

// B. if/else merge stays External: both branches (and, since there's no
// `else` here, the untaken-branch outcome too) leave the binding External
// - the merge must not spuriously degrade to Unknown just because a
// branch exists at all.
void testIfElseAllExternalBranchesStayExternalAllowsReturn() {
    SourceManager sm;
    expectAcceptedReturn(analyzeAndCheck(sm,
                                          "fn f(xs: [i32], ys: [i32], cond: bool) -> [i32] {\n"
                                          "    mut s = xs\n"
                                          "    if cond {\n"
                                          "        s = ys\n"
                                          "    }\n"
                                          "    return s\n"
                                          "}"));
}

// C. A while loop forces Unknown even though the loop body might run zero
// times at runtime - soundness requires this regardless of the trip
// count, which TypeChecker cannot know statically.
void testWhileLoopTouchingBindingForcesUnknownRejectsReturn() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(xs: [i32], cond: bool) -> [i32] {\n"
                                              "    let values = [1, 2, 3]\n"
                                              "    mut s = xs\n"
                                              "    while cond {\n"
                                              "        s = slice(values)\n"
                                              "    }\n"
                                              "    return s\n"
                                              "}"));
}

// D. An if/else where BOTH branches assign a Local source stays Local
// (External+External -> External and Local+Local -> Local are symmetric
// entries in the same merge table) - still correctly rejected (Local is
// never returnable either), but for the right reason: this exercises the
// table's Local+Local entry specifically, not merely "any reassignment in
// a branch means Unknown".
void testIfElseBothBranchesLocalStaysLocalRejectsReturn() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(cond: bool) -> [i32] {\n"
                                              "    let a = [1, 2, 3]\n"
                                              "    let b = [4, 5, 6]\n"
                                              "    mut s = slice(a)\n"
                                              "    if cond {\n"
                                              "        s = slice(b)\n"
                                              "    } else {\n"
                                              "        s = slice(a)\n"
                                              "    }\n"
                                              "    return s\n"
                                              "}"));
}

// --- M11A FINAL SAFETY REGRESSION CLEANUP ---
//
// A very small, targeted set of additional tests locking in the
// flow-sensitive provenance STATE itself (snapshot/restore, touch-
// tracking, per-function isolation) rather than re-testing the merge
// TABLE or the return rule again - those are already covered above.
// Note: the "if without else, External+Local -> Unknown" case this
// cleanup pass calls for is already exactly
// testUnknownAfterBranchMergeReturnRejected() above (same fixture shape:
// a Slice parameter reassigned to `slice(...)` of a local array inside a
// single `if` with no `else`) - not duplicated here.

// A `for` loop must be JUST as conservative as a `while` loop -
// touch-tracking (checkForStmt) is a SEPARATE call site from
// checkWhileStmt's own, so this proves the same rule was actually wired
// up there too, not merely designed once and copy-pasted incompletely.
// Deliberately does NOT special-case the trivially-constant `0..1` trip
// count - M11A's rule is "any binding touched anywhere in the loop body"
// regardless of iteration count, by design (TypeChecker cannot prove
// trip counts as a general rule, and M11A does not special-case the
// cases where it happens to be able to).
void testForLoopTouchingBindingForcesUnknownRejectsReturn() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(xs: [i32]) -> [i32] {\n"
                                              "    let local = [1, 2, 3]\n"
                                              "    mut s = xs\n"
                                              "    for i in 0..1 {\n"
                                              "        s = slice(local)\n"
                                              "    }\n"
                                              "    return s\n"
                                              "}"));
}

// Provenance flow state lives inside the compilation-wide SemanticModel
// (see this file's own top-of-file comment), so this proves one
// function's own flow-sensitive mutations (`bad`'s `s` becoming Local)
// can never bleed into an UNRELATED function's own, differently-scoped
// binding of the exact same NAME (`good`'s own `s`) - each declared `s`
// is a genuinely distinct SymbolId, and provenance is keyed by SymbolId,
// never by name. `bad` is checked first specifically so any accidental
// leftover state would have a chance to poison `good`, checked right
// after it in the same SemanticModel/compilation.
void testProvenanceDoesNotLeakAcrossFunctions() {
    SourceManager sm;
    Checked result = analyzeAndCheck(sm,
                                      "fn bad(xs: [i32]) -> [i32] {\n"
                                      "    let local = [1, 2, 3]\n"
                                      "    mut s = xs\n"
                                      "    s = slice(local)\n"
                                      "    return s\n"
                                      "}\n"
                                      "fn good(xs: [i32]) -> [i32] {\n"
                                      "    let s = xs\n"
                                      "    return s\n"
                                      "}");
    KAI_CHECK(result.parsed.has_value());
    if (!result.parsed) {
        return;
    }
    // Exactly ONE error total: `bad`'s own EscapingLocalSlice. If `good`'s
    // distinct `s` were somehow contaminated by `bad`'s `s` (e.g. a bug
    // keying provenance by name instead of SymbolId, or failing to seed
    // `good`'s own parameter as External), `good` would ALSO fail here,
    // pushing this past one error.
    KAI_CHECK(result.model.errors().size() == 1);
    if (result.model.errors().size() == 1) {
        KAI_CHECK(result.model.errors()[0].kind == SemanticErrorKind::EscapingLocalSlice);
    }
}

// Optional nested-control-flow case: an External binding that only ever
// becomes Local two `if` levels deep must still fail to merge back to
// External at the OUTER level - proving the merge/fold logic composes
// correctly across nesting, not just for a single flat `if`. Trace: the
// inner `if` (no `else`) merges its own entry (External, inherited from
// the outer `if`'s own entry) with its taken branch (Local) -> Unknown;
// the outer `if` (also no `else`) then merges ITS entry (External) with
// its own branch's result (Unknown, from the inner `if`) -> Unknown.
void testNestedIfMergeDoesNotStayExternal() {
    SourceManager sm;
    expectEscapingLocalSlice(analyzeAndCheck(sm,
                                              "fn bad(xs: [i32], cond: bool) -> [i32] {\n"
                                              "    let local = [1, 2, 3]\n"
                                              "    mut s = xs\n"
                                              "    if cond {\n"
                                              "        if cond {\n"
                                              "            s = slice(local)\n"
                                              "        }\n"
                                              "    }\n"
                                              "    return s\n"
                                              "}"));
}

} // namespace

int main() {
    testDirectParameterForwardingIsSafeReturn();
    testSingleCopyOfParameterIsSafeReturn();
    testDoubleCopyOfParameterIsSafeReturn();

    testLocalArraySliceDirectReturnRejected();
    testLocalArraySliceViaIntermediateCopyReturnRejected();
    testFixedArrayParameterSliceReturnRejected();
    testExternalReassignedToLocalThenReturnedRejected();
    testUnknownAfterBranchMergeReturnRejected();
    testArbitraryFunctionCallResultIsUnknown();
    testLocalUseOfUnknownCallResultIsValid();

    testIfElseMixedBranchesMergeToUnknownRejectsReturn();
    testIfElseAllExternalBranchesStayExternalAllowsReturn();
    testWhileLoopTouchingBindingForcesUnknownRejectsReturn();
    testIfElseBothBranchesLocalStaysLocalRejectsReturn();

    testForLoopTouchingBindingForcesUnknownRejectsReturn();
    testProvenanceDoesNotLeakAcrossFunctions();
    testNestedIfMergeDoesNotStayExternal();

    return kai::test::failureCount == 0 ? 0 : 1;
}
