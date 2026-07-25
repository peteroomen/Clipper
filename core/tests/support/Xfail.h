// Clipper — test support: XFAIL for KNOWN-BAD properties with an open audit finding.
//
// WHY THIS EXISTS (2026-07-24 audit, "Test & process integrity"):
// the audit's systemic finding is that a recurring class of test asserts an identity, a
// tautology, or the implementation against a reference derived from the same code — so
// wrong topologies and wrong constants pass. Fixing those tests exposes real, open
// defects. The two obvious ways to keep the suite green are BOTH the disease:
//
//   * `#if 0` the check       -> invisible; the property is silently untested again
//   * loosen the bound        -> invisible; the defect becomes canon (the AC30 starved
//                               phase inverter shipped through exactly a loosened bound)
//
// So instead: an XFAIL that RUNS the real measurement, PRINTS the real number, names the
// finding and the slice that will fix it, and — the load-bearing part —
//
//                     *** AN XPASS IS A HARD FAILURE ***
//
// The moment somebody actually fixes the defect, the suite goes RED until they delete the
// XFAIL. An XFAIL therefore cannot rot into a permanent excuse, and cannot be used to
// smuggle a genuinely-failing property past review.
//
// VISIBILITY. `ctest --output-on-failure` shows nothing for a passing test, so a binary
// that carries XFAILs also registers a companion ctest entry `<target>_xfail_ledger` which
// runs it with `--xfail-ledger` and exits 77 (`SKIP_RETURN_CODE 77`). A plain `ctest` run
// then prints e.g. `clipper_twin_tests_xfail_ledger ... ***Skipped` in the DEFAULT summary,
// so the open defects are visible without any extra flag. See core/CMakeLists.txt.
//
// Portable C++17, header-only, no platform includes — same rules as core/.

#ifndef CLIPPER_TESTS_SUPPORT_XFAIL_H
#define CLIPPER_TESTS_SUPPORT_XFAIL_H

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace clipper::test {

// One known-bad property. Declared as a file-scope constant next to the check that uses
// it, and listed in the binary's ledger array so `--xfail-ledger` can print it.
struct XfailDecl {
    const char* id;        // stable short id, e.g. "finding7-pi-starved-tail"
    const char* finding;   // "2026-07-24 audit finding 7"
    const char* property;  // what SHOULD hold (the player-observable expectation)
    const char* owner;     // the slice that will fix it
};

namespace detail {

inline std::vector<const XfailDecl*>& seenXfails() {
    static std::vector<const XfailDecl*> v;
    return v;
}
inline std::vector<const XfailDecl*>& seenXpasses() {
    static std::vector<const XfailDecl*> v;
    return v;
}

}  // namespace detail

// Run a check whose expected outcome is FAILURE because of an open audit finding.
//
//   `holds`  the MEASURED property (true = the expectation is met)
//   `d`      the declaration naming the finding
//   `detail` the measured numbers, formatted by the caller — always printed, so a
//            reader of the test log sees how far off it currently is
//
// !holds -> "[XFAIL]" and the run CONTINUES.
//  holds -> "[XPASS]" and reportXfails() will return non-zero: fix the defect, delete
//           the XFAIL, in the same slice.
//
// Most suites run every check at 44.1 / 48 / 96 kHz, so the same declaration is exercised
// several times. An XPASS is recorded EVERY time (a property that holds at one rate and not
// another still has to be looked at), but the multi-line XFAIL banner prints only on first
// sight, with later hits condensed to one line — otherwise the ledger triples and the real
// [ok] lines drown.
inline void expectXfail(bool holds, const XfailDecl& d, const char* detail) {
    if (holds) {
        detail::seenXpasses().push_back(&d);
        std::printf("  [XPASS] %s: %s -- NOW HOLDS: %s\n", d.id, d.property, detail);
        std::printf("          %s appears FIXED. Delete this XFAIL and assert it for real.\n",
                    d.finding);
        std::fflush(stdout);
        return;
    }
    auto& seen = detail::seenXfails();
    const bool first = std::find(seen.begin(), seen.end(), &d) == seen.end();
    seen.push_back(&d);
    if (first) {
        std::printf("  [XFAIL] %s: %s\n", d.id, d.property);
        std::printf("          measured: %s\n", detail);
        std::printf("          KNOWN DEFECT -- %s; fix owned by %s\n", d.finding, d.owner);
    } else {
        std::printf("  [XFAIL] %s (as above): %s\n", d.id, detail);
    }
    std::fflush(stdout);
}

// Print the run's XFAIL summary. Returns the process exit code: 0 normally, 1 if any
// check XPASSed (a fixed defect with a stale XFAIL is a test-suite bug).
//
// Call as the last statement of main:  return clipper::test::reportXfails();
inline int reportXfails() {
    auto fails = detail::seenXfails();
    std::sort(fails.begin(), fails.end());
    fails.erase(std::unique(fails.begin(), fails.end()), fails.end());
    const auto& passes = detail::seenXpasses();
    if (!fails.empty()) {
        std::printf("\n--- XFAIL LEDGER: %zu known defect(s) exercised, still broken ---\n",
                    fails.size());
        for (const auto* d : fails)
            std::printf("  %-38s %s\n%-40s (fix: %s)\n", d->id, d->finding, "", d->owner);
    }
    if (!passes.empty()) {
        std::printf("\n*** XPASS: %zu XFAIL(ed) propert%s NOW HOLD ***\n", passes.size(),
                    passes.size() == 1 ? "y" : "ies");
        for (const auto* d : passes)
            std::printf("  %-34s %s\n", d->id, d->finding);
        std::printf("An XFAIL must not outlive its defect: assert the property for real and\n"
                    "delete the XFAIL. Failing the suite on purpose.\n");
        return 1;
    }
    return 0;
}

// `--xfail-ledger` mode: print this binary's declared known defects and exit 77, which
// ctest reports as Skipped (SKIP_RETURN_CODE 77) in its DEFAULT summary. Returns -1 when
// the flag is absent so main can fall through to the real tests.
inline int ledgerMain(int argc, char** argv, const XfailDecl* decls, std::size_t n,
                      const char* binaryName) {
    bool want = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--xfail-ledger") == 0) want = true;
    if (!want) return -1;
    std::printf("%s -- %zu KNOWN DEFECT(S) under XFAIL (2026-07-24 audit):\n", binaryName, n);
    for (std::size_t i = 0; i < n; ++i)
        std::printf("  %zu. [%s] %s\n     expected: %s\n     fix owned by: %s\n", i + 1,
                    decls[i].finding, decls[i].id, decls[i].property, decls[i].owner);
    std::printf("These properties ARE measured and printed by the real test run; they are\n"
                "recorded as XFAIL, not skipped. An XPASS fails the suite.\n");
    std::fflush(stdout);
    return 77;
}

}  // namespace clipper::test

#endif  // CLIPPER_TESTS_SUPPORT_XFAIL_H
