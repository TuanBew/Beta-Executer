#include "PipeServer.h"
#include "../Payload/PipeProtocol.h"
#include "../Logging/Logger.h"
#include <vector>

static constexpr const char* PIPE_NAME = "\\\\.\\pipe\\UniversalHub";

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

PipeServer& PipeServer::GetInstance() {
    static PipeServer instance;
    return instance;
}

PipeServer::~PipeServer() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Initialize / Shutdown
// ---------------------------------------------------------------------------

bool PipeServer::Initialize() {
    m_hPipe = CreateNamedPipeA(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX,            // read/write
        PIPE_TYPE_MESSAGE |             // message mode (frame boundaries preserved)
        PIPE_READMODE_MESSAGE |
        PIPE_WAIT,                      // blocking I/O
        1,                              // max 1 instance (single client)
        65536,                          // output buffer: 64KB
        65536,                          // input buffer: 64KB
        0,                              // default timeout
        nullptr);                       // default security

    if (m_hPipe == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[PipeServer] CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }

    LOG_INFO("[PipeServer] Waiting for DLL connection...");

    // Block until the DLL connects
    BOOL connected = ConnectNamedPipe(m_hPipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        LOG_ERROR("[PipeServer] ConnectNamedPipe failed: %lu", GetLastError());
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
        return false;
    }

    m_connected = true;
    LOG_INFO("[PipeServer] DLL connected on %s", PIPE_NAME);
    return true;
}

void PipeServer::Shutdown() {
    if (m_connected) {
        // Send SHUTDOWN command to DLL
        auto frame = PipeProtocol::MakeSimpleFrame(
            PipeProtocol::Command::SHUTDOWN);
        DWORD written = 0;
        WriteFile(m_hPipe, frame.data(),
                  static_cast<DWORD>(frame.size()), &written, nullptr);
        m_connected = false;
    }
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_hPipe);
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
}

bool PipeServer::IsConnected() const {
    return m_connected;
}

// ---------------------------------------------------------------------------
// Low-level Frame I/O
// ---------------------------------------------------------------------------

bool PipeServer::ReadFrame(uint16_t& outCmd,
                            std::vector<uint8_t>& outPayload) {
    // Read header
    uint8_t header[PipeProtocol::FRAME_HEADER_SIZE];
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(m_hPipe, header, sizeof(header), &bytesRead, nullptr);
    if (!ok || bytesRead != sizeof(header)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
            m_connected = false;
        }
        return false;
    }

    PipeProtocol::Command cmd;
    uint32_t payloadLen = 0;
    if (!PipeProtocol::DeserializeFrameHeader(header, sizeof(header),
                                               cmd, payloadLen)) {
        return false;
    }
    outCmd = static_cast<uint16_t>(cmd);

    // Read payload
    outPayload.clear();
    if (payloadLen > 0) {
        outPayload.resize(payloadLen);
        ok = ReadFile(m_hPipe, outPayload.data(), payloadLen,
                      &bytesRead, nullptr);
        if (!ok || bytesRead != payloadLen) {
            return false;
        }
    }
    return true;
}

bool PipeServer::WriteFrame(uint16_t cmd,
                             const void* payload, uint32_t len) {
    auto frame = PipeProtocol::SerializeFrame(
        static_cast<PipeProtocol::Command>(cmd), payload, len);
    DWORD written = 0;
    BOOL ok = WriteFile(m_hPipe, frame.data(),
                        static_cast<DWORD>(frame.size()), &written, nullptr);
    return ok && written == frame.size();
}

// ---------------------------------------------------------------------------
// Public Commands
// ---------------------------------------------------------------------------

bool PipeServer::ExecuteScript(const std::string& script,
                                std::string& outError) {
    if (!m_connected) return false;

    // Send EXECUTE_SCRIPT frame
    if (!WriteFrame(
            static_cast<uint16_t>(PipeProtocol::Command::EXECUTE_SCRIPT),
            script.data(),
            static_cast<uint32_t>(script.size()))) {
        return false;
    }

    // Wait for EXECUTE_RESULT
    uint16_t cmd = 0;
    std::vector<uint8_t> payload;
    if (!ReadFrame(cmd, payload)) return false;

    if (cmd != static_cast<uint16_t>(PipeProtocol::Command::EXECUTE_RESULT)) {
        return false;
    }

    if (payload.empty()) return false;

    auto status = static_cast<PipeProtocol::Result>(payload[0]);
    if (status == PipeProtocol::Result::OK) {
        return true;
    }

    // Extract error message (after status byte, up to null terminator)
    if (payload.size() > 1) {
        outError.assign(reinterpret_cast<const char*>(&payload[1]),
                        payload.size() - 1);
        // Trim trailing null
        if (!outError.empty() && outError.back() == '\0') {
            outError.pop_back();
        }
    }
    return false;
}

bool PipeServer::Ping(uint32_t& outStateFlags) {
    if (!m_connected) return false;

    if (!WriteFrame(
            static_cast<uint16_t>(PipeProtocol::Command::PING),
            nullptr, 0)) {
        return false;
    }

    uint16_t cmd = 0;
    std::vector<uint8_t> payload;
    if (!ReadFrame(cmd, payload)) return false;

    if (cmd != static_cast<uint16_t>(PipeProtocol::Command::PONG)) {
        return false;
    }

    outStateFlags = 0;
    if (payload.size() >= 4) {
        outStateFlags = payload[0]
            | (static_cast<uint32_t>(payload[1]) << 8)
            | (static_cast<uint32_t>(payload[2]) << 16)
            | (static_cast<uint32_t>(payload[3]) << 24);
    }
    return true;
}
