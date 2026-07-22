#pragma once

#include <cstddef>
#include <filesystem>

namespace listopad {

// Performs an offline Authenticode policy check and compares the SHA-256
// fingerprint of the leaf signing certificates.
bool has_trusted_authenticode_signature(const std::filesystem::path& path);
bool has_matching_authenticode_signer(const std::filesystem::path& left,
                                      const std::filesystem::path& right);

}  // namespace listopad
