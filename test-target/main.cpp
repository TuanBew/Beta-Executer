/**
 * Mock Test Target (mock_test_exe.exe)
 *
 * A simulated target process that initializes a memory region matching
 * the expected layout of test.exe. Used to test the Universal Hub's
 * memory I/O operations in a controlled, predictable environment.
 *
 * The process:
 *  1. Allocates a fixed memory block to simulate the target's data segment
 *  2. Places known values at the offsets defined in offsets.h
 *  3. Prints its PID and base address for easy attachment
 *  4. Loops indefinitely, allowing the Universal Hub to interact with it
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <windows.h>

// Forward-declare offsets we'll simulate (matching offsets.h)
// These are the key fields we'll populate for testing
namespace SimOffsets {
    constexpr uintptr_t Name           = 0x98;
    constexpr uintptr_t Children       = 0x70;
    constexpr uintptr_t Parent         = 0x68;
    constexpr uintptr_t ClassDescriptor = 0x18;
    constexpr uintptr_t ClassDescriptorToClassName = 0x8;
    constexpr uintptr_t WalkSpeed      = 0x1d0;
    constexpr uintptr_t JumpPower      = 0x1a4;
    constexpr uintptr_t HipHeight      = 0x194;
    constexpr uintptr_t Health         = 0x190;
    constexpr uintptr_t MaxHealth      = 0x1a8;
    constexpr uintptr_t FOV            = 0x140;
    constexpr uintptr_t Position       = 0xec;
    constexpr uintptr_t Gravity        = 0x210;
    constexpr uintptr_t Value          = 0xb8;
    constexpr uintptr_t LocalPlayer    = 0x130;
    constexpr uintptr_t IsPlaying      = 0x140;
    constexpr uintptr_t ClassBase      = 0x1b0;
}

// Simulated object structure sizes
constexpr size_t OBJ_SIZE  = 0x200;  // Base object
constexpr size_t PLAYER_SIZE = 0x400; // Player object (larger)
constexpr size_t PART_SIZE = 0x300;   // Part object

struct SimulatedRegion {
    // Our "heap" — a contiguous block simulating the target's memory
    uint8_t* base;
    size_t   totalSize;
    uintptr_t baseAddr;       // Virtual address of the block
};

SimulatedRegion g_Region{};

/**
 * Write a string at a given offset within the region.
 */
void WriteStr(uintptr_t offset, const std::string& str) {
    size_t maxLen = 64;
    size_t len = std::min(str.size(), maxLen - 1);
    std::memcpy(g_Region.base + offset, str.c_str(), len);
    g_Region.base[offset + len] = '\0';
}

/**
 * Write a value at a given offset within the region.
 */
template<typename T>
void WriteVal(uintptr_t offset, T value) {
    std::memcpy(g_Region.base + offset, &value, sizeof(T));
}

/**
 * Create a mock "object" at a given offset within the region.
 * Simulates the internal layout of objects in the target application.
 */
void CreateMockObject(uintptr_t objOffset, const std::string& name,
                      const std::string& className, uintptr_t parentOffset) {
    // Name string (offset 0x98)
    WriteStr(objOffset + SimOffsets::Name, name);
    // Parent pointer (offset 0x68)
    WriteVal<uintptr_t>(objOffset + SimOffsets::Parent, parentOffset);

    // ---- ClassDescriptor simulation ----
    // Allocate a small structure for the ClassDescriptor and class name string
    // We place it after the object
    uintptr_t cdOffset = objOffset + OBJ_SIZE;
    // ClassDescriptorToClassName pointer (offset 0x8 of ClassDescriptor)
    uintptr_t classNameStrOffset = cdOffset + 0x40;
    WriteVal<uintptr_t>(cdOffset + SimOffsets::ClassDescriptorToClassName, classNameStrOffset);
    WriteStr(classNameStrOffset - g_Region.baseAddr, className); // Actually we write direct
    // Wait, let me rethink this — we write into the region at classNameStrOffset

    // Simpler approach: ClassDescriptor is just an object in our region
    // We'll place className string right after the descriptor
    uintptr_t cnActual = cdOffset + 0x40;
    // Actually, we need to use absolute addresses or offsets consistently
    // Let's use offsets from base for everything
    // ClassDescriptor is at objOffset + OBJ_SIZE
    // ClassDescriptor + 0x8 points to class name string
    // Class name string is at objOffset + OBJ_SIZE + 0x40
    WriteVal<uintptr_t>(cdOffset + 0x8,
                        g_Region.baseAddr + objOffset + OBJ_SIZE + 0x40);
    WriteStr(objOffset + OBJ_SIZE + 0x40, className);
    // Object's ClassDescriptor pointer (offset 0x18)
    WriteVal<uintptr_t>(objOffset + SimOffsets::ClassDescriptor,
                        g_Region.baseAddr + cdOffset);
}

/**
 * Initialize the simulated memory region with mock objects.
 */
void InitRegion() {
    // Allocate a large contiguous block (16 MB)
    g_Region.totalSize = 16 * 1024 * 1024;
    g_Region.base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, g_Region.totalSize,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_Region.base) {
        std::cerr << "Failed to allocate simulated region" << std::endl;
        return;
    }
    g_Region.baseAddr = reinterpret_cast<uintptr_t>(g_Region.base);
    // Zero-fill
    std::memset(g_Region.base, 0, g_Region.totalSize);

    std::cout << "[Mock] Allocated " << (g_Region.totalSize / (1024*1024))
              << " MB at 0x" << std::hex << g_Region.baseAddr << std::dec << std::endl;

    // ---- Create mock object tree ----

    uintptr_t offset = 0x1000; // Start at some offset into the region

    // Root "Workspace" object
    CreateMockObject(offset, "Workspace", "Workspace", 0);
    uintptr_t workspace = g_Region.baseAddr + offset;

    // Camera object
    offset += OBJ_SIZE + 0x100;
    CreateMockObject(offset, "Camera", "Camera", workspace);
    uintptr_t camera = g_Region.baseAddr + offset;

    // Player object
    offset += OBJ_SIZE + 0x100;
    CreateMockObject(offset, "Player1", "Player", workspace);
    uintptr_t player = g_Region.baseAddr + offset;
    // Write player-specific fields
    WriteVal<float>(    offset + SimOffsets::WalkSpeed,  16.0f);
    WriteVal<float>(    offset + SimOffsets::JumpPower,  50.0f);
    WriteVal<float>(    offset + SimOffsets::HipHeight,  0.0f);
    WriteVal<float>(    offset + SimOffsets::Health,     100.0f);
    WriteVal<float>(    offset + SimOffsets::MaxHealth,  100.0f);
    WriteVal<float>(    offset + SimOffsets::FOV,        70.0f);
    WriteVal<uintptr_t>(offset + SimOffsets::LocalPlayer, player);

    // Character part (child of Player)
    offset += PLAYER_SIZE + 0x100;
    CreateMockObject(offset, "Head", "Part", player);
    uintptr_t head = g_Region.baseAddr + offset;
    // Simulate Children linked list for Player
    // Player.Children → Head (simplified: first child pointer)
    WriteVal<uintptr_t>(player - g_Region.baseAddr + SimOffsets::Children, head);

    offset += PART_SIZE + 0x100;
    CreateMockObject(offset, "Torso", "Part", player);
    uintptr_t torso = g_Region.baseAddr + offset;
    // Head's next sibling → Torso (using ChildrenEnd as "next" pointer)
    WriteVal<uintptr_t>(head - g_Region.baseAddr + 0x8, torso);
    // Actually the Children/ChildrenEnd pair: Children = first child, ChildrenEnd = 0x8 offset
    // In many internal layouts: Children = pointer to first, offset+8 = pointer to next sibling (linked list)

    // ---- Create mock communication objects (RemoteEvent simulation) ----
    offset += PART_SIZE + 0x200;
    CreateMockObject(offset, "OnPlayerJoined", "RemoteEvent", workspace);
    uintptr_t remote1 = g_Region.baseAddr + offset;
    // ClassBase pointer to mark it as a communication object
    // (RemoteEvent inherits from BaseScript → Instance)

    offset += OBJ_SIZE + 0x100;
    CreateMockObject(offset, "OnDataReceived", "RemoteFunction", workspace);
    uintptr_t remote2 = g_Region.baseAddr + offset;

    // ---- Write environmental values ----
    // Gravity in the workspace
    WriteVal<float>(workspace - g_Region.baseAddr + SimOffsets::Gravity, 196.2f);
    WriteVal<float>(workspace - g_Region.baseAddr + SimOffsets::IsPlaying, 1.0f);

    // ---- Summary ----
    std::cout << "[Mock] Created objects:\n";
    std::cout << "  Workspace  @ 0x" << std::hex << workspace      << std::dec << "\n";
    std::cout << "  Camera     @ 0x" << std::hex << camera         << std::dec << "\n";
    std::cout << "  Player1    @ 0x" << std::hex << player         << std::dec << "\n";
    std::cout << "  Head       @ 0x" << std::hex << head           << std::dec << "\n";
    std::cout << "  Torso      @ 0x" << std::hex << torso          << std::dec << "\n";
    std::cout << "  Remotes    @ 0x" << std::hex << remote1 << ", 0x" << remote2 << std::dec << "\n";
    std::cout << "\n";
    std::cout << "[Mock] Player WalkSpeed offset 0x" << std::hex << SimOffsets::WalkSpeed
              << " → addr 0x" << (player + SimOffsets::WalkSpeed) << std::dec << "\n";
    std::cout << "[Mock] Player FOV offset 0x" << std::hex << SimOffsets::FOV
              << " → addr 0x" << (player + SimOffsets::FOV) << std::dec << "\n";
}

int main() {
    std::cout << "=== Mock Test Target ===\n";
    std::cout << "FOR EDUCATIONAL DEMONSTRATION ONLY\n";
    std::cout << "Simulates the expected memory layout of test.exe\n\n";

    std::cout << "PID: " << GetCurrentProcessId() << std::endl;

    InitRegion();

    std::cout << "\n[Mock] Running — attach Universal Hub and test memory operations.\n";
    std::cout << "[Mock] Press Ctrl+C to exit.\n\n";

    // Infinite loop — allows memory inspection
    // Every few seconds, print a heartbeat so the user knows it's alive
    int tick = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        ++tick;

        // Read current WalkSpeed to show live interaction
        uintptr_t playerAddr = g_Region.baseAddr + 0x1000 + OBJ_SIZE + 0x100 + OBJ_SIZE + 0x100;
        float ws = 0;
        std::memcpy(&ws,
                    g_Region.base + (playerAddr - g_Region.baseAddr) + SimOffsets::WalkSpeed,
                    sizeof(float));
        std::cout << "[Mock] tick=" << tick << " WalkSpeed=" << ws
                  << " FOV=";
        float fov = 0;
        std::memcpy(&fov,
                    g_Region.base + (playerAddr - g_Region.baseAddr) + SimOffsets::FOV,
                    sizeof(float));
        std::cout << fov << std::endl;
    }

    VirtualFree(g_Region.base, 0, MEM_RELEASE);
    return 0;
}
