# xemu Debug Tools modular additions.
#
# Upstream xemu hooks only debug-tools.cc/debug-tools.hh. Current Game + RAW
# Cheat Engine are the main addition. HDD Directory and Memory Viewer/Search are
# optional additions that register through the Debug Tools module API.
#
# Select locally in build-profile.txt:
#   main        = Current Game + Cheat Engine only
#   main+hdd    = main + HDD Directory/Kernel RPC
#   main+memory = main + Memory Viewer/Search/x86 Debugger
#   full        = all additions (default, v2.90.6-equivalent feature set)
profile_reader = join_paths(meson.current_source_dir(), 'read-build-profile.py')
profile_file = join_paths(meson.current_source_dir(), 'build-profile.txt')
profile_result = run_command(python, profile_reader, profile_file, check: true)
debug_tools_profile = profile_result.stdout().strip()

debug_tools_with_hdd = (debug_tools_profile == 'full' or
                        debug_tools_profile == 'main+hdd')
debug_tools_with_memory = (debug_tools_profile == 'full' or
                           debug_tools_profile == 'main+memory')

message('xemu Debug Tools profile: ' + debug_tools_profile)

# Add-on implementation files live below addons/, but deliberately keep their
# historical include spelling. Provide the Debug Tools root as an include path
# so moving a module does not churn its implementation or behavior fingerprints.
debug_tools_root_include = declare_dependency(
  include_directories: include_directories('.'),
)
xemu_ss.add(debug_tools_root_include)

# Main addition + shared services. This is the distributable Current Game /
# Cheat Engine package and is always present for every Debug Tools profile.
xemu_ss.add(files(
  'debug-tools.cc',
  'detached-tools.cc',
  'cheat-engine.cc',
  'cheat-engine-source.cc',
  'cheat-engine-fhooks.cc',
  'cheat-engine-execute.cc',
  'cheat-engine-ui.cc',
  'guest-pause-guard.cc',
  'current-game.cc',
  'current-game-ui.cc',
  'xbe-labels.cc',
  'label-packs.cc',
  'xdk-labels.cc',
  'map-labels.cc',
  'pdb-labels.cc',
  'disc-block-io.c',
  'xdvdfs-disc.cc',
  'xdvdfs-export-service.cc',
  'external-code-memory.c',
))

# Optional Addition: HDD Directory / FATX / guest Kernel RPC. Nothing in normal
# xemu source includes these headers; the addition registers with debug-tools.cc.
if debug_tools_with_hdd
  xemu_ss.add(files(
    'addons/hdd/debug-tools-hdd-addon.cc',
    'addons/hdd/hdd-directory.cc',
    'addons/hdd/hdd-directory-ui.cc',
    'addons/hdd/hdd-export-service.cc',
    'addons/hdd/hdd-snapshot-service.cc',
    'addons/hdd/fatx-hdd.cc',
    'addons/hdd/guest-kernel-rpc.cc',
    'addons/hdd/guest-kernel-rpc-ui.cc',
    'addons/hdd/guest-kernel-rpc-completion.cc',
    'addons/hdd/guest-kernel-rpc-filesystem.cc',
    'addons/hdd/kernel-rpc-filesystem.cc',
    'addons/hdd/kernel-rpc-filesystem-stream.cc',
    'addons/hdd/guest-kernel-rpc-memory.c',
  ))
else
  xemu_ss.add(files('addons/stubs/debug-tools-hdd-addon-stub.cc'))
endif

# Optional Addition: Memory Viewer / Search / x86 Debugger / Inject / RAM Dump.
# It consumes Current Game, Cheat Engine memory access, Capstone and the shared
# assembler interfaces but adds no hook to upstream xemu files.
if debug_tools_with_memory
  xemu_ss.add(files(
    'addons/memory-tools/debug-tools-memory-tools-addon.cc',
    'addons/memory-tools/breakpoint-conditions.cc',
    'addons/memory-tools/memory-tools.cc',
    'addons/memory-tools/memory-tools-memory.cc',
    'addons/memory-tools/memory-tools-memory-ui.cc',
    'addons/memory-tools/memory-tools-search.cc',
    'addons/memory-tools/memory-tools-search-ui.cc',
    'addons/memory-tools/memory-tools-debugger.cc',
    'addons/memory-tools/memory-tools-debugger-ui.cc',
    'addons/memory-tools/memory-tools-inject.cc',
    'addons/memory-tools/memory-tools-inject-ui.cc',
    'addons/memory-tools/memory-tools-labels.cc',
    'addons/memory-tools/memory-tools-labels-ui.cc',
    'addons/memory-tools/memory-tools-dump.cc',
    'addons/memory-tools/memory-tools-dump-ui.cc',
  ))
else
  xemu_ss.add(files('addons/stubs/debug-tools-memory-tools-addon-stub.cc'))
endif

# F0 and debugger Change use Keystone as the only generic IA-32 text encoder.
# The main Current Game/Cheat Engine addition itself requires F0, so Keystone is
# intentionally part of the shared/main layer rather than the Memory addition.
keystone = dependency('keystone', required: true, static: true)
xemu_ss.add([
  files('x86-cheat-assembler.cc', 'x86-cheat-assembler-keystone.cc'),
  keystone,
])

# Josh-style native breakpoint/watchpoint backend. The memory-access bridge is
# shared by Cheat Engine/F0 and the optional debugger, so the backend remains in
# the main/shared layer even when the Memory Viewer addition is excluded.
specific_ss.add(files(
  'backend/xemu-dbg.c',
))
specific_ss.add(when: 'CONFIG_WHPX', if_true: files(
  'backend/whpx-debug.c',
))

# The shared memory bridge uses Capstone directly when CONFIG_CAPSTONE is
# enabled (F0 hook-span/decode support plus the optional debugger frontend).
if capstone.found()
  xemu_ss.add([files('cheat-engine-memory.c'), capstone])
else
  xemu_ss.add(files('cheat-engine-memory.c'))
endif
