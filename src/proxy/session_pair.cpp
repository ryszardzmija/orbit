#include "session_pair.h"

#include <memory>

namespace orbit {

std::unique_ptr<SessionPair> makeSessionPair(int downstream_fd, int upstream_fd,
                                             const SendBufferFactory& factory) {
    auto pair = std::make_unique<SessionPair>();

    pair->upstream = {upstream_fd, &pair->downstream, factory.make(), 0, false, false};
    pair->downstream = {downstream_fd, &pair->upstream, factory.make(), 0, false, false};

    return pair;
}

} // namespace orbit
