#ifndef FASTBEE_SAME_ORIGIN_H
#define FASTBEE_SAME_ORIGIN_H

#include <Arduino.h>

namespace FastBee {

inline bool isSameOriginRequest(const String& method, const String& origin, const String& host) {
    if (method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE") return true;
    if (origin.isEmpty()) return true;
    if (host.isEmpty()) return false;
    return origin == ("http://" + host) || origin == ("https://" + host) ||
           origin == "http://fastbee.local" || origin == "https://fastbee.local";
}

} // namespace FastBee

#endif // FASTBEE_SAME_ORIGIN_H
