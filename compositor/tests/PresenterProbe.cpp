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
    ExpectWindow(a, true, L"not marked as a specialized display",
                 L"UsageKind Standard -> window, reports the EDID requirement");
    {
        const PresenterSelection s = ChoosePresenter(a);
        Check(s.reason.find(L"docs/HIDING.md") != std::wstring::npos,
              L"the UsageKind failure points at docs/HIDING.md", s.reason);
        Check(s.reason.find(L"EDID override") != std::wstring::npos,
              L"the UsageKind failure names the EDID override", s.reason);
    }

    // Mock: marked specialized but acquisition denied.
    a = SpecializedAvailability();
    a.requested = true;
    a.apiAvailable = true;
    a.targetFound = true;
    a.targetSpecialized = true;
    a.detail = L"TargetAccessDenied";
    ExpectWindow(a, true, L"TryAcquireTarget did not succeed",
                 L"acquisition denied -> window, reports the result code");

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

} // namespace

int wmain() {
    ::wprintf(L"dscomp presenter probe\n");
    ::wprintf(L"======================\n");

    TestNotRequested();
    TestHappyPath();
    TestFallbackLadder();
    TestDetailPropagation();
    TestReasonAlwaysSet();

    ::wprintf(L"\n======================\n");
    ::wprintf(L"%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
