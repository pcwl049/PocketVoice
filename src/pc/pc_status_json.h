#pragma once

#include "pc_runtime.h"

#include <string>

namespace stt {

std::string toStatusJson(const PcRuntimeSnapshot& snapshot);

}
