# xemu RAW Cheat Engine / Memory / Debugger extensions.
#
# Keep custom tooling isolated from the upstream xui source list so future
# xemu updates only need a small integration hook in ui/xui/meson.build.
xemu_ss.add(files(
  'cheat-engine.cc',
  'breakpoint-conditions.cc',
  'x86-cheat-assembler.cc',
  'current-game.cc',
  'hdd-directory.cc',
  'fatx-hdd.cc',
  'xbe-labels.cc',
  'label-packs.cc',
  'xdk-labels.cc',
  'map-labels.cc',
  'pdb-labels.cc',
  'disc-block-io.c',
  'xdvdfs-disc.cc',
  'detached-tools.cc',
  'memory-tools.cc',
  'memory-tools-memory.cc',
  'memory-tools-search.cc',
  'memory-tools-debugger.cc',
  'memory-tools-inject.cc',
  'memory-tools-labels.cc',
  'memory-tools-dump.cc',
  'external-code-memory.c',
))

# Josh-style native breakpoint/watchpoint backend. This is target-specific
# C code so it can use QEMU's i386 CPU/debug structures without exposing
# target headers to the C++ ImGui frontend.
specific_ss.add(files(
  'backend/xemu-dbg.c',
))
specific_ss.add(when: 'CONFIG_WHPX', if_true: files(
  'backend/whpx-debug.c',
))

# The debugger bridge uses Capstone directly when CONFIG_CAPSTONE is enabled.
# Add the dependency to this translation unit so Meson propagates Capstone's
# pkg-config include path (notably .../include/capstone for <capstone.h>) and
# link flags. Keep the source buildable when Capstone is disabled.
if capstone.found()
  xemu_ss.add([files('cheat-engine-memory.c'), capstone])
else
  xemu_ss.add(files('cheat-engine-memory.c'))
endif
