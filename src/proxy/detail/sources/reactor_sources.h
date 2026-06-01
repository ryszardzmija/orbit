#pragma once

#include <optional>

#include <absl/container/flat_hash_map.h>

#include "proxy/detail/sources/registration.h"
#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

class ReactorSources {
public:
    SourceId add(ReactorRegistration registration);
    void remove(SourceId id);
    std::optional<ReactorRegistration> find(SourceId id) const;

private:
    SourceIdGenerator id_generator_;
    absl::flat_hash_map<SourceId, ReactorRegistration> registrations_;
};

} // namespace orbit::proxy::detail
