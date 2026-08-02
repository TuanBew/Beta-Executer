#pragma once
#include <cstdint>
#include <vector>
#include <cstring>
#include <string>

namespace PipeProtocol {

constexpr uint32_t PIPE_MAGIC    = 0x48554221;  // "HUB!"
constexpr uint8_t  PIPE_VERSION  = 0x01;
constexpr size_t   FRAME_HEADER_SIZE = 11;       // 4 + 1 + 2 + 4

enum class Command : uint16_t {
    EXECUTE_SCRIPT = 0x01,
    EXECUTE_RESULT = 0x02,
    PING           = 0x03,
    PONG           = 0x04,
    SHUTDOWN       = 0x05,
};

enum class Result : uint8_t {
    OK    = 0,
    ERROR = 1,
    FATAL = 2,
};

// State flags for PONG response
enum StateFlags : uint32_t {
    STATE_READY     = 1 << 0,
    STATE_EXECUTING = 1 << 1,
    STATE_ERROR     = 1 << 2,
};

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint8_t  version;
    uint16_t command;
    uint32_t payloadLen;
};
#pragma pack(pop)

inline std::vector<uint8_t> SerializeFrame(Command cmd,
                                            const void* payload,
                                            uint32_t payloadLen) {
    std::vector<uint8_t> buf(FRAME_HEADER_SIZE + payloadLen);
    auto* hdr = reinterpret_cast<FrameHeader*>(buf.data());
    hdr->magic      = PIPE_MAGIC;
    hdr->version    = PIPE_VERSION;
    hdr->command    = static_cast<uint16_t>(cmd);
    hdr->payloadLen = payloadLen;
    if (payload && payloadLen > 0) {
        std::memcpy(buf.data() + FRAME_HEADER_SIZE, payload, payloadLen);
    }
    return buf;
}

inline bool ValidateFrameHeader(const uint8_t* buf, size_t bufSize) {
    if (bufSize < FRAME_HEADER_SIZE) return false;
    auto* hdr = reinterpret_cast<const FrameHeader*>(buf);
    return hdr->magic == PIPE_MAGIC && hdr->version <= PIPE_VERSION;
}

inline bool DeserializeFrameHeader(const uint8_t* buf, size_t bufSize,
                                    Command& outCmd, uint32_t& outPayloadLen) {
    if (!ValidateFrameHeader(buf, bufSize)) return false;
    auto* hdr = reinterpret_cast<const FrameHeader*>(buf);
    outCmd        = static_cast<Command>(hdr->command);
    outPayloadLen = hdr->payloadLen;
    return true;
}

// Convenience: build a command frame with no payload
inline std::vector<uint8_t> MakeSimpleFrame(Command cmd) {
    return SerializeFrame(cmd, nullptr, 0);
}

// Convenience: build EXECUTE_SCRIPT frame from string
inline std::vector<uint8_t> MakeExecuteFrame(const std::string& script) {
    return SerializeFrame(Command::EXECUTE_SCRIPT,
                          script.data(),
                          static_cast<uint32_t>(script.size()));
}

// Convenience: build EXECUTE_RESULT frame
inline std::vector<uint8_t> MakeResultFrame(Result status,
                                              const std::string& errorMsg) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(status));
    if (!errorMsg.empty()) {
        payload.insert(payload.end(),
                       errorMsg.begin(),
                       errorMsg.end());
        payload.push_back(0); // null terminator
    }
    return SerializeFrame(Command::EXECUTE_RESULT,
                          payload.data(),
                          static_cast<uint32_t>(payload.size()));
}

} // namespace PipeProtocol
