#ifndef AI_PROVIDER_STREAM_H
#define AI_PROVIDER_STREAM_H

#include <Arduino.h>
#include <atomic>

class AiProviderStream {
public:
    virtual ~AiProviderStream() = default;
    virtual int available() = 0;
    virtual size_t readBytes(uint8_t* data, size_t length) = 0;
    virtual bool connected() = 0;
};

enum class AiHttpReadStatus : uint8_t {
    COMPLETE,
    TIMEOUT,
    TOO_LARGE,
    IO_ERROR
};

#endif
