#pragma once
#include <string>

namespace tw::sim {
// Create (once) a fake device filesystem used by --sim mode.
void ensureRoot(const std::string& root);
}
