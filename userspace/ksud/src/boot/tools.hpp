#pragma once

#include <string>

namespace ksud {

// Resolve the current executable, which embeds the magiskboot applet.
std::string find_magiskboot();

// Simple DD command wrapper
bool exec_dd(const std::string& input, const std::string& output);

// Clear the read-only flag on a block device, the one thing `blockdev --setrw`
// does. Returns false if the device could not be opened or the ioctl was
// refused; callers treat that as advisory, since many partitions are already
// writable and reject it.
bool set_block_device_rw(const std::string& device);

}  // namespace ksud
