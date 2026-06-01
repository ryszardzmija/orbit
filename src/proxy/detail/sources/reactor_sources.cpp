#include "proxy/detail/sources/reactor_sources.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>

#include "proxy/detail/sources/source_id.h"

namespace orbit::proxy::detail {

SourceId ReactorSources::add(ReactorRegistration registration) {
    SourceId id = id_generator_.getNextId();

    auto [it, inserted] = registrations_.emplace(id, std::move(registration));
    assert(inserted && "generated ID is duplicate");

    return id;
}

void ReactorSources::remove(SourceId id) {
    std::size_t erased = registrations_.erase(id);
    assert(erased == 1 && "source ID is not registered");
}

std::optional<ReactorRegistration> ReactorSources::find(SourceId id) const {
    auto it = registrations_.find(id);

    if (it == registrations_.end()) {
        return std::nullopt;
    }

    return it->second;
}

} // namespace orbit::proxy::detail
