#include "guest-pause-guard.hh"

#include <cassert>

static bool g_running;
static int g_stop_calls;
static int g_start_calls;

bool runstate_is_running(void)
{
    return g_running;
}

int vm_stop(RunState state)
{
    assert(state == RUN_STATE_PAUSED);
    ++g_stop_calls;
    g_running = false;
    return 0;
}

void vm_start(void)
{
    ++g_start_calls;
    g_running = true;
}

static void reset(bool running)
{
    g_running = running;
    g_stop_calls = 0;
    g_start_calls = 0;
}

int main()
{
    reset(true);
    {
        XemuDebugGuestPauseGuard pause;
        assert(!g_running);
        assert(g_stop_calls == 1);
        assert(g_start_calls == 0);
    }
    assert(g_running);
    assert(g_stop_calls == 1);
    assert(g_start_calls == 1);

    reset(false);
    {
        XemuDebugGuestPauseGuard pause;
        assert(!g_running);
        assert(g_stop_calls == 0);
        assert(g_start_calls == 0);
    }
    assert(!g_running);
    assert(g_stop_calls == 0);
    assert(g_start_calls == 0);

    reset(true);
    {
        XemuDebugGuestPauseGuard pause;
        pause.Resume();
        assert(g_running);
        assert(g_start_calls == 1);
        pause.Resume();
        assert(g_start_calls == 1);
    }
    assert(g_start_calls == 1);

    reset(true);
    {
        XemuDebugGuestPauseGuard outer;
        assert(!g_running);
        {
            XemuDebugGuestPauseGuard inner;
            assert(g_stop_calls == 1);
            assert(g_start_calls == 0);
        }
        assert(!g_running);
        assert(g_start_calls == 0);
    }
    assert(g_running);
    assert(g_stop_calls == 1);
    assert(g_start_calls == 1);

    return 0;
}
