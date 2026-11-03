// Block read-disturb accounting: a physical block counts the senses it induced,
// the counter restarts with the erase that removes the disturbed pages, and the
// threshold predicate is the OCP p59-61/p118 block-health signal. A zero
// threshold must leave the model exactly as it was before refresh existed.
#include "physical/hbf/hbf_address_layout.hpp"
#include "physical/hbf/hbf_device.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace hbfsim::physical::hbf;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

HbfDeviceConfig make_config(AddressLayout layout, std::uint64_t threshold) {
    HbfDeviceConfig config;
    config.address_layout = layout;
    config.stacks = 1;
    config.channels_per_stack = 1;
    config.dies_per_channel = 1;
    config.planes_per_die = 2;
    config.blocks_per_plane = 4;
    config.pages_per_block = 8;
    config.read_count_threshold = threshold;
    return config;
}

// Page index of `page` inside the physical block identified by a global block
// slot, in whatever order the chosen layout places it.
std::uint64_t page_of_block(const HbfDeviceConfig& config, std::size_t block_slot,
                            std::uint32_t page) {
    const auto per_plane = static_cast<std::size_t>(config.blocks_per_plane);
    PageCoordinates coordinates{};
    coordinates.block = static_cast<std::uint32_t>(block_slot % per_plane);
    auto rest = block_slot / per_plane;
    coordinates.plane = static_cast<std::uint32_t>(rest % config.planes_per_die);
    rest /= config.planes_per_die;
    coordinates.die = static_cast<std::uint32_t>(rest % config.dies_per_channel);
    rest /= config.dies_per_channel;
    coordinates.channel = static_cast<std::uint32_t>(rest % config.channels_per_stack);
    rest /= config.channels_per_stack;
    coordinates.stack = static_cast<std::uint32_t>(rest);
    coordinates.page = page;
    return encode_page_index(config.address_layout, config, coordinates);
}

// Every sense of a block raises its counter by one, across both layouts.
void senses_accumulate_per_block(AddressLayout layout) {
    const auto config = make_config(layout, 3);
    HbfDevice device(config);
    for (std::uint64_t block = 0; block < 8; ++block)
        device.seed_block(block, config.pages_per_block);

    const auto first = page_of_block(config, 0, 0);
    for (int sense = 0; sense < 3; ++sense) device.record_page_read(first);
    require(device.block_read_count(0) == 3, "three senses did not count three times");
    require(!device.block_read_count_exceeds_threshold(0),
            "the threshold must not trip while the count equals it");

    device.record_page_read(first);
    require(device.block_read_count_exceeds_threshold(0),
            "the threshold must trip once the count exceeds it");
    require(device.block_read_count(1) == 0, "an untouched block must stay at zero");
}

// A block's other pages raise the same counter: disturb is per block, not per page.
void reads_on_any_page_of_the_block_count(AddressLayout layout) {
    const auto config = make_config(layout, 0);
    HbfDevice device(config);
    for (std::uint64_t block = 0; block < 8; ++block)
        device.seed_block(block, config.pages_per_block);

    for (std::uint32_t page = 0; page < config.pages_per_block; ++page)
        device.record_page_read(page_of_block(config, 5, page));
    require(device.block_read_count(5) == config.pages_per_block,
            "pages of one block must share its counter");
    require(device.block_read_count(4) == 0, "a neighbouring block received the count");
}

// Erasing the block is what clears the count, exactly like the P/E cycle.
void erase_restarts_the_counter(AddressLayout layout) {
    const auto config = make_config(layout, 0);
    HbfDevice device(config);
    for (std::uint64_t block = 0; block < 8; ++block)
        device.seed_block(block, config.pages_per_block);

    const auto page = page_of_block(config, 2, 1);
    for (int sense = 0; sense < 5; ++sense) device.record_page_read(page);
    require(device.block_read_count(2) == 5, "setup senses were not counted");

    device.erase(2);
    device.record_erase(2);
    require(device.block_read_count(2) == 0, "erase must clear the read-disturb count");
    require(device.block_read_count_exceeds_threshold(2) == false, "cleared block still hot");
}

// A zero threshold is the pre-refresh model: the counter still exists but never
// reports block health, so existing scenarios cannot change.
void zero_threshold_disables_the_signal(AddressLayout layout) {
    const auto config = make_config(layout, 0);
    HbfDevice device(config);
    for (std::uint64_t block = 0; block < 8; ++block)
        device.seed_block(block, config.pages_per_block);
    const auto page = page_of_block(config, 0, 0);
    for (int sense = 0; sense < 1000; ++sense) device.record_page_read(page);
    require(device.block_read_count(0) == 1000, "the counter must still run when disabled");
    require(!device.block_read_count_exceeds_threshold(0),
            "a zero threshold must never report a hot block");
}

} // namespace

int main() {
    try {
        for (const auto layout : {AddressLayout::bank_major, AddressLayout::ocp_v070}) {
            senses_accumulate_per_block(layout);
            reads_on_any_page_of_the_block_count(layout);
            erase_restarts_the_counter(layout);
            zero_threshold_disables_the_signal(layout);
        }
    } catch (const std::exception& error) {
        std::cerr << "hbf_read_disturb: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hbf_read_disturb: ok\n";
    return 0;
}
