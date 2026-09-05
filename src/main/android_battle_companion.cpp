#if defined(__ANDROID__)

#include <jni.h>

#include <array>
#include <atomic>
#include <cstdint>

#include <ultramodern/ultra_trace.hpp>

#include "companion_snapshot.h"

namespace {

using namespace psr::companion;

constexpr uint32_t kRdramSize = 8u * 1024u * 1024u;
constexpr size_t kBattleSectionAddressIndex = 259;
constexpr size_t kOrderSelectSectionAddressIndex = 267;
constexpr uint32_t kCombatantPointersOffset = 0x90010u;
constexpr uint32_t kBattlePokemonOffset = 0x68Cu;
constexpr uint32_t kParticipantOffset = 0x724u;
constexpr uint32_t kOrderSelectCountOffset = 0x379Cu;
constexpr uint32_t kOrderSelectPlayersOffset = 0x37A0u;
constexpr uint32_t kOrderSelectPlayerSize = 0x4B20u;
constexpr uint32_t kOrderSelectPokemonSize = 0x54u;

constexpr size_t kSnapshotWords = static_cast<size_t>(kCapacity);

using Snapshot = std::array<int32_t, kSnapshotWords>;

std::array<std::atomic<int32_t>, kSnapshotWords> g_snapshot{};
std::atomic<uint32_t> g_snapshot_sequence{0};
uint32_t g_frame_counter = 0;

extern "C" int32_t* section_addresses;

bool guest_range_valid(uint32_t vaddr, uint32_t size) {
    const uint32_t segment = vaddr & 0xE0000000u;
    if (segment != 0x80000000u && segment != 0xA0000000u) {
        return false;
    }
    const uint32_t paddr = vaddr & 0x1FFFFFFFu;
    return paddr <= kRdramSize && size <= kRdramSize - paddr;
}

uint8_t read_u8(const uint8_t* rdram, uint32_t vaddr) {
    return rdram[(vaddr & 0x1FFFFFFFu) ^ 3u];
}

uint16_t read_u16(const uint8_t* rdram, uint32_t vaddr) {
    return (uint16_t(read_u8(rdram, vaddr)) << 8) |
           uint16_t(read_u8(rdram, vaddr + 1));
}

uint32_t read_u32(const uint8_t* rdram, uint32_t vaddr) {
    return (uint32_t(read_u8(rdram, vaddr)) << 24) |
           (uint32_t(read_u8(rdram, vaddr + 1)) << 16) |
           (uint32_t(read_u8(rdram, vaddr + 2)) << 8) |
           uint32_t(read_u8(rdram, vaddr + 3));
}

bool valid_active_pokemon(const uint8_t* rdram, uint32_t address) {
    if (!guest_range_valid(address, 0x64u)) {
        return false;
    }

    const uint8_t species = read_u8(rdram, address + 0x0Bu);
    const uint8_t level = read_u8(rdram, address + 0x26u);
    const uint16_t current_hp = read_u16(rdram, address + 0x0Cu);
    const uint16_t max_hp = read_u16(rdram, address + 0x28u);
    if (species < 1 || species > 151 || level < 1 || level > 100 ||
        max_hp == 0 || max_hp > 999 || current_hp > max_hp) {
        return false;
    }

    for (uint32_t i = 0; i < 4; i++) {
        if (read_u8(rdram, address + 0x1Fu + i) > 165) {
            return false;
        }
    }
    return true;
}

int controller_index(const uint8_t* rdram, uint32_t combatant) {
    if (!guest_range_valid(combatant + kParticipantOffset, 4)) {
        return -1;
    }
    const uint32_t participant = read_u32(rdram, combatant + kParticipantOffset);
    if (!guest_range_valid(participant, 2)) {
        return -1;
    }
    return read_u8(rdram, participant + 1);
}

// Stamps the header every mode shares. Called last so the frame counter
// advances even when a capture failed and the payload stayed zeroed.
void finish(Snapshot& next, int32_t mode, int32_t payload_words, int32_t flags) {
    next[kHeaderVersion] = kSchemaVersion;
    next[kHeaderMode] = mode;
    next[kHeaderPayloadWords] = payload_words;
    next[kHeaderFrameCounter] = static_cast<int32_t>(++g_frame_counter);
    next[kHeaderFlags] = flags;

    g_snapshot_sequence.fetch_add(1, std::memory_order_acq_rel);
    for (size_t i = 0; i < next.size(); i++) {
        g_snapshot[i].store(next[i], std::memory_order_relaxed);
    }
    g_snapshot_sequence.fetch_add(1, std::memory_order_release);
}

void publish_standby(Snapshot& next) {
    finish(next, kModeStandby, 0, 0);
}

bool capture_order_selection(const uint8_t* rdram, Snapshot& next) {
    const uint32_t order_base = section_addresses != nullptr
        ? static_cast<uint32_t>(section_addresses[kOrderSelectSectionAddressIndex])
        : 0;
    if (!guest_range_valid(order_base, kOrderSelectPlayersOffset + 0x20u)) {
        return false;
    }

    const int32_t player_count = static_cast<int32_t>(
        read_u32(rdram, order_base + kOrderSelectCountOffset));
    if (player_count < 1 || player_count > 4) {
        return false;
    }

    for (int32_t i = 0; i < player_count; i++) {
        const uint32_t selection = order_base + kOrderSelectPlayersOffset
            + static_cast<uint32_t>(i) * kOrderSelectPlayerSize;
        if (!guest_range_valid(selection, 0x20u)) {
            return false;
        }

        const uint32_t team = read_u32(rdram, selection + 0x1Cu);
        if (!guest_range_valid(team, 2u)) {
            continue;
        }
        const bool computer_controlled = (read_u8(rdram, team) & 2u) != 0;
        if (computer_controlled || read_u8(rdram, team + 1u) != 0) {
            continue;
        }

        const int roster_count = read_u8(rdram, selection + 0x17u);
        const int selected_count = read_u8(rdram, selection + 0x0Bu);
        const int selection_state = read_u8(rdram, selection + 0x05u);
        const int confirmation_choice = read_u8(rdram, selection + 0x0Au);
        const uint32_t roster = read_u32(rdram, selection + 0x18u);
        if (roster_count < 1 || roster_count > static_cast<int>(kRosterSlots) ||
            selected_count < 0 || selected_count > roster_count ||
            selection_state < 0 || selection_state > 4 ||
            confirmation_choice < 0 || confirmation_choice > 1 ||
            !guest_range_valid(roster,
                static_cast<uint32_t>(roster_count) * kOrderSelectPokemonSize)) {
            return false;
        }

        for (int slot = 0; slot < roster_count; slot++) {
            const int species = read_u8(rdram,
                roster + static_cast<uint32_t>(slot) * kOrderSelectPokemonSize);
            if (species < 1 || species > 151) {
                return false;
            }
            next[kOrderRosterSpecies + slot] = species;
        }
        for (int order = 0; order < selected_count; order++) {
            const int slot = read_u8(rdram, selection + 0x0Cu + order);
            if (slot < 0 || slot >= roster_count ||
                next[kOrderRosterOrder + slot] != 0) {
                return false;
            }
            next[kOrderRosterOrder + slot] = order + 1;
        }

        next[kOrderSelectedCount] = selected_count;
        next[kOrderSelectionState] = selection_state;
        next[kOrderConfirmationChoice] = confirmation_choice;
        return true;
    }
    return false;
}

// Returns false when the battle fragment is absent or implausible, so the
// caller can fall through to the next candidate mode.
bool capture_battle(const uint8_t* rdram, Snapshot& next, int32_t& flags) {
    const uint32_t battle_base = section_addresses != nullptr
        ? static_cast<uint32_t>(section_addresses[kBattleSectionAddressIndex])
        : 0;
    if (!guest_range_valid(battle_base, 0xC6760u)) {
        return false;
    }
    const uint32_t combatant_pointers = battle_base + kCombatantPointersOffset;

    const uint32_t combatants[2] = {
        read_u32(rdram, combatant_pointers),
        read_u32(rdram, combatant_pointers + 4),
    };
    if (!guest_range_valid(combatants[0], kParticipantOffset + 4) ||
        !guest_range_valid(combatants[1], kParticipantOffset + 4)) {
        return false;
    }

    const uint32_t pokemon[2] = {
        combatants[0] + kBattlePokemonOffset,
        combatants[1] + kBattlePokemonOffset,
    };
    if (!valid_active_pokemon(rdram, pokemon[0]) ||
        !valid_active_pokemon(rdram, pokemon[1])) {
        return false;
    }

    const int controllers[2] = {
        controller_index(rdram, combatants[0]),
        controller_index(rdram, combatants[1]),
    };
    int player_side = 0;
    bool player_details = false;
    if (controllers[0] == 0 && controllers[1] != 0) {
        player_side = 0;
        player_details = true;
    } else if (controllers[1] == 0 && controllers[0] != 0) {
        player_side = 1;
        player_details = true;
    }
    const int opponent_side = 1 - player_side;
    const uint32_t player = pokemon[player_side];
    const uint32_t opponent = pokemon[opponent_side];

    next[kBattlePlayerSpecies] = read_u8(rdram, player + 0x0Bu);
    next[kBattlePlayerLevel] = read_u8(rdram, player + 0x26u);
    next[kBattlePlayerCurrentHp] = read_u16(rdram, player + 0x0Cu);
    next[kBattlePlayerMaxHp] = read_u16(rdram, player + 0x28u);
    next[kBattlePlayerStatus] = read_u8(rdram, player + 0x15u);
    next[kBattlePlayerType1] = read_u8(rdram, player + 0x1Cu);
    next[kBattlePlayerType2] = read_u8(rdram, player + 0x1Du);
    if (player_details) {
        next[kBattlePlayerAttack] = read_u16(rdram, player + 0x38u);
        next[kBattlePlayerDefense] = read_u16(rdram, player + 0x3Au);
        next[kBattlePlayerSpeed] = read_u16(rdram, player + 0x3Cu);
        next[kBattlePlayerSpecial] = read_u16(rdram, player + 0x3Eu);
        for (uint32_t i = 0; i < static_cast<uint32_t>(kMoveSlots); i++) {
            next[kBattlePlayerMove + i] = read_u8(rdram, player + 0x1Fu + i);
            next[kBattlePlayerPp + i] = read_u8(rdram, player + 0x32u + i) & 0x3F;
        }
    }

    next[kBattleOpponentSpecies] = read_u8(rdram, opponent + 0x0Bu);
    next[kBattleOpponentLevel] = read_u8(rdram, opponent + 0x26u);
    const int opponent_hp = read_u16(rdram, opponent + 0x0Cu);
    const int opponent_max_hp = read_u16(rdram, opponent + 0x28u);
    next[kBattleOpponentHpGauge] = opponent_hp > 0 && opponent_max_hp > 0
        ? (opponent_hp * kOpponentHpGaugeSteps + opponent_max_hp - 1) / opponent_max_hp
        : 0;
    next[kBattleOpponentStatus] = read_u8(rdram, opponent + 0x15u);
    next[kBattleOpponentType1] = read_u8(rdram, opponent + 0x1Cu);
    next[kBattleOpponentType2] = read_u8(rdram, opponent + 0x1Du);

    flags = player_details ? kFlagPlayerDetails : 0;
    return true;
}

} // namespace

extern "C" void psr_android_battle_companion_update(void) {
    Snapshot next{};
    const uint8_t* rdram = recomp_runtime_get_rdram();
    if (rdram == nullptr) {
        publish_standby(next);
        return;
    }

    int32_t flags = 0;
    if (capture_battle(rdram, next, flags)) {
        finish(next, kModeBattle, kBattlePayloadWords, flags);
        return;
    }

    // Each capture validates fully before writing, so the block is still clean
    // here. Re-zeroing keeps that a property of this dispatcher rather than an
    // obligation on every future capture routine.
    next = Snapshot{};
    if (capture_order_selection(rdram, next)) {
        finish(next, kModeOrderSelect, kOrderPayloadWords, 0);
        return;
    }

    next = Snapshot{};
    publish_standby(next);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_pokemonstadiumrecomp_probe_MainActivity_nativeGetBattleSnapshot(
        JNIEnv* env, jclass) {
    std::array<jint, kSnapshotWords> copy{};
    for (;;) {
        const uint32_t before = g_snapshot_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        for (size_t i = 0; i < copy.size(); i++) {
            copy[i] = g_snapshot[i].load(std::memory_order_relaxed);
        }
        const uint32_t after = g_snapshot_sequence.load(std::memory_order_acquire);
        if (before == after) {
            break;
        }
    }

    jintArray result = env->NewIntArray(static_cast<jsize>(copy.size()));
    if (result != nullptr) {
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(copy.size()), copy.data());
    }
    return result;
}

#endif
