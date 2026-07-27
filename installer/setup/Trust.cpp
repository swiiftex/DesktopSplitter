#include "Trust.h"

#include <wintrust.h>
#include <softpub.h>
#include <strsafe.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
    void FormatFileTimeUtc(const FILETIME& Time, wchar_t* Buffer, size_t BufferChars)
    {
        SYSTEMTIME SystemTime = {};
        if (!FileTimeToSystemTime(&Time, &SystemTime))
        {
            StringCchCopyW(Buffer, BufferChars, L"?");
            return;
        }

        StringCchPrintfW(
            Buffer,
            BufferChars,
            L"%04u-%02u-%02u %02u:%02u:%02u UTC",
            SystemTime.wYear,
            SystemTime.wMonth,
            SystemTime.wDay,
            SystemTime.wHour,
            SystemTime.wMinute,
            SystemTime.wSecond);
    }

    HCERTSTORE OpenLocalMachineStore(const wchar_t* StoreName)
    {
        return CertOpenStore(
            CERT_STORE_PROV_SYSTEM_W,
            0,
            0,
            CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
            StoreName);
    }
}

namespace DeskSplit
{
    _Use_decl_annotations_
    LONG VerifyFileTrust(const wchar_t* Path)
    {
        WINTRUST_FILE_INFO FileInfo = {};
        FileInfo.cbStruct = sizeof(FileInfo);
        FileInfo.pcwszFilePath = Path;

        GUID ActionGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;

        WINTRUST_DATA TrustData = {};
        TrustData.cbStruct = sizeof(TrustData);
        TrustData.dwUIChoice = WTD_UI_NONE;                 // never pop a dialog
        TrustData.fdwRevocationChecks = WTD_REVOKE_NONE;    // offline machines must still work
        TrustData.dwUnionChoice = WTD_CHOICE_FILE;
        TrustData.pFile = &FileInfo;
        TrustData.dwStateAction = WTD_STATEACTION_VERIFY;
        TrustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

        const LONG Status = WinVerifyTrust(
            static_cast<HWND>(INVALID_HANDLE_VALUE),
            &ActionGuid,
            &TrustData);

        TrustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &ActionGuid, &TrustData);

        return Status;
    }

    const wchar_t* TrustStatusText(LONG Status)
    {
        switch (Status)
        {
        case ERROR_SUCCESS:
            return L"signature is valid and chains to a trusted root";
        case TRUST_E_NOSIGNATURE:
            return L"the file is not signed";
        case TRUST_E_BAD_DIGEST:
            return L"the file has been modified since it was signed";
        case TRUST_E_PROVIDER_UNKNOWN:
            return L"no trust provider is available for this file type";
        case TRUST_E_SUBJECT_FORM_UNKNOWN:
            return L"the trust provider does not understand this file type";
        case TRUST_E_EXPLICIT_DISTRUST:
            return L"the signature is explicitly distrusted on this machine";
        case CRYPT_E_SECURITY_SETTINGS:
            return L"local security settings blocked the verification";
        case static_cast<LONG>(CERT_E_UNTRUSTEDROOT):
            return L"signed, but the root certificate is not trusted on this machine";
        case static_cast<LONG>(CERT_E_CHAINING):
            return L"signed, but the certificate chain could not be built";
        case static_cast<LONG>(CERT_E_EXPIRED):
            return L"the signing certificate has expired";
        case static_cast<LONG>(CERT_E_UNTRUSTEDTESTROOT):
            return L"signed by a test root certificate that is not trusted";
        case static_cast<LONG>(CERT_E_WRONG_USAGE):
            return L"the certificate is not valid for code signing";
        default:
            return L"verification failed";
        }
    }

    _Use_decl_annotations_
    PCCERT_CONTEXT LoadSignerCertificate(const wchar_t* Path)
    {
        HCERTSTORE Store = nullptr;
        HCRYPTMSG Message = nullptr;
        DWORD Encoding = 0;
        DWORD ContentType = 0;
        DWORD FormatType = 0;

        // Ask specifically for the PKCS#7 interpretation.
        //
        // A catalog is *also* a valid Certificate Trust List, and with
        // CERT_QUERY_CONTENT_FLAG_ALL CryptQueryObject resolves a .cat as
        // CERT_QUERY_CONTENT_CTL: it then hands back an empty certificate store
        // and no message, so there is no signer to find. Restricting the flags
        // to the PKCS#7 content types makes it parse the signature instead,
        // which works for both a .cat (PKCS7_SIGNED) and a PE file with an
        // embedded signature (PKCS7_SIGNED_EMBED).
        if (!CryptQueryObject(
                CERT_QUERY_OBJECT_FILE,
                Path,
                CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED | CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                CERT_QUERY_FORMAT_FLAG_BINARY,
                0,
                &Encoding,
                &ContentType,
                &FormatType,
                &Store,
                &Message,
                nullptr))
        {
            return nullptr;
        }

        PCCERT_CONTEXT Result = nullptr;

        DWORD SignerInfoSize = 0;
        if (Message != nullptr &&
            CryptMsgGetParam(Message, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &SignerInfoSize) &&
            SignerInfoSize > 0)
        {
            auto* SignerInfo = static_cast<CERT_INFO*>(LocalAlloc(LPTR, SignerInfoSize));
            if (SignerInfo != nullptr)
            {
                if (CryptMsgGetParam(Message, CMSG_SIGNER_CERT_INFO_PARAM, 0, SignerInfo, &SignerInfoSize))
                {
                    Result = CertFindCertificateInStore(
                        Store,
                        Encoding,
                        0,
                        CERT_FIND_SUBJECT_CERT,
                        SignerInfo,
                        nullptr);
                }

                LocalFree(SignerInfo);
            }
        }

        if (Message != nullptr)
        {
            CryptMsgClose(Message);
        }
        if (Store != nullptr)
        {
            // The certificate context holds its own reference to the store.
            CertCloseStore(Store, 0);
        }

        return Result;
    }

    _Use_decl_annotations_
    bool SummarizeCertificate(PCCERT_CONTEXT pCert, CertificateSummary* pSummary)
    {
        ZeroMemory(pSummary, sizeof(*pSummary));

        if (pCert == nullptr)
        {
            return false;
        }

        CertGetNameStringW(
            pCert,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            nullptr,
            pSummary->SubjectName,
            static_cast<DWORD>(ARRAYSIZE(pSummary->SubjectName)));

        CertGetNameStringW(
            pCert,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            CERT_NAME_ISSUER_FLAG,
            nullptr,
            pSummary->IssuerName,
            static_cast<DWORD>(ARRAYSIZE(pSummary->IssuerName)));

        FormatFileTimeUtc(pCert->pCertInfo->NotBefore, pSummary->NotBefore, ARRAYSIZE(pSummary->NotBefore));
        FormatFileTimeUtc(pCert->pCertInfo->NotAfter, pSummary->NotAfter, ARRAYSIZE(pSummary->NotAfter));

        DWORD HashSize = static_cast<DWORD>(kSha1Length);
        if (!CertGetCertificateContextProperty(pCert, CERT_SHA1_HASH_PROP_ID, pSummary->Sha1, &HashSize))
        {
            return false;
        }

        wchar_t* Cursor = pSummary->Thumbprint;
        size_t Remaining = ARRAYSIZE(pSummary->Thumbprint);
        for (size_t Index = 0; Index < kSha1Length; ++Index)
        {
            const wchar_t* Format = (Index + 1 < kSha1Length) ? L"%02X " : L"%02X";
            if (FAILED(StringCchPrintfExW(Cursor, Remaining, &Cursor, &Remaining, 0, Format, pSummary->Sha1[Index])))
            {
                break;
            }
        }

        // Self-signed when subject == issuer.
        pSummary->SelfSigned = CertCompareCertificateName(
            X509_ASN_ENCODING,
            &pCert->pCertInfo->Subject,
            &pCert->pCertInfo->Issuer) != FALSE;

        return true;
    }

    _Use_decl_annotations_
    DWORD AddCertificateToStore(PCCERT_CONTEXT pCert, const wchar_t* StoreName)
    {
        HCERTSTORE Store = OpenLocalMachineStore(StoreName);
        if (Store == nullptr)
        {
            return GetLastError();
        }

        DWORD Result = ERROR_SUCCESS;
        if (!CertAddCertificateContextToStore(Store, pCert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr))
        {
            Result = GetLastError();
        }

        CertCloseStore(Store, 0);
        return Result;
    }

    _Use_decl_annotations_
    DWORD RemoveCertificateFromStore(const BYTE* Sha1, const wchar_t* StoreName)
    {
        HCERTSTORE Store = OpenLocalMachineStore(StoreName);
        if (Store == nullptr)
        {
            return GetLastError();
        }

        CRYPT_HASH_BLOB HashBlob = {};
        HashBlob.cbData = static_cast<DWORD>(kSha1Length);
        HashBlob.pbData = const_cast<BYTE*>(Sha1);

        DWORD Result = ERROR_SUCCESS;

        for (;;)
        {
            PCCERT_CONTEXT Found = CertFindCertificateInStore(
                Store,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0,
                CERT_FIND_SHA1_HASH,
                &HashBlob,
                nullptr);

            if (Found == nullptr)
            {
                break;
            }

            // CertDeleteCertificateFromStore always frees the context.
            if (!CertDeleteCertificateFromStore(Found))
            {
                Result = GetLastError();
                break;
            }
        }

        CertCloseStore(Store, 0);
        return Result;
    }
}
