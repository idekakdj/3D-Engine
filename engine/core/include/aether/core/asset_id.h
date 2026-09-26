// aether/core/asset_id.h — AssetId text helpers (additive to handle.h).
//
// ADDITIVE (not a frozen contract). AssetId::to_string() yields 32 lowercase hex
// characters (hi then lo, zero padded); asset_id_from_hex() is its exact inverse and
// accepts upper/lower case. AssetId::from_string() semantics (see handle.cpp):
//   hi = XXH64(s, seed 'AETHERID'), lo = FNV-1a 64(s)  - two independent 64-bit hashes;
//   an empty string maps to kInvalidAsset.
// Thread-safe (pure functions).
#pragma once

#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"

namespace aether {

// Parses exactly 32 hex digits. InvalidArgument on bad length / characters.
[[nodiscard]] Result<AssetId> asset_id_from_hex(StringView hex);

} // namespace aether
