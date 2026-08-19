//
// xemu Debug Tools - scoped guest pause helper
//
// Keeps short debugger/cheat transactions symmetric across every early return:
// a running VM is paused on construction and resumed exactly once on release or
// destruction. A VM that was already paused/stopped is left untouched.
//
#pragma once

#include "qemu/osdep.h"

extern "C" {
#include "system/runstate.h"
}

class XemuDebugGuestPauseGuard
{
public:
    XemuDebugGuestPauseGuard()
        : m_resume(runstate_is_running())
    {
        if (m_resume) {
            vm_stop(RUN_STATE_PAUSED);
        }
    }

    ~XemuDebugGuestPauseGuard()
    {
        Resume();
    }

    void Resume()
    {
        if (m_resume) {
            vm_start();
            m_resume = false;
        }
    }

    XemuDebugGuestPauseGuard(const XemuDebugGuestPauseGuard &) = delete;
    XemuDebugGuestPauseGuard &operator=(const XemuDebugGuestPauseGuard &) = delete;

private:
    bool m_resume;
};
