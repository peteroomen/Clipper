// Clipper — test support: prove `assert()` is actually compiled in.
//
// WHY THIS EXISTS (2026-07-24 audit, Test & process integrity):
//
//   "core/CMakeLists.txt:255 guards -UNDEBUG behind if(NOT MSVC), so on MSVC the entire
//    1129-line expectations suite compiles to a no-op main that prints 'All M11
//    player-expectations tests passed' — including with zero golden WAVs present."
//
// Every test in this tree is a plain `int main` + `<cassert>`, with no framework. That is a
// good, dependency-free choice, but it has one catastrophic failure mode: `assert()` is
// erased by `NDEBUG`, which CMake defines in every Release build — the documented build
// type. A suite whose asserts are erased does not fail. It prints its success banner and
// exits 0. There is no way to tell from the outside.
//
// The CMake `if(NOT MSVC)` guard is fixed (the flags are now applied on every platform),
// but a build-system flag is exactly the kind of thing that silently stops working — a
// generator change, a toolchain file, a `CMAKE_CXX_FLAGS` override, a new test target that
// forgets the helper. So the invariant is enforced at COMPILE TIME, in the translation unit
// that depends on it, where it cannot be bypassed by accident:
//
//   include this header from every test .cpp. If asserts are dead, the BUILD fails.
//
// A build failure is loud. A vacuous green suite is not.

#ifndef CLIPPER_TESTS_SUPPORT_ASSERTS_LIVE_H
#define CLIPPER_TESTS_SUPPORT_ASSERTS_LIVE_H

#ifdef NDEBUG
#error "NDEBUG is defined in a Clipper test translation unit, so every assert() in this \
file would compile to nothing and the test would pass unconditionally. The test targets \
must be built with -UNDEBUG (MSVC: /UNDEBUG) — see clipper_add_test_flags() in \
core/CMakeLists.txt. Do not silence this by removing the include; fix the flags."
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>

// A runtime belt-and-braces check for the same invariant, so a compiler that somehow
// accepted the above still cannot ship a no-op suite. Call once from main().
namespace clipper::test {
inline void requireAssertsLive() {
    bool ran = false;
    assert((ran = true) && "assert() is not evaluating its expression");
    if (!ran) {
        // Cannot use assert to report that assert is broken.
        std::fputs("FATAL: assert() did not evaluate — this test binary is a no-op.\n", stderr);
        std::abort();
    }
}
}  // namespace clipper::test

#endif  // CLIPPER_TESTS_SUPPORT_ASSERTS_LIVE_H
