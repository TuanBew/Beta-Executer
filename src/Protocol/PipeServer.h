#pragma once

#include <windows.h>
#include <string>
#include <cstdint>
#include <vector>

class PipeServer {
public:
    static PipeServer& GetInstance();

    bool Initialize();
    void Shutdown();
    bool IsConnected() const;

    // Send script to DLL, wait for result.
    // Returns true if script executed without error.
    // On error, outError is populated.
    bool ExecuteScript(const std::string& script, std::string& outError);

    // Health check. outStateFlags receives the DLL's state bits.
    bool Ping(uint32_t& outStateFlags);

private:
    PipeServer() = default;
    ~PipeServer();

    // Non-copyable, non-movable
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    bool   m_connected = false;

    // Low-level: read a complete frame from the pipe
    bool ReadFrame(uint16_t& outCmd, std::vector<uint8_t>& outPayload);
    // Low-level: write a frame to the pipe
    bool WriteFrame(uint16_t cmd, const void* payload, uint32_t len);
};
