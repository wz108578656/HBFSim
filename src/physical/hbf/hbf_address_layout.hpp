#pragma once
#include "physical/hbf/hbf_config.hpp"

#include <cstdint>

namespace hbfsim::physical::hbf {

// One physical page resolved into the fields the model schedules on.
// `plane` is the OCP NAND bank: the simulator names the bank "plane" because
// the device model's geometry follows the JEDEC plane wording.
struct PageCoordinates {
    std::uint32_t stack = 0;
    std::uint32_t channel = 0;
    std::uint32_t die = 0;
    std::uint32_t plane = 0;
    std::uint32_t block = 0;
    std::uint32_t page = 0;

    bool operator==(const PageCoordinates&) const = default;
};

[[nodiscard]] inline std::uint64_t pages_per_plane(const HbfDeviceConfig& config) {
    return static_cast<std::uint64_t>(config.blocks_per_plane) * config.pages_per_block;
}

[[nodiscard]] inline std::uint64_t pages_per_die(const HbfDeviceConfig& config) {
    return pages_per_plane(config) * config.planes_per_die;
}

[[nodiscard]] inline std::uint64_t pages_per_channel(const HbfDeviceConfig& config) {
    return pages_per_die(config) * config.dies_per_channel;
}

[[nodiscard]] inline std::uint64_t pages_per_stack(const HbfDeviceConfig& config) {
    return pages_per_channel(config) * config.channels_per_stack;
}

[[nodiscard]] inline std::uint64_t total_physical_pages(const HbfDeviceConfig& config) {
    return pages_per_stack(config) * config.stacks;
}

// Pages one block spans once every die and bank of a channel is counted. The
// OCP section 5.7 replay algebra (p.63) counts in exactly this unit.
[[nodiscard]] inline std::uint64_t pages_per_channel_block(const HbfDeviceConfig& config) {
    return static_cast<std::uint64_t>(config.pages_per_block) * config.dies_per_channel *
           config.planes_per_die;
}

// Split a physical page index into stack/channel/die/plane/block/page.
// The caller owns range validation; this function is pure arithmetic so the
// device model and the host controller can share one definition of the layout.
[[nodiscard]] inline PageCoordinates decode_page_index(AddressLayout layout,
                                                       const HbfDeviceConfig& config,
                                                       std::uint64_t ppn) {
    PageCoordinates page{};
    if (layout == AddressLayout::ocp_v070) {
        // stack | channel | block | page | die | plane, with the bank in the
        // lowest bits of the block-relative index (p.22 "first 4 bits = bank,
        // next 2 bits = die"; p.63 bank/die = remainder within R1*R2).
        const auto channel_pages = pages_per_channel(config);
        const auto local = ppn % channel_pages;
        const auto outer = ppn / channel_pages;
        page.channel = static_cast<std::uint32_t>(outer % config.channels_per_stack);
        page.stack = static_cast<std::uint32_t>(outer / config.channels_per_stack);

        const auto block_span = pages_per_channel_block(config);
        page.block = static_cast<std::uint32_t>(local / block_span);
        const auto in_block = local % block_span;
        const auto die_bank_span = static_cast<std::uint64_t>(config.dies_per_channel) *
                                   config.planes_per_die;
        page.page = static_cast<std::uint32_t>(in_block / die_bank_span);
        const auto remainder = in_block % die_bank_span;
        page.die = static_cast<std::uint32_t>(remainder / config.planes_per_die);
        page.plane = static_cast<std::uint32_t>(remainder % config.planes_per_die);
        return page;
    }

    // bank_major: stack | channel | die | plane | block | page.
    auto unit = ppn;
    page.page = static_cast<std::uint32_t>(unit % config.pages_per_block);
    unit /= config.pages_per_block;
    page.block = static_cast<std::uint32_t>(unit % config.blocks_per_plane);
    unit /= config.blocks_per_plane;
    page.plane = static_cast<std::uint32_t>(unit % config.planes_per_die);
    unit /= config.planes_per_die;
    page.die = static_cast<std::uint32_t>(unit % config.dies_per_channel);
    unit /= config.dies_per_channel;
    page.channel = static_cast<std::uint32_t>(unit % config.channels_per_stack);
    unit /= config.channels_per_stack;
    page.stack = static_cast<std::uint32_t>(unit);
    return page;
}

// Inverse of decode_page_index. Field ranges are the caller's responsibility.
[[nodiscard]] inline std::uint64_t encode_page_index(AddressLayout layout,
                                                     const HbfDeviceConfig& config,
                                                     const PageCoordinates& page) {
    if (layout == AddressLayout::ocp_v070) {
        std::uint64_t unit = page.stack;
        unit = unit * config.channels_per_stack + page.channel;
        // A channel spans blocks_per_plane channel blocks, so the outer base
        // scales by the whole channel, not by one block.
        unit *= pages_per_channel(config);
        unit += static_cast<std::uint64_t>(page.block) * pages_per_channel_block(config);
        const auto die_bank_span = static_cast<std::uint64_t>(config.dies_per_channel) *
                                   config.planes_per_die;
        unit += static_cast<std::uint64_t>(page.page) * die_bank_span;
        unit += static_cast<std::uint64_t>(page.die) * config.planes_per_die;
        unit += page.plane;
        return unit;
    }

    std::uint64_t unit = page.stack;
    unit = unit * config.channels_per_stack + page.channel;
    unit = unit * config.dies_per_channel + page.die;
    unit = unit * config.planes_per_die + page.plane;
    unit = unit * config.blocks_per_plane + page.block;
    unit = unit * config.pages_per_block + page.page;
    return unit;
}

// Global bank ordinal across every stack, channel, die and plane. This is the
// key the decoded page cache and the per-bank sense timeline use, so it must
// stay global: two pages in different channels must never share a bank slot.
[[nodiscard]] inline std::size_t global_plane_index(const HbfDeviceConfig& config,
                                                    const PageCoordinates& page) {
    return static_cast<std::size_t>(
        ((static_cast<std::uint64_t>(page.stack) * config.channels_per_stack + page.channel) *
             config.dies_per_channel + page.die) * config.planes_per_die + page.plane);
}

// Global channel ordinal across stacks.
[[nodiscard]] inline std::size_t global_channel_index(const HbfDeviceConfig& config,
                                                      const PageCoordinates& page) {
    return static_cast<std::size_t>(static_cast<std::uint64_t>(page.stack) *
                                        config.channels_per_stack + page.channel);
}

[[nodiscard]] inline std::size_t plane_index_of(AddressLayout layout,
                                                const HbfDeviceConfig& config,
                                                std::uint64_t ppn) {
    return decode_page_index(layout, config, ppn).plane;
}

[[nodiscard]] inline std::size_t channel_index_of(AddressLayout layout,
                                                  const HbfDeviceConfig& config,
                                                  std::uint64_t ppn) {
    return decode_page_index(layout, config, ppn).channel;
}

} // namespace hbfsim::physical::hbf
