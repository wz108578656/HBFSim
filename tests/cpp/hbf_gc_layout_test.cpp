// Garbage collection and wear-leveling relocate a victim block by walking its
// page slots. Under an interleaved address layout those pages are not adjacent,
// so the relocation source page must be encoded rather than computed as
// block * pages_per_block + page. This test drives enough writes to reclaim a
// block through the relocation path in both layouts.
#include "../../verification/probes/hbf_with_hbm.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace hbfsim::host;
using namespace hbfsim::physical;
using hbfsim::physical::hbf::AddressLayout;
using Device = hbfsim::verification::HbfWithHbm;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

HbfConfig config(AddressLayout layout) {
    HbfConfig c;
    c.device.address_layout = layout;
    c.device.stacks = 1;
    c.device.channels_per_stack = 1;
    c.device.dies_per_channel = 1;
    c.device.planes_per_die = 2;
    c.device.blocks_per_plane = 8;
    c.device.pages_per_block = 8;
    c.device.t_read_page_ns = 40;
    c.device.t_program_page_ns = 100;
    c.device.t_erase_block_ns = 1000;
    c.host.mapping_mode = MappingMode::FullResident;
    // 48 logical pages live over 128 physical pages: writing the capacity
    // several times forces GC to relocate a victim block.
    c.host.logical_capacity_bytes = 48 * 4096;
    c.host.gc_reserved_free_blocks_per_plane = 2;
    c.host.gc_low_watermark_pages = 8;
    c.host.write_coalescing_enabled = false;
    c.host.write_buffer_pages = 0;
    return c;
}

void write(Device& device, std::uint64_t lpn, double now) {
    const auto completion = device.issue(PhysicalRequest{.id = "gc-layout",
        .tier = Tier::HBF, .op = Op::Write, .address_space = AddressSpace::Logical,
        .arrival_ns = now, .addr = lpn * 4096, .bytes = 4096});
    check(completion.finish_ns > 0.0, "write did not complete");
}

void read(Device& device, std::uint64_t lpn, double now) {
    const auto completion = device.issue(PhysicalRequest{.id = "gc-layout",
        .tier = Tier::HBF, .op = Op::Read, .address_space = AddressSpace::Logical,
        .arrival_ns = now, .addr = lpn * 4096, .bytes = 4096});
    check(completion.finish_ns > now, "read did not complete after its arrival");
}

void reclamation_relocates_under(AddressLayout layout) {
    Device device(config(layout));
    double now = 0.0;
    // Three times the logical capacity, so the allocator must reclaim blocks
    // and relocate the live pages of each victim.
    for (std::uint64_t round = 0; round < 3; ++round) {
        for (std::uint64_t lpn = 0; lpn < 48; ++lpn) {
            write(device, lpn, now);
            now += 1'000.0;
        }
    }
    for (std::uint64_t lpn = 0; lpn < 48; ++lpn) {
        read(device, lpn, now);
        now += 1'000.0;
    }
    check(device.stats().block_erases > 0, "the workload never reclaimed a block");
    check(device.stats().gc_relocations > 0 || device.stats().gc_reclaimed_invalid_pages > 0,
          "reclamation never relocated or reclaimed a page");
}

} // namespace

int main() {
    try {
        for (const auto layout : {AddressLayout::bank_major, AddressLayout::ocp_v070})
            reclamation_relocates_under(layout);
    } catch (const std::exception& error) {
        std::cerr << "hbf_gc_layout: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hbf_gc_layout: ok\n";
    return 0;
}
