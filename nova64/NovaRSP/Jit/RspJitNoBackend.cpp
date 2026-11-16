/**
 * @file RspJitNoBackend.cpp
 * @brief Stand-in for CPUs the RSP recompiler has no code emitter for yet.
 */

#include "RspJitBackend.hpp"

#if !(defined(__aarch64__) || defined(_M_ARM64)) && !(defined(__x86_64__) || defined(_M_X64))

namespace Nova64::RSP::Jit {
std::unique_ptr<Backend> CreateBackend() { return nullptr; }
} // namespace Nova64::RSP::Jit

#endif
