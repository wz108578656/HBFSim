// Address layout contract: the OCP v0.7.0 field order and the model's
// historical bank-major order must describe the same capacity, stay invertible,
// and agree with the published bit assignment and the section 5.7 algebra.
#include "physical/hbf/hbf_address_layout.hpp"

#include <cstdint>
#include <set>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace hbfsim::physical::hbf;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

HbfDeviceConfig make_config(AddressLayout layout, std::uint32_t stacks,
                            std::uint32_t channels, std::uint32_t dies,
                            std::uint32_t planes, std::uint32_t blocks,
                            std::uint32_t pages) {
    HbfDeviceConfig config;
    config.address_layout = layout;
    config.stacks = stacks;
    config.channels_per_stack = channels;
    config.dies_per_channel = dies;
    config.planes_per_die = planes;
    config.blocks_per_plane = blocks;
    config.pages_per_block = pages;
    return config;
}

bool encoded_in_range(const HbfDeviceConfig& config, const PageCoordinates& page) {
    return page.stack < config.stacks && page.channel < config.channels_per_stack &&
           page.die < config.dies_per_channel && page.plane < config.planes_per_die &&
           page.block < config.blocks_per_plane && page.page < config.pages_per_block;
}

// decode() then encode() must return the original page index for every page,
// in both layouts and in degenerate geometries (one die, one bank, one page).
void round_trip(const HbfDeviceConfig& config) {
    const auto total = total_physical_pages(config);
    require(total > 0, "geometry has no pages");
    std::vector<std::uint64_t> samples{0, total - 1};
    for (std::uint64_t step = 1; step < total; step = step * 3 + 1) samples.push_back(step);
    for (std::uint64_t page = 0; page < total && page < 4096; ++page) samples.push_back(page);
    for (const auto ppn : samples) {
        const auto decoded = decode_page_index(config.address_layout, config, ppn);
        require(encoded_in_range(config, decoded), "decoded field outside geometry");
        require(encode_page_index(config.address_layout, config, decoded) == ppn,
                "layout round trip changed the page index");
    }
}

// bank_major is the model's historical layout and must keep its arithmetic:
// a bank owns one contiguous range, so both decoded fields follow division.
void bank_major_matches_history() {
    const auto config = make_config(AddressLayout::bank_major, 2, 4, 2, 3, 5, 7);
    const auto pages_per_bank = std::uint64_t{5} * 7;
    for (std::uint64_t ppn = 0; ppn < total_physical_pages(config); ++ppn) {
        const auto decoded = decode_page_index(AddressLayout::bank_major, config, ppn);
        const auto bank = ppn / pages_per_bank;
        require(decoded.plane == bank % 3, "bank_major bank field changed");
        require(decoded.die == (bank / 3) % 2, "bank_major die field changed");
        require(decoded.block == (ppn / 7) % 5, "bank_major block field changed");
        require(decoded.page == ppn % 7, "bank_major page field changed");
    }
}

// OCP section 4.6 (p.22): the channel-local page index selects the bank in its
// lowest bits and the die next, so a sequential stream rotates over every bank
// and die before advancing the page slot inside a block.
void ocp_puts_bank_and_die_in_the_low_bits() {
    const auto config = make_config(AddressLayout::ocp_v070, 1, 1, 2, 4, 8, 16);
    const auto bank_die_span = std::uint64_t{2} * 4;
    for (std::uint64_t offset = 0; offset < bank_die_span; ++offset) {
        const auto decoded = decode_page_index(AddressLayout::ocp_v070, config, offset);
        require(decoded.block == 0, "low index left the first block");
        require(decoded.page == 0, "low index advanced the page slot too early");
        require(decoded.plane == offset % 4, "bank is not the lowest field");
        require(decoded.die == offset / 4, "die is not the second-lowest field");
    }
    // The next bank_die_span indices advance the page slot, not the block.
    for (std::uint64_t offset = bank_die_span; offset < 2 * bank_die_span; ++offset) {
        const auto decoded = decode_page_index(AddressLayout::ocp_v070, config, offset);
        require(decoded.page == 1, "page slot did not advance after the bank/die sweep");
        require(decoded.plane == offset % 4, "bank does not rotate inside the page slot");
    }
}

// OCP section 5.7 (p.63): with R1 dies, R2 banks and R3 pages per bank block,
// the block-relative index P satisfies Bank_Num = P mod (R1*R2), and the replay
// start address L2 = B1 * R1 * R2 * R3 + Bank_Num.
void ocp_matches_section_5_7_algebra() {
    const std::uint32_t r1 = 4, r2 = 16, r3 = 256;
    const auto config = make_config(AddressLayout::ocp_v070, 1, 1, r1, r2, 2, r3);
    const std::uint64_t block_span = std::uint64_t{r1} * r2 * r3;
    for (std::uint32_t block = 0; block < 2; ++block) {
        for (std::uint32_t slot = 0; slot < r1 * r2; ++slot) {
            const auto ppn = std::uint64_t{block} * block_span + slot;
            const auto decoded = decode_page_index(AddressLayout::ocp_v070, config, ppn);
            const auto p = ppn % block_span;
            const auto bank_number = p % (std::uint64_t{r1} * r2);
            require(bank_number == std::uint64_t{decoded.die} * r2 + decoded.plane,
                    "Bank_Num disagrees with the section 5.7 formula");
            const auto start = std::uint64_t{block} * block_span + bank_number;
            require(start == ppn, "L2 start address disagrees with the layout");
        }
    }
}

// A sequential 4 KiB stream is the mechanism behind the bandwidth difference:
// bank_major serializes it on one sense resource per plane block range, while
// ocp_v070 spreads it over every bank and die of the channel from the start.
void sequential_stream_spreads_over_banks() {
    const auto config = make_config(AddressLayout::ocp_v070, 1, 1, 2, 4, 8, 16);
    const auto planes = std::uint64_t{4}, dies = std::uint64_t{2};
    std::set<std::uint32_t> banks_major, banks_ocp, dies_ocp;
    for (std::uint64_t ppn = 0; ppn < planes * dies; ++ppn) {
        const auto major = decode_page_index(AddressLayout::bank_major, config, ppn);
        const auto ocp = decode_page_index(AddressLayout::ocp_v070, config, ppn);
        banks_major.insert(major.plane);
        banks_ocp.insert(ocp.plane);
        dies_ocp.insert(ocp.die);
    }
    require(banks_major.size() == 1, "bank_major should keep a sequential stream on one bank");
    require(banks_ocp.size() == planes, "ocp_v070 should rotate the stream over every bank");
    require(dies_ocp.size() == dies, "ocp_v070 should rotate the stream over every die");
}

// The cache and sense timelines key on global ordinals: pages in different
// channels or dies must never collide on one bank slot.
void global_ordinals_are_unique() {
    const auto config = make_config(AddressLayout::ocp_v070, 2, 3, 2, 4, 2, 8);
    std::vector<bool> banks(2 * 3 * 2 * 4, false);
    std::vector<bool> channels(2 * 3, false);
    for (std::uint64_t ppn = 0; ppn < total_physical_pages(config); ++ppn) {
        const auto decoded = decode_page_index(AddressLayout::ocp_v070, config, ppn);
        banks.at(global_plane_index(config, decoded)) = true;
        channels.at(global_channel_index(config, decoded)) = true;
    }
    for (const auto used : banks) require(used, "a global bank ordinal is unreachable");
    for (const auto used : channels) require(used, "a global channel ordinal is unreachable");
}

} // namespace

int main() {
    try {
        round_trip(make_config(AddressLayout::bank_major, 2, 4, 2, 3, 5, 7));
        round_trip(make_config(AddressLayout::ocp_v070, 2, 4, 2, 3, 5, 7));
        round_trip(make_config(AddressLayout::ocp_v070, 1, 1, 1, 1, 1, 1));
        round_trip(make_config(AddressLayout::bank_major, 1, 1, 4, 16, 2048, 256));
        round_trip(make_config(AddressLayout::ocp_v070, 1, 1, 4, 16, 2048, 256));
        bank_major_matches_history();
        ocp_puts_bank_and_die_in_the_low_bits();
        ocp_matches_section_5_7_algebra();
        global_ordinals_are_unique();
        sequential_stream_spreads_over_banks();
    } catch (const std::exception& error) {
        std::cerr << "hbf_address_layout: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hbf_address_layout: ok\n";
    return 0;
}
