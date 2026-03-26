#pragma once

#include <optional>
#include <string>

namespace type_proof {

// Minimal view of provider evidence needed for type_proof verification.
struct ProviderEvidenceView {
    int         provider_id = -1;
    std::string nonce;
    std::string masked_wire; // canonical decimal integer string (masked_value)
};

// Minimal view of provider manifest evidence used as binding inputs.
struct ManifestMinimalView {
    std::string share_manifest_id;
    std::string mask_commitment_leaves_digest;
};

// Parsed provider-submitted type_proof evidence.
struct Evidence {
    int provider_id = -1;
    std::string nonce;
    std::string schema_id;
    std::string masked_wire_digest;
    std::string share_manifest_id;
    std::string mask_commitment_leaves_digest;

    std::string type_proof_system;
    std::string type_proof_vk_id;
    std::string type_proof_blob;
    std::string type_proof_id;
};

// Parse inputs/provider_<id>_type_proof.json (regex-based, stable-format assumption).
std::optional<Evidence> parse_evidence_file(const std::string& json_content);
std::optional<Evidence> load_evidence_file(const std::string& path);

// Verify bindings + call the selected backend (factory by type_proof_system).
// Returns true on success; false on failure and sets out_reason to a stable string.
bool verify_for_provider(const ProviderEvidenceView& provider,
                         const ManifestMinimalView& manifest,
                         const Evidence& tp,
                         const std::string& expected_schema_id,
                         std::string* out_reason);

} // namespace type_proof

