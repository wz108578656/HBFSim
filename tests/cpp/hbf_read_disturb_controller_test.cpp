// The controller must count one disturb event per page sense, not per host
// request: a 4 KiB read, the 64 B reads that share its sense, and every cached
// re-read of the same page all answer from one array access.
#include "../../verification/probes/hbf_with_hbm.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace hbfsim::host;
using namespace hbfsim::physical;
using Device = hbfsim::verification::HbfWithHbm;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

HbfConfig config(std::uint64_t threshold) {
    HbfConfig c;
    c.device.stacks = 1;
    c.device.channels_per_stack = 1;
    c.device.dies_per_channel = 1;
    c.device.planes_per_die = 2;
    c.device.blocks_per_plane = 16;
    c.device.pages_per_block = 8;
    c.device.t_read_page_ns = 40;
    c.device.t_program_page_ns = 200;
    c.device.t_erase_block_ns = 2000;
    c.device.read_count_threshold = threshold;
    c.host.mapping_mode = MappingMode::FullResident;
    c.host.logical_capacity_bytes = 32 * 4096;
    c.host.gc_reserved_free_blocks_per_plane = 2;
    c.host.write_buffer_pages = 0;
    c.host.write_coalescing_enabled = false;
    return c;
}

double issue(Device& device, Op op, std::uint64_t lpn, double now,
             std::uint64_t bytes = 4096, std::uint64_t offset = 0) {
    return device.issue(PhysicalRequest{.id = "read-disturb", .tier = Tier::HBF, .op = op,
        .address_space = AddressSpace::Logical, .arrival_ns = now,
        .addr = lpn * 4096 + offset, .bytes = bytes}).finish_ns;
}

// Senses recorded by the device, summed over every block. The test does not
// assume where the FTL placed the page.
std::uint64_t senses(const Device& device) {
    const auto& media = device.media_device();
    std::uint64_t total = 0;
    for (std::uint64_t block = 0; block < 16 * 2; ++block)
        total += media.block_read_count(block);
    return total;
}

std::uint64_t hot_blocks(const Device& device) {
    const auto& media = device.media_device();
    std::uint64_t total = 0;
    for (std::uint64_t block = 0; block < 16 * 2; ++block)
        if (media.block_read_count_exceeds_threshold(block)) ++total;
    return total;
}

void repeated_reads_share_one_sense() {
    Device device(config(0));
    issue(device, Op::Write, 0, 0.0);
    check(senses(device) == 0, "a program must not count as a read");

    issue(device, Op::Read, 0, 10'000.0);
    check(senses(device) == 1, "the first read of a page must sense once");

    for (int repeat = 0; repeat < 10; ++repeat)
        issue(device, Op::Read, 0, 20'000.0 + repeat * 1'000.0);
    check(senses(device) == 1, "cached re-reads must not count as disturb");

    // 64 B fragments of the same page are answered from the decoded page: the
    // array is touched once however many fragments the host issues.
    for (std::uint64_t offset = 0; offset < 4096; offset += 64)
        issue(device, Op::Read, 0, 40'000.0 + offset, 64, offset);
    check(senses(device) == 1, "64 B fragments of a cached page must not add senses");
}

void a_second_page_of_the_same_block_adds_one() {
    Device device(config(0));
    for (std::uint64_t lpn = 0; lpn < 2; ++lpn) issue(device, Op::Write, lpn, 0.0);
    issue(device, Op::Read, 0, 10'000.0);
    const auto after_first = senses(device);
    issue(device, Op::Read, 1, 20'000.0);
    check(senses(device) == after_first + 1, "a distinct page must sense once more");
}

void threshold_reports_health_only_when_configured() {
    Device disabled(config(0));
    issue(disabled, Op::Write, 0, 0.0);
    for (int repeat = 0; repeat < 8; ++repeat)
        issue(disabled, Op::Read, 0, 10'000.0 + repeat * 1'000.0);
    check(hot_blocks(disabled) == 0, "a zero threshold must never report a hot block");

    Device enabled(config(1));
    for (std::uint64_t lpn = 0; lpn < 4; ++lpn) issue(enabled, Op::Write, lpn, 0.0);
    for (std::uint64_t lpn = 0; lpn < 4; ++lpn)
        issue(enabled, Op::Read, lpn, 10'000.0 + lpn * 1'000.0);
    check(hot_blocks(enabled) > 0, "blocks above the threshold must report as hot");
}

} // namespace

int main() {
    try {
        repeated_reads_share_one_sense();
        a_second_page_of_the_same_block_adds_one();
        threshold_reports_health_only_when_configured();
    } catch (const std::exception& error) {
        std::cerr << "hbf_read_disturb_controller: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hbf_read_disturb_controller: ok\n";
    return 0;
}
