# Next Protocol Layer Interface: `type_proof`

## Goal
The current semi2k prototype validates:
- provider file integrity/authenticity (keyed BLAKE2b proof)
- canonical *wire syntax* of masked payloads (decimal integer string)
- share consistency via distributed per-`party_index` ACK coverage

What is still missing is a semantic layer:
- `masked_value` does **not** let the system derive the semantic type of the hidden clear value (bool, fixed_point(scale), recursive schema composition, etc.).
- we therefore need a `type_proof` layer proving that the hidden clear value is correctly encoded according to `schema_id`.

This document proposes a clean artifact/interface for the next layer so it integrates with the existing provider manifest + ACK/admission path, while keeping:
- bridge execution-only
- no raw-share inspection in consensus
- distributed verification (one verifier per `party_index`) for shares
- share verification remains distributed; semantic type validity remains consensus-verified

## Design principles (matching the repo’s current trust split)
1. **Consensus should remain admission-only**: verify signatures and binding fields, and enforce coverage.
2. **No single verifier should see all shares**: each verifier must only load its local share file.
3. **`type_proof` must be bindable to the same provider evidence and manifest digests** already used by the share-consistency ACK path.
4. **No `type_ack` family**: computation nodes remain responsible only for local share verification + their existing share ACK signatures.

## Backend notes (current prototype)

- `stub-typeproof-v1`: deterministic placeholder evidence (integration-focused).
- `semantic-schema-v1`: not a ZK proof; the provider submits semantic evidence that `consensus` checks against the authoritative `schemas/type_registry.json`.
- `proof-real-v1`: stronger commitment-based prototype (Pedersen commitments + OR-proof / commitment equation checks), but still not a full SNARK; it relies on the share-consistency material authenticated by the ACKs.

## Artifacts Overview (Phase 2)
We introduce a single new artifact:
1. **Provider-submitted `type_proof` evidence**
   - public location under `inputs/`

Consensus then admits a provider only if:
- share-consistency ACK coverage holds (already implemented)
- the provider-submitted `type_proof` artifact verifies directly in `consensus` (no extra ACK family)

## Provider submission: `type_proof` evidence artifact

### File path
`inputs/provider_<provider_id>_type_proof.json`

### Suggested JSON schema (clean interface)
Fields (minimum):
- `provider_id` (int, matches the provider submission)
- `schema_id` (string, matches declared recursive schema)
- `type_proof_id` (string, digest of the evidence blob)
- `type_proof_system` (string, identifies the proof system/verification key family)
- `type_proof_vk_id` (string, optional indirection to a published verification key)
- `type_proof_blob` (base64 string, opaque proof transcript)
- `masked_wire_digest` (string, digest binding to the canonical masked wire integer string used in `inputs/provider_<provider_id>.txt`)
- `nonce` (string) (same nonce that already exists in `inputs/provider_<provider_id>.txt` for binding / replay protection)
- `share_manifest_id` (string, binds provider evidence to the public share manifest commitments/digests)
- `mask_commitment_leaves_digest` (string, binds provider evidence to the public commitment digest used in share ACKs)

Note: the current verifier prototype does not require/use `protocol_version` for acceptance (it can be present in the artifact without affecting checks).

**Binding rule (recommended):**
`type_proof_id = H( provider_id || nonce || schema_id || type_proof_system || type_proof_vk_id || type_proof_blob )`
(use the same canonical hash primitive family you’re using for manifest digests; today that’s unkeyed BLAKE2b-256 hex).

## How `consensus` verifies `type_proof`
Consensus verification should stay “admission-only”:
1. Load `inputs/provider_<provider_id>_type_proof.json`.
2. Verify canonical binding fields against authoritative context:
   - `provider_id` and `nonce` match `inputs/provider_<provider_id>.txt`
   - `masked_wire_digest` matches the digest of the canonical masked wire used by provider admission (`inputs/provider_<provider_id>.txt`)
   - `schema_id` is confronted with the authoritative `schema_id` expected for the round/context (consensus does not trust it blindly)
   - `share_manifest_id` and `mask_commitment_leaves_digest` match `inputs/provider_<provider_id>_manifest.json`
3. Verify `type_proof_id` / integrity via recomputation of the digest.
4. Call the semantic type proof backend/hook (initially stubbed in the prototype):
   - validate that `type_proof_blob` proves the semantic type correctness of the hidden clear value w.r.t. the authoritative schema.

Consensus must not:
- touch raw share values (no `provider_<id>_share_<p>.secret` loading)
- reconstruct the hidden clear value
- move semantic type verification into the bridge

## Integration with the current provider manifest + share ACK path
You currently have:
- `inputs/provider_<id>_manifest.json`
- per-party `share_verifier` ACKs with `share_manifest_id`, `mask_commitment_leaves_digest`, `share_file_digest`, etc.

Recommended minimal integration step:
1. Keep the provider manifest unchanged for the share-consistency layer.
2. Put the complete `type_proof` evidence in `inputs/provider_<id>_type_proof.json`.
3. Have `consensus` enforce that the `type_proof` binds to the manifest digests that are already used by the share ACK path.

This creates a single consistent “evidence chain”:
`provider evidence` → `manifest digests` → `share_ack coverage`  
and then: `provider evidence + type_proof evidence` → `type_proof_id` → `semantic type verified in consensus`.

## Provider submission format changes (summary)
Today:
- `inputs/provider_<id>.txt` (id, masked_value, nonce, proof)
- `inputs/provider_<id>_manifest.json`

Next:
- add `inputs/provider_<id>_type_proof.json` (or embed the fields into provider file if you prefer)
- optionally add `type_proof_id` into the existing `provider_<id>_manifest.json` for tighter linkage (future)

## Notes on hidden semantic validity for recursive schemas
The interface deliberately keeps the proof system opaque:
- for bool, fixed_point(scale), and recursive compositions you can use:
  - ZK proofs,
  - MPC-assisted validity proofs,
  - or any construction you pick later.

As long as you can produce:
- a public `type_proof_blob`
- a stable `type_proof_id`
- consensus can verify the semantic validity proof against the authoritative `schema_id`

…the rest of the admission path remains stable.

## Open design questions (to decide when implementing `type_proof`)
1. Proof system choice (ZK vs MPC proof vs hybrid)
2. Whether `type_proof` verification requires any additional public inputs beyond the masked wire digest + schema_id (today the prototype also binds manifest digests)
3. How verification key family identifiers are sourced authoritatively by consensus (today stubbed to `stub-vk-v1`)

These choices affect only the verifier’s internal implementation, not the interface described above.

