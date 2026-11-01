// A transport with made-up data instead of TDLib, for tests and screenshots: no network, no account. Started with
// CONTRAIL_SCENARIO=1. Grows with the features that need showing.
#pragma once

#include "td/client.hpp"

#include <memory>

namespace Contrail::Td {

[[nodiscard]] std::unique_ptr<Transport> MakeScenarioTransport();

}  // namespace Contrail::Td
