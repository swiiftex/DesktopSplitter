// Trust.h - Authenticode verification of the driver catalog, extraction of the
// signer certificate, and installation/removal of that certificate in the
// LocalMachine trust stores.
//
// Trust model
// -----------
// A UMDF driver is user-mode code, so kernel driver signing policy (test
// signing / Secure Boot) does not apply to it. The only barrier on a normal
// Windows 10/11 machine is whether the driver package's catalog signature
// chains to a trusted root.
//
//   * Package signed by a Microsoft attestation/WHQL signature
//       -> WinVerifyTrust succeeds, nothing to do, zero prompts.
//   * Package signed by our development certificate
//       -> WinVerifyTrust fails with CERT_E_UNTRUSTEDROOT. The installer shows
//          the publisher and thumbprint and asks the user to confirm before
//          adding that certificate to LocalMachine\Root (so the chain
//          validates) and LocalMachine\TrustedPublisher (so PnP installs the
//          package without a second "do you trust" dialog).
#pragma once

#include <windows.h>
#include <wincrypt.h>

namespace DeskSplit
{
    constexpr size_t kSha1Length = 20;

    struct CertificateSummary
    {
        wchar_t SubjectName[256];       // simple display name (usually the CN)
        wchar_t IssuerName[256];
        wchar_t NotBefore[64];
        wchar_t NotAfter[64];
        wchar_t Thumbprint[kSha1Length * 3];   // "AA BB CC ..." form
        BYTE    Sha1[kSha1Length];
        bool    SelfSigned;
    };

    // WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2) over a file.
    // Returns ERROR_SUCCESS (0) when the signature chains to a trusted root,
    // otherwise the provider status (e.g. CERT_E_UNTRUSTEDROOT, TRUST_E_NOSIGNATURE).
    LONG VerifyFileTrust(_In_z_ const wchar_t* Path);

    // Text for the status codes VerifyFileTrust commonly returns.
    const wchar_t* TrustStatusText(_In_ LONG Status);

    // Loads the signer certificate out of a signed file (a .cat is a PKCS#7
    // signed object). Free with CertFreeCertificateContext.
    PCCERT_CONTEXT LoadSignerCertificate(_In_z_ const wchar_t* Path);

    // Human-readable summary of a certificate, for the confirmation prompt.
    bool SummarizeCertificate(_In_ PCCERT_CONTEXT pCert, _Out_ CertificateSummary* pSummary);

    // Adds the certificate to a LocalMachine system store ("Root",
    // "TrustedPublisher"). Returns a Win32 error code.
    DWORD AddCertificateToStore(_In_ PCCERT_CONTEXT pCert, _In_z_ const wchar_t* StoreName);

    // Removes any certificate with the given SHA-1 thumbprint from a
    // LocalMachine system store. ERROR_SUCCESS also when nothing matched.
    DWORD RemoveCertificateFromStore(
        _In_reads_bytes_(kSha1Length) const BYTE* Sha1,
        _In_z_ const wchar_t* StoreName);
}
