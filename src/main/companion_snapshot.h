#ifndef PSR_COMPANION_SNAPSHOT_H
#define PSR_COMPANION_SNAPSHOT_H

#include <cstddef>
#include <cstdint>

// C++ view of the lower-screen companion snapshot contract. The layout itself
// lives in companion_snapshot_fields.def, which the Android Gradle build also
// reads to generate the Java constants — edit the .def, never this file's
// values or the generated Java.
namespace psr::companion {

#define PSR_COMPANION_CONST(name, value)   inline constexpr int32_t k##name = (value);
#define PSR_COMPANION_MODE(name, value)    inline constexpr int32_t kMode##name = (value);
#define PSR_COMPANION_FLAG(name, value)    inline constexpr int32_t kFlag##name = (value);
#define PSR_COMPANION_HEADER(name, index)  inline constexpr size_t kHeader##name = (index);
#define PSR_COMPANION_WORDS(name, count)   inline constexpr int32_t k##name##PayloadWords = (count);
#define PSR_COMPANION_BATTLE(name, offset) inline constexpr size_t kBattle##name = kPayloadBase + (offset);
#define PSR_COMPANION_ORDER(name, offset)  inline constexpr size_t kOrder##name = kPayloadBase + (offset);

#include "companion_snapshot_fields.def"

#undef PSR_COMPANION_CONST
#undef PSR_COMPANION_MODE
#undef PSR_COMPANION_FLAG
#undef PSR_COMPANION_HEADER
#undef PSR_COMPANION_WORDS
#undef PSR_COMPANION_BATTLE
#undef PSR_COMPANION_ORDER

} // namespace psr::companion

#endif // PSR_COMPANION_SNAPSHOT_H
