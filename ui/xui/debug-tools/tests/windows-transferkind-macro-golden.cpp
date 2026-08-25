// v2.87 current regression ownership.
// Windows macro-collision guard for XemuKernelFs::TransferKind.
// windows.h maps CreateDirectory to CreateDirectoryA/W. The public filesystem
// header must remain identical whether that host macro is active or not.
#define CreateDirectory CreateDirectoryW
#include "addons/hdd/kernel-rpc-filesystem.hh"
#undef CreateDirectory

#include <type_traits>

static_assert(std::is_enum<XemuKernelFs::TransferKind>::value, "TransferKind must remain an enum");
static_assert(static_cast<int>(XemuKernelFs::TransferKind::HostImport) == 0, "HostImport ordinal changed");
static_assert(static_cast<int>(XemuKernelFs::TransferKind::CreateFatxDirectory) == 1, "directory ordinal changed");
static_assert(static_cast<int>(XemuKernelFs::TransferKind::FatxCopy) == 2, "FatxCopy ordinal changed");
static_assert(static_cast<int>(XemuKernelFs::TransferKind::CrossVolumeMove) == 3, "CrossVolumeMove ordinal changed");

int main()
{
    XemuKernelFs::TransferPlan plan;
    plan.kind = XemuKernelFs::TransferKind::CreateFatxDirectory;
    return plan.kind == XemuKernelFs::TransferKind::CreateFatxDirectory ? 0 : 1;
}
