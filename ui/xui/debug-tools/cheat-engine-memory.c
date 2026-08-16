//
// xemu RAW Cheat Engine - QEMU memory bridge
//
// Keep QEMU C headers in a C translation unit. Some QEMU headers rely on
// GNU C constructs and identifiers that are not valid when parsed as C++.
//

#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "cheat-engine-memory.h"
#include "backend/xemu-dbg.h"

#include "hw/core/cpu.h"
#include "exec/target_page.h"
#include "hw/boards.h"
#include "qemu/cutils.h"
#include "system/address-spaces.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "exec/gdbstub.h"

/* xemu/QEMU uses Capstone for x86 monitor disassembly. Keep the optional
 * decoder API entirely on this C side of the bridge so none of those
 * headers leak into the ImGui C++ frontend. */
#include "disas/capstone.h"

int xemu_cheat_cpu_available(void)
{
    return qemu_get_cpu(0) != NULL;
}

int xemu_cheat_memory_read(int is_virtual, uint32_t address,
                           void *buffer, size_t size)
{
    if (is_virtual) {
        CPUState *cpu = qemu_get_cpu(0);
        if (cpu == NULL) {
            return 0;
        }
        return cpu_memory_rw_debug(cpu, (vaddr)address,
                                   buffer, size, false) == 0;
    }

    return address_space_read(&address_space_memory,
                              (hwaddr)address,
                              MEMTXATTRS_UNSPECIFIED,
                              buffer, size) == MEMTX_OK;
}

int xemu_cheat_memory_write(int is_virtual, uint32_t address,
                            const void *buffer, size_t size)
{
    if (is_virtual) {
        CPUState *cpu = qemu_get_cpu(0);
        if (cpu == NULL) {
            return 0;
        }
        return cpu_memory_rw_debug(cpu, (vaddr)address,
                                   (void *)buffer, size, true) == 0;
    }

    return address_space_write(&address_space_memory,
                               (hwaddr)address,
                               MEMTXATTRS_UNSPECIFIED,
                               buffer, size) == MEMTX_OK;
}

int xemu_cheat_patch_virtual(uint32_t address, const void *buffer, size_t size)
{
    int ok;
    const bool was_running = runstate_is_running();

    if (buffer == NULL || size == 0) {
        return 0;
    }
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }
    ok = xemu_cheat_memory_write(1, address, buffer, size);
    if (was_running) {
        vm_start();
    }
    return ok;
}

uint64_t xemu_cheat_ram_size(void)
{
    if (current_machine == NULL) {
        return 0;
    }
    return (uint64_t)current_machine->ram_size;
}

int xemu_cheat_prepare_virtual_map(void)
{
    CPUState *cpu = qemu_get_cpu(0);
    if (cpu == NULL) {
        return 0;
    }

    /* Match xemu-xbe.c's virtual-to-physical path. This is important for
     * hardware accelerators such as WHPX, where the architectural CPU state
     * may need to be synchronized before CR3/page-table inspection. */
    cpu_synchronize_state(cpu);
    return 1;
}

static int xemu_cheat_virtual_to_physical_cpu(
    CPUState *cpu, uint32_t address, uint64_t *physical_address)
{
    MemTxAttrs attrs;
    vaddr page;
    hwaddr physical;

    if (cpu == NULL || physical_address == NULL) {
        return 0;
    }

    /* cpu_get_phys_page_attrs_debug() returns the physical address of the
     * translated target page, not the exact byte address. Keep the 4 KiB
     * offset so debugger/breakpoint translations such as V 00579003 report
     * P 0087C003 instead of only P 0087C000. */
    page = (vaddr)address & TARGET_PAGE_MASK;
    physical = cpu_get_phys_page_attrs_debug(cpu, page, &attrs);
    if (physical == (hwaddr)-1) {
        return 0;
    }

    physical += (hwaddr)((vaddr)address - page);
    *physical_address = (uint64_t)physical;
    return 1;
}

int xemu_cheat_virtual_to_physical(uint32_t address, uint64_t *physical_address)
{
    return xemu_cheat_virtual_to_physical_cpu(qemu_get_cpu(0), address,
                                               physical_address);
}

static int xemu_cheat_find_gdb_register(const GArray *register_list,
                                        const char *name)
{
    guint i;

    if (register_list == NULL || name == NULL) {
        return -1;
    }

    for (i = 0; i < register_list->len; ++i) {
        const GDBRegDesc *desc = &g_array_index(register_list, GDBRegDesc, i);
        if (desc->name != NULL && g_ascii_strcasecmp(desc->name, name) == 0) {
            return desc->gdb_reg;
        }
    }
    return -1;
}

static int xemu_cheat_read_gdb_u32(CPUState *cpu,
                                    const GArray *register_list,
                                    GByteArray *bytes, const char *name,
                                    uint32_t *value)
{
    const int register_number =
        xemu_cheat_find_gdb_register(register_list, name);
    int read_size;

    if (cpu == NULL || bytes == NULL || value == NULL ||
        register_number < 0) {
        return 0;
    }

    /* gdb_read_register() appends to the supplied array. Reuse one byte
     * buffer for the complete register snapshot instead of allocating a new
     * GByteArray for every individual register. */
    g_byte_array_set_size(bytes, 0);
    read_size = gdb_read_register(cpu, bytes, register_number);
    if (read_size < 4 || bytes->len < 4) {
        return 0;
    }

    /* The Xbox target is IA-32/little-endian. gdb_read_register() returns
     * target-order bytes, so decode the low 32 bits explicitly instead of
     * depending on host endianness. This also remains safe if the enclosing
     * QEMU build happens to expose a wider register representation. */
    *value = ((uint32_t)bytes->data[0]) |
             ((uint32_t)bytes->data[1] << 8) |
             ((uint32_t)bytes->data[2] << 16) |
             ((uint32_t)bytes->data[3] << 24);
    return 1;
}

int xemu_cheat_set_x86_register(const char *name, uint32_t value)
{
    CPUState *cpu = qemu_get_cpu(0);
    GArray *register_list;
    int register_number;
    int write_size;
    uint8_t bytes[4];

    if (cpu == NULL || name == NULL || name[0] == '\0' ||
        runstate_is_running()) {
        return 0;
    }

    /* Pull accelerator state into CPUState first. WHPX marks vcpu_dirty while
     * synchronized, so a successful GDB-register write below is pushed back to
     * the virtual processor automatically before execution resumes. */
    cpu_synchronize_state(cpu);

    register_list = gdb_get_register_list(cpu);
    if (register_list == NULL) {
        return 0;
    }
    register_number = xemu_cheat_find_gdb_register(register_list, name);
    g_array_free(register_list, true);

    if (register_number < 0) {
        return 0;
    }

    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);

    write_size = gdb_write_register(cpu, bytes, register_number);
    return write_size >= 4;
}

int xemu_cheat_get_x86_registers(XemuCheatX86Registers *registers)
{
    CPUState *cpu = qemu_get_cpu(0);
    GArray *register_list;
    GByteArray *bytes;
    uint32_t pc;
    int ok;

    if (cpu == NULL || registers == NULL) {
        return 0;
    }

    /* Keep accelerator-backed architectural state current before asking the
     * generic GDB register bridge to read it. Crucially, this file no longer
     * includes target/i386/cpu.h; target-specific register access stays in
     * QEMU's per-target gdbstub implementation. */
    cpu_synchronize_state(cpu);
    memset(registers, 0, sizeof(*registers));

    /* The register descriptor set is static for an initialized CPU. Build it
     * once for this complete snapshot and reuse one append buffer for all
     * values, rather than rebuilding both objects for each register. */
    register_list = gdb_get_register_list(cpu);
    if (register_list == NULL) {
        return 0;
    }
    bytes = g_byte_array_new();
    if (bytes == NULL) {
        g_array_free(register_list, true);
        return 0;
    }

    ok = xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "eax",
                                 &registers->eax) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "ebx",
                                 &registers->ebx) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "ecx",
                                 &registers->ecx) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "edx",
                                 &registers->edx) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "esi",
                                 &registers->esi) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "edi",
                                 &registers->edi) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "esp",
                                 &registers->esp) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "ebp",
                                 &registers->ebp) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "eip",
                                 &registers->eip) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "eflags",
                                 &registers->eflags) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "cs",
                                 &registers->cs) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "ds",
                                 &registers->ds) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "es",
                                 &registers->es) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "fs",
                                 &registers->fs) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "gs",
                                 &registers->gs) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "ss",
                                 &registers->ss) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "cr0",
                                 &registers->cr0) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "cr2",
                                 &registers->cr2) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "cr3",
                                 &registers->cr3) &&
         xemu_cheat_read_gdb_u32(cpu, register_list, bytes, "cr4",
                                 &registers->cr4);

    g_byte_array_free(bytes, true);
    g_array_free(register_list, true);
    if (!ok) {
        return 0;
    }

    /* QEMU's generic CPUClass PC callback already returns linear PC
     * (CS.base + EIP) for i386, without requiring target/i386/cpu.h here. */
    if (cpu->cc != NULL && cpu->cc->get_pc != NULL) {
        pc = (uint32_t)cpu->cc->get_pc(cpu);
    } else {
        pc = registers->eip;
    }
    registers->pc = pc;
    return 1;
}

int xemu_cheat_get_x86_extra_registers(XemuCheatX86ExtraRegisters *registers)
{
    if (registers == NULL) {
        return 0;
    }
    memset(registers, 0, sizeof(*registers));
    return xemu_dbg_get_extra_registers(registers->st_low, registers->st_high,
                                        registers->mmx, registers->xmm,
                                        &registers->fctrl, &registers->fstat,
                                        &registers->fop, &registers->mxcsr,
                                        &registers->fp_top);
}

/* Breakpoint/watchpoint operations are delegated to the target-specific
 * Josh-style debugger backend. Keep this file as the plain C bridge consumed
 * by the C++ Memory Tools UI. */
int xemu_cheat_breakpoint_insert(uint32_t address)
{
    return xemu_dbg_bp_insert(address);
}

int xemu_cheat_breakpoint_remove(uint32_t address)
{
    return xemu_dbg_bp_remove(address);
}

int xemu_cheat_guest_debug_supported(void)
{
    return xemu_dbg_guest_debug_supported();
}

int xemu_cheat_watchpoint_supported(void)
{
    return xemu_dbg_wp_supported();
}

int xemu_cheat_watchpoint_access_supported(int access_flags)
{
    return xemu_dbg_wp_access_supported(access_flags);
}

int xemu_cheat_watchpoint_insert(uint32_t address, uint32_t length,
                                 int access_flags)
{
    return xemu_dbg_wp_insert(address, length, access_flags);
}

int xemu_cheat_watchpoint_remove(uint32_t address, uint32_t length,
                                 int access_flags)
{
    return xemu_dbg_wp_remove(address, length, access_flags);
}

int xemu_cheat_watchpoint_get_hit(XemuCheatWatchpointHit *hit)
{
    XemuDbgWatchpointHit backend_hit;
    int result;

    if (hit == NULL) {
        return XEMU_CHEAT_WATCH_HIT_NONE;
    }

    result = xemu_dbg_wp_get_hit(&backend_hit);
    if (result != XEMU_DBG_WATCH_HIT_REPORTED) {
        return result;
    }

    hit->watch_address = backend_hit.watch_address;
    hit->hit_address = backend_hit.hit_address;
    hit->length = backend_hit.length;
    hit->access_flags = backend_hit.access_flags;
    return XEMU_CHEAT_WATCH_HIT_REPORTED;
}

int xemu_cheat_debug_backend(void)
{
    const char *name;

    /* current_accel_name() assumes accelerator initialization is complete. */
    if (qemu_get_cpu(0) == NULL) {
        return XEMU_CHEAT_DEBUG_BACKEND_OTHER;
    }
    name = current_accel_name();
    if (name == NULL) {
        return XEMU_CHEAT_DEBUG_BACKEND_OTHER;
    }
    if (strcmp(name, "whpx") == 0) {
        return XEMU_CHEAT_DEBUG_BACKEND_WHPX;
    }
    if (strcmp(name, "tcg") == 0) {
        return XEMU_CHEAT_DEBUG_BACKEND_TCG;
    }
    if (strcmp(name, "kvm") == 0) {
        return XEMU_CHEAT_DEBUG_BACKEND_KVM;
    }
    if (strcmp(name, "hvf") == 0) {
        return XEMU_CHEAT_DEBUG_BACKEND_HVF;
    }
    return XEMU_CHEAT_DEBUG_BACKEND_OTHER;
}

int xemu_cheat_single_step(int enabled)
{
    CPUState *cpu = qemu_get_cpu(0);

    if (cpu == NULL || (enabled && !xemu_dbg_guest_debug_supported())) {
        return 0;
    }

    /* Explicit single-step is used for user Step Into on every backend.
     * TCG also uses it internally for ContinuePastBreakpoint because TCG
     * intentionally lets single-step override a breakpoint at the current PC.
     * WHPX Resume does not use this helper; WHPX performs its own native
     * breakpoint step-over in whpx_vcpu_run(). */
    cpu_single_step(cpu, enabled ? (SSTEP_ENABLE | SSTEP_NOIRQ |
                                   SSTEP_NOTIMER) : 0);
    qemu_cpu_kick(cpu);
    return 1;
}

int xemu_cheat_start_single_step(void)
{
    CPUState *cpu = qemu_get_cpu(0);

    if (cpu == NULL || !xemu_dbg_guest_debug_supported()) {
        return 0;
    }

    /* WHPX must be told before the vCPU resumes that a debugger step is
     * pending. vm_start() always passes step_pending=false, which leaves the
     * WHPX debug-trap exception intercept disabled when no other breakpoint
     * or watchpoint requires it. Mirror the QEMU gdbstub single-step startup:
     * prepare the VM with step_pending=true, arm CPU single-step, then resume. */
    if (vm_prepare_start(true) != 0) {
        return 0;
    }

    cpu_single_step(cpu, SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    resume_all_vcpus();
    qemu_cpu_kick(cpu);
    return 1;
}

int xemu_cheat_disassembler_available(void)
{
#ifdef CONFIG_CAPSTONE
    return 1;
#else
    return 0;
#endif
}

int xemu_cheat_disassemble_paired(uint32_t address, int instruction_count,
                                  XemuCheatDisasmRow *rows, size_t row_capacity,
                                  size_t *row_count)
{
    CPUState *cpu = qemu_get_cpu(0);

    if (row_count != NULL) {
        *row_count = 0;
    }
    if (rows == NULL || row_capacity == 0 || row_count == NULL ||
        cpu == NULL || instruction_count <= 0) {
        return XEMU_CHEAT_DISAS_ERROR;
    }

    cpu_synchronize_state(cpu);

    /* Check the exact starting address separately. This lets the UI report
     * an unmapped virtual address distinctly from a missing decoder backend. */
    {
        uint8_t probe;
        if (cpu_memory_rw_debug(cpu, (vaddr)address, &probe, 1, false) != 0) {
            return XEMU_CHEAT_DISAS_UNMAPPED;
        }
    }

#ifndef CONFIG_CAPSTONE
    return XEMU_CHEAT_DISAS_NO_BACKEND;
#else
    {
        csh handle;
        cs_err err;
        uint64_t pc = address;
        int i;
        size_t produced = 0;

        err = cs_open(CS_ARCH_X86, CS_MODE_32, &handle);
        if (err != CS_ERR_OK) {
            return XEMU_CHEAT_DISAS_NO_BACKEND;
        }

        cs_option(handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
        cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);

        for (i = 0; i < instruction_count && produced < row_capacity &&
                    pc <= 0xFFFFFFFFull; ++i) {
            uint8_t code[15];
            size_t available = 0;
            size_t j;
            cs_insn *insn = NULL;
            size_t decoded;
            XemuCheatDisasmRow *row = &rows[produced];
            uint64_t physical = 0;

            for (j = 0; j < sizeof(code) && pc + j <= 0xFFFFFFFFull; ++j) {
                if (cpu_memory_rw_debug(cpu, (vaddr)(pc + j),
                                        &code[j], 1, false) != 0) {
                    break;
                }
                ++available;
            }

            if (available == 0) {
                if (i == 0) {
                    cs_close(&handle);
                    return XEMU_CHEAT_DISAS_UNMAPPED;
                }
                break;
            }

            memset(row, 0, sizeof(*row));
            row->virtual_address = (uint32_t)pc;
            if (xemu_cheat_virtual_to_physical_cpu(cpu, (uint32_t)pc,
                                                    &physical)) {
                row->physical_address = physical;
                row->physical_valid = 1;
            }

            decoded = cs_disasm(handle, code, available, pc, 1, &insn);
            if (decoded == 0 || insn == NULL || insn[0].size == 0) {
                row->size = 1;
                row->bytes[0] = code[0];
                g_strlcpy(row->mnemonic, "db", sizeof(row->mnemonic));
                g_snprintf(row->operands, sizeof(row->operands),
                           "0x%02X", code[0]);
                ++pc;
                if (insn != NULL) {
                    cs_free(insn, decoded);
                }
            } else {
                row->size = (uint8_t)MIN(insn[0].size, sizeof(row->bytes));
                memcpy(row->bytes, insn[0].bytes, row->size);
                g_strlcpy(row->mnemonic, insn[0].mnemonic,
                          sizeof(row->mnemonic));
                g_strlcpy(row->operands, insn[0].op_str,
                          sizeof(row->operands));
                pc += insn[0].size;
                cs_free(insn, decoded);
            }

            ++produced;
        }

        cs_close(&handle);
        *row_count = produced;
        return produced != 0 ? XEMU_CHEAT_DISAS_OK
                             : XEMU_CHEAT_DISAS_ERROR;
    }
#endif
}


int xemu_cheat_disassemble_page(uint32_t address, XemuCheatDisasmRow *rows,
                                size_t row_capacity, size_t *row_count)
{
    CPUState *cpu = qemu_get_cpu(0);
    const uint32_t page_base = address & (uint32_t)TARGET_PAGE_MASK;
    const size_t page_size = (size_t)TARGET_PAGE_SIZE;
    uint8_t *page_bytes;

    if (row_count != NULL) {
        *row_count = 0;
    }
    if (rows == NULL || row_capacity == 0 || row_count == NULL || cpu == NULL) {
        return XEMU_CHEAT_DISAS_ERROR;
    }

    cpu_synchronize_state(cpu);

    page_bytes = g_malloc(page_size);
    if (page_bytes == NULL) {
        return XEMU_CHEAT_DISAS_ERROR;
    }
    if (cpu_memory_rw_debug(cpu, (vaddr)page_base, page_bytes,
                            page_size, false) != 0) {
        g_free(page_bytes);
        return XEMU_CHEAT_DISAS_UNMAPPED;
    }

#ifndef CONFIG_CAPSTONE
    g_free(page_bytes);
    return XEMU_CHEAT_DISAS_NO_BACKEND;
#else
    {
        csh handle;
        cs_err err;
        size_t offset = 0;
        size_t produced = 0;

        err = cs_open(CS_ARCH_X86, CS_MODE_32, &handle);
        if (err != CS_ERR_OK) {
            g_free(page_bytes);
            return XEMU_CHEAT_DISAS_NO_BACKEND;
        }

        cs_option(handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
        cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);

        while (offset < page_size && produced < row_capacity) {
            const uint64_t pc = (uint64_t)page_base + offset;
            const size_t available = MIN((size_t)15, page_size - offset);
            cs_insn *insn = NULL;
            size_t decoded;
            XemuCheatDisasmRow *row = &rows[produced];
            uint64_t physical = 0;

            memset(row, 0, sizeof(*row));
            row->virtual_address = (uint32_t)pc;
            if (xemu_cheat_virtual_to_physical_cpu(cpu, (uint32_t)pc,
                                                    &physical)) {
                row->physical_address = physical;
                row->physical_valid = 1;
            }

            decoded = cs_disasm(handle, page_bytes + offset, available,
                                pc, 1, &insn);

            /* The page start is not guaranteed to be an x86 instruction
             * boundary. Never let a speculative decode cross over the exact
             * requested focus address; resynchronize there so breakpoint EIP
             * always appears as its own exact row. */
            if (decoded != 0 && insn != NULL && insn[0].size != 0 &&
                pc < address && pc + insn[0].size > address) {
                cs_free(insn, decoded);
                insn = NULL;
                decoded = 0;
            }

            if (decoded == 0 || insn == NULL || insn[0].size == 0 ||
                insn[0].size > available) {
                row->size = 1;
                row->bytes[0] = page_bytes[offset];
                g_strlcpy(row->mnemonic, "db", sizeof(row->mnemonic));
                g_snprintf(row->operands, sizeof(row->operands),
                           "0x%02X", page_bytes[offset]);
                ++offset;
                if (insn != NULL) {
                    cs_free(insn, decoded);
                }
            } else {
                row->size = (uint8_t)MIN(insn[0].size, sizeof(row->bytes));
                memcpy(row->bytes, insn[0].bytes, row->size);
                g_strlcpy(row->mnemonic, insn[0].mnemonic,
                          sizeof(row->mnemonic));
                g_strlcpy(row->operands, insn[0].op_str,
                          sizeof(row->operands));
                offset += insn[0].size;
                cs_free(insn, decoded);
            }

            ++produced;
        }

        cs_close(&handle);
        g_free(page_bytes);
        *row_count = produced;
        return produced != 0 ? XEMU_CHEAT_DISAS_OK
                             : XEMU_CHEAT_DISAS_ERROR;
    }
#endif
}

int xemu_cheat_get_executable_dir(char *buffer, size_t buffer_size)
{
    char *path;
    size_t path_len;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    buffer[0] = '\0';

    /* qemu_init_exec_dir() is called during normal xemu startup. Asking
     * QEMU to relocate CONFIG_BINDIR therefore gives us the directory that
     * actually contains the running xemu executable, independent of the
     * process working directory. */
    path = get_relocated_path(CONFIG_BINDIR);
    if (path == NULL || path[0] == '\0') {
        g_free(path);
        return 0;
    }

    path_len = strlen(path);
    if (path_len + 1 > buffer_size) {
        g_free(path);
        return 0;
    }

    memcpy(buffer, path, path_len + 1);
    g_free(path);
    return 1;
}

