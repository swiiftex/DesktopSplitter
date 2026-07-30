// PresenterProbe.cpp - unit tests for presenter selection and fallback.
//
// Includes the real src/PresenterSelect.h, so this exercises exactly the policy
// dscomp runs. Acquisition is mocked by constructing SpecializedAvailability
// values directly, which lets every failure mode be tested without touching a
// display.
//
// Exit code 0 = all passed, 1 = at least one failure.

#include "../src/PresenterSelect.h"

#include <cstdio>
#include <string>

using namespace ds;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const wchar_t* name, const std::wstring& detail) {
    if (ok) {
        ++g_passed;
        ::wprintf(L"  pass  %s\n", name);
    } else {
        ++g_failed;
        ::wprintf(L"  FAIL  %s  --  %s\n", name, detail.c_str());
    }
}

void ExpectWindow(const SpecializedAvailability& a, bool expectFallback,
                  const wchar_t* mustContain, const wchar_t* name) {
    const PresenterSelection s = ChoosePresenter(a);
    std::wstring detail = L"kind=";
    detail += PresenterKindName(s.kind);
    detail += L" fellBack=";
    detail += s.fellBack ? L"true" : L"false";
    detail += L" reason='" + s.reason + L"'";

    const bool ok = s.kind == PresenterKind::Window &&
                    s.fellBack == expectFallback &&
                    (mustContain == nullptr ||
                     s.reason.find(mustContain) != std::wstring::npos);
    Check(ok, name, detail);
}

// A fully successful probe result.
SpecializedAvailability FullySuccessful() {
    SpecializedAvailability a;
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.targetSpecialized = true;
    a.acquired = true;
    a.presentationReady = true;
    return a;
}

void TestNotRequested() {
    ::wprintf(L"\nspecialized not requested\n");
    SpecializedAvailability a;   // requested = false
    ExpectWindow(a, false, L"not requested", L"defaults to the window presenter");

    // Even if everything else somehow probed true, not requesting wins.
    SpecializedAvailability b = FullySuccessful();
    b.requested = false;
    ExpectWindow(b, false, L"not requested",
                 L"never uses specialized without being asked");
}

void TestHappyPath() {
    ::wprintf(L"\nspecialized requested and fully available\n");
    const PresenterSelection s = ChoosePresenter(FullySuccessful());
    Check(s.kind == PresenterKind::Specialized, L"chooses the specialized presenter",
          s.reason);
    Check(!s.fellBack, L"does not report a fallback", s.reason);
}

void TestFallbackLadder() {
    ::wprintf(L"\nfallback: each acquisition step failing in turn\n");

    // Mock: DisplayManager::Create threw.
    SpecializedAvailability a;
    a.requested = true;
    a.detail = L"class not registered";
    ExpectWindow(a, true, L"Display.Core is unavailable",
                 L"API unavailable -> window, reports API");

    // Mock: no target matched the configured monitor.
    a = SpecializedAvailability();
    a.requested = true;
    a.apiAvailable = true;
    ExpectWindow(a, true, L"no display target matched",
                 L"target not found -> window, reports target");

    // Mock: the real situation today - monitor is not marked specialized.
    a = SpecializedAvailability();
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.detail = L"UsageKind is not SpecialPurpose";
    ExpectWindow(a, true, L"could not be prepared",
                 L"target not preparable -> window");

    // Mock: marked specialized but acquisition denied.
    a = SpecializedAvailability();
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.targetSpecialized = true;
    a.detail = L"TargetAccessDenied";
    ExpectWindow(a, true, L"could not be acquired",
                 L"acquisition denied -> window, points at the Settings toggle");

    // Mock: acquired but the mode/primaries could not be set up.
    a = SpecializedAvailability();
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.targetSpecialized = true;
    a.acquired = true;
    a.detail = L"TryApply did not succeed";
    ExpectWindow(a, true, L"display mode could not be set",
                 L"presentation not ready -> window, reports mode failure");
}

void TestDetailPropagation() {
    ::wprintf(L"\nfailure detail is surfaced to the log\n");
    SpecializedAvailability a;
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.targetSpecialized = true;
    a.detail = L"TargetAccessDenied";
    const PresenterSelection s = ChoosePresenter(a);
    Check(s.reason.find(L"TargetAccessDenied") != std::wstring::npos,
          L"API detail is appended to the reason", s.reason);

    a.detail.clear();
    const PresenterSelection s2 = ChoosePresenter(a);
    Check(s2.reason.find(L"()") == std::wstring::npos,
          L"no empty parentheses when there is no detail", s2.reason);
}

void TestReasonAlwaysSet() {
    ::wprintf(L"\nevery outcome carries a reason\n");
    for (int mask = 0; mask < 64; ++mask) {
        SpecializedAvailability a;
        a.requested = (mask & 1) != 0;
        a.apiAvailable = (mask & 2) != 0;
        a.targetFound = (mask & 4) != 0;
        a.targetSpecialized = (mask & 8) != 0;
        a.acquired = (mask & 16) != 0;
        a.presentationReady = (mask & 32) != 0;
        const PresenterSelection s = ChoosePresenter(a);
        if (s.reason.empty()) {
            Check(false, L"reason is never empty",
                  L"empty reason for mask " + std::to_wstring(mask));
            return;
        }
        // Specialized may only be chosen when every gate passed.
        const bool allTrue = (mask == 63);
        if ((s.kind == PresenterKind::Specialized) != allTrue) {
            Check(false, L"specialized only when every gate passes",
                  L"mismatch at mask " + std::to_wstring(mask));
            return;
        }
        // Falling back is reported exactly when specialized was wanted but not used.
        const bool expectFallback = a.requested && !allTrue;
        if (s.fellBack != expectFallback) {
            Check(false, L"fellBack is set exactly when a request was downgraded",
                  L"mismatch at mask " + std::to_wstring(mask));
            return;
        }
    }
    Check(true, L"all 64 probe combinations behave consistently", L"");
}

// The start-ordering that makes the black gap a blink: dscomp waits, the user
// toggles, dscomp acquires.
void TestWaitForTarget() {
    ::wprintf(L"\nwait-for-target polling\n");

    Check(!ShouldKeepWaiting(0, 0.0, false),
          L"waiting disabled: a single attempt, no polling", L"");
    Check(ShouldKeepWaiting(30, 0.0, false),
          L"wait enabled: keeps polling at t=0", L"");
    Check(ShouldKeepWaiting(30, 29.9, false),
          L"keeps polling just before the deadline", L"");
    Check(!ShouldKeepWaiting(30, 30.0, false),
          L"stops exactly at the deadline", L"");
    Check(!ShouldKeepWaiting(30, 45.0, false),
          L"stops after the deadline", L"");
    Check(!ShouldKeepWaiting(30, 1.0, true),
          L"stops immediately once acquired, mid-wait", L"");
    Check(!ShouldKeepWaiting(0, 0.0, true),
          L"stops once acquired even with waiting disabled", L"");

    Check(WaitSecondsRemaining(30, 0.0) == 30, L"countdown starts at the full wait", L"");
    Check(WaitSecondsRemaining(30, 18.0) == 12,
          L"countdown reports 12s left after 18s of a 30s wait", L"");
    Check(WaitSecondsRemaining(30, 30.0) == 0, L"countdown floors at 0", L"");
    Check(WaitSecondsRemaining(30, 99.0) == 0, L"countdown never goes negative", L"");
}

void TestWaitOutcomes() {
    ::wprintf(L"\nwait outcome -> presenter selection\n");

    // Wait expired: nothing was acquired, so this is an ordinary fallback and
    // the process still exits 0.
    SpecializedAvailability expired;
    expired.requested = true;
    expired.apiAvailable = true;
    expired.targetFound = true;
    expired.targetSpecialized = true;
    expired.detail = L"TargetAccessDenied";
    const PresenterSelection s1 = ChoosePresenter(expired);
    Check(s1.kind == PresenterKind::Window && s1.fellBack,
          L"wait expiry falls back to the window presenter", s1.reason);
    Check(ExitCodeForRun(false) == exitcode::kOk,
          L"...and wait expiry keeps exit-0 semantics", L"");
    Check(s1.reason.find(L"Remove display from desktop") != std::wstring::npos,
          L"...and the reason tells the user about the Settings toggle", s1.reason);

    // Waited, then the user toggled and acquisition succeeded.
    const PresenterSelection s2 = ChoosePresenter(FullySuccessful());
    Check(s2.kind == PresenterKind::Specialized && !s2.fellBack,
          L"wait-then-acquire selects the specialized presenter", s2.reason);
}

void TestFrameDeadline() {
    ::wprintf(L"\nframe deadline (acquired but black)\n");

    Check(!FrameDeadlineExceeded(20, false, 0.0),
          L"not exceeded immediately after acquisition", L"");
    Check(!FrameDeadlineExceeded(20, false, 19.9),
          L"not exceeded just before the deadline", L"");
    Check(FrameDeadlineExceeded(20, false, 20.1),
          L"exceeded just after the deadline", L"");
    Check(!FrameDeadlineExceeded(20, true, 999.0),
          L"never exceeded once a frame has been scanned out", L"");
    Check(!FrameDeadlineExceeded(0, false, 999.0),
          L"deadline of 0 disables the check", L"");

    // Blowing the deadline is a runtime failure, not a fallback: the display is
    // already specialized and black, so the app must be told to un-specialize.
    Check(ExitCodeForRun(true) == exitcode::kDisplayLost,
          L"a blown frame deadline maps to exit 20 (kDisplayLost)", L"");
    Check(ExitCodeForRun(true) != ExitCodeForRun(false),
          L"...which is distinguishable from a clean exit", L"");
}

void TestExitCodes() {
    ::wprintf(L"\nprocess exit codes (contract with the control app)\n");

    Check(ExitCodeForRun(false) == exitcode::kOk,
          L"a clean run exits 0", L"");
    Check(ExitCodeForRun(true) == exitcode::kDisplayLost,
          L"losing the display while presenting exits with kDisplayLost", L"");
    Check(exitcode::kDisplayLost == 20,
          L"kDisplayLost is 20, as documented", L"");
    Check(ExitCodeForRun(true) != exitcode::kOk,
          L"the display-lost code is nonzero so the app can detect it", L"");

    // Runtime failures must never collide with a startup failure, otherwise the
    // app cannot tell "never started" from "started then went dark".
    const int startup[] = { exitcode::kOk,          exitcode::kBadArgs,
                            exitcode::kConfigFailed, exitcode::kMonitorNotFound,
                            exitcode::kWindowFailed, exitcode::kRendererFailed };
    bool distinct = true;
    for (int a : startup) {
        if (a == exitcode::kDisplayLost) distinct = false;
    }
    Check(distinct, L"kDisplayLost does not collide with any startup code", L"");

    for (size_t i = 0; i < sizeof(startup) / sizeof(startup[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(startup) / sizeof(startup[0]); ++j) {
            if (startup[i] == startup[j]) {
                Check(false, L"startup exit codes are pairwise distinct",
                      L"duplicate startup code");
                return;
            }
        }
    }
    Check(true, L"startup exit codes are pairwise distinct", L"");
}

// The specialized path must be the only one that can report a lost display:
// the window path draws to a swapchain the desktop owns, so "gone dark" is not
// a state it can meaningfully reach.
void TestFailureOnlyMeaningfulWhenSpecialized() {
    ::wprintf(L"\nfailure reporting is tied to the specialized path\n");

    const PresenterSelection specialized = ChoosePresenter(FullySuccessful());
    Check(specialized.kind == PresenterKind::Specialized,
          L"a fully available probe selects the specialized presenter",
          specialized.reason);

    SpecializedAvailability a;
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.targetSpecialized = true;
    a.detail = L"TargetAccessDenied";
    const PresenterSelection fallback = ChoosePresenter(a);
    Check(fallback.kind == PresenterKind::Window && fallback.fellBack,
          L"a failed acquisition falls back rather than reporting display-lost",
          fallback.reason);
    Check(ExitCodeForRun(false) == exitcode::kOk,
          L"...and that fallback still exits 0, not kDisplayLost", L"");
}

} // namespace

int wmain() {
    ::wprintf(L"dscomp presenter probe\n");
    ::wprintf(L"======================\n");

    TestNotRequested();
    TestHappyPath();
    TestFallbackLadder();
    TestDetailPropagation();
    TestReasonAlwaysSet();
    TestWaitForTarget();
    TestWaitOutcomes();
    TestFrameDeadline();
    TestExitCodes();
    TestFailureOnlyMeaningfulWhenSpecialized();

    ::wprintf(L"\n======================\n");
    ::wprintf(L"%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
