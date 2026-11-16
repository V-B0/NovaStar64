/**
 * @file LoopAnalysis.hpp
 * @brief Loop analysis for register optimization in dynamic recompilation
 * 
 * Derived from Project64 (https://github.com/project64/project64),
 * licensed under the GNU General Public License v2.0 -- see LICENSE.
 * 
 * This class performs data flow analysis on loops to determine which registers
 * can be kept in host registers across loop iterations, enabling significant
 * performance optimizations through register allocation.
 */

#pragma once

#include "RegBase.hpp"
#include "RegInfo.hpp"
#include "CodeSection.hpp"
#include "CodeBlock.hpp"
#include "ExitInfo.hpp"
#include "../Types.hpp"
#include "../SysGlobals.hpp"
#include "../MIPSY/R4300Instruction.hpp"
#include "../MIPSY/R4300Iops.hpp"
#include "../MIPSY/Vmem.hpp"
#include "../../common/MemoryManagment.hpp"
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>

namespace Nova64 {
namespace Recompiler {

class CodeSection;
class CodeBlock;

} // namespace Recompiler
} // namespace Nova64
