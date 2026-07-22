#include "listopad/signature.h"

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <array>
#include <optional>
#include <vector>

namespace listopad {
namespace {

using CertificateHash = std::array<std::byte, 32>;

std::optional<CertificateHash> verified_signer_hash(const std::filesystem::path& path) {
  WINTRUST_FILE_INFO file{sizeof(file)};
  file.pcwszFilePath = path.c_str();

  WINTRUST_DATA trust{sizeof(trust)};
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG verified = WinVerifyTrust(nullptr, &policy, &trust);
  std::optional<CertificateHash> result;
  if (verified == ERROR_SUCCESS && trust.hWVTStateData) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer = provider
        ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)
        : nullptr;
    if (signer && signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
      CertificateHash hash{};
      DWORD size = static_cast<DWORD>(hash.size());
      if (CertGetCertificateContextProperty(signer->pasCertChain[0].pCert,
                                            CERT_SHA256_HASH_PROP_ID,
                                            hash.data(), &size) &&
          size == hash.size()) {
        result = hash;
      }
    }
  }

  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &trust);
  return result;
}

}  // namespace

bool has_trusted_authenticode_signature(const std::filesystem::path& path) {
  return verified_signer_hash(path).has_value();
}

bool has_matching_authenticode_signer(const std::filesystem::path& left,
                                      const std::filesystem::path& right) {
  const auto left_hash = verified_signer_hash(left);
  const auto right_hash = verified_signer_hash(right);
  return left_hash && right_hash && *left_hash == *right_hash;
}

}  // namespace listopad
