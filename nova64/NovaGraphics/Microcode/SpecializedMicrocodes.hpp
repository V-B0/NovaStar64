#pragma once
// Stub handlers for specialized microcodes not yet fully implemented.
// Each one falls back to the closest compatible base handler.

#include "F3D.hpp"
#include "F3DEX.hpp"
#include "F3DEX2.hpp"
#include "ExtendedGBIHandlers.hpp"

namespace Nova64::Video {

// F3DTEXA — texture-alpha variant used by Yoshi's Story (GBI1, F3D-based)
class F3DTEXAHandler : public F3DHandler {
public:
    F3DTEXAHandler() = default;
    MicrocodeType GetType() const override { return MicrocodeType::F3DTEXA; }
    const char* GetName() const override { return "F3DTEXA (Yoshi's Story)"; }
};

// F3DGOLDEN — GoldenEye F3D variant (GBI1, F3D-based)
class F3DGOLDENHandler : public F3DHandler {
public:
    F3DGOLDENHandler() = default;
    MicrocodeType GetType() const override { return MicrocodeType::F3DGOLDEN; }
    const char* GetName() const override { return "F3DGOLDEN (GoldenEye)"; }
};

// JetForce — Jet Force Gemini (GBI2, F3DEX2-based)
class JetForceHandler : public F3DEX2Handler {
public:
    JetForceHandler() {
        m_type = MicrocodeType::JetForce;
        m_name = "JetForce (JFG)";
    }
};

// F3DBETA — early F3D beta (GBI1, F3D-based)
class F3DBETAHandler : public F3DHandler {
public:
    F3DBETAHandler() = default;
    MicrocodeType GetType() const override { return MicrocodeType::F3DBETA; }
    const char* GetName() const override { return "F3DBETA (early beta)"; }
};

// F3DEX3 — newest F3DEX2 derivative (GBI2, F3DEX2-based)
class F3DEX3Handler : public F3DEX2Handler {
public:
    F3DEX3Handler() {
        m_type = MicrocodeType::F3DEX3;
        m_name = "F3DEX3";
    }
};

// F3DEX095 — F3DEX 0.95 used by Mario Kart 64 (GBI1, F3DEX-based)
class F3DEX095Handler : public F3DEXHandler {
public:
    F3DEX095Handler() = default;
    MicrocodeType GetType() const override { return MicrocodeType::F3DEX095; }
    const char* GetName() const override { return "F3DEX 0.95 (MK64)"; }
};

// F5Rogue — Star Wars: Rogue Squadron (Factor 5 custom, GBI2 subset)
class F5RogueHandler : public F3DEX2Handler {
public:
    F5RogueHandler() {
        m_type = MicrocodeType::F5Rogue;
        m_name = "F5 Rogue Squadron";
    }
};

// F5IndiNaboo — Indiana Jones / Battle for Naboo (Factor 5 custom)
class F5IndiNabooHandler : public F3DEX2Handler {
public:
    F5IndiNabooHandler() {
        m_type = MicrocodeType::F5Indi_Naboo;
        m_name = "F5 Indi/Naboo";
    }
};

// T3DUX — Turbo3D UX (Toukon Road / Acclaim)
class T3DUXHandler : public Turbo3DGBIHandler {
public:
    T3DUXHandler() = default;
    MicrocodeType GetType() const override { return MicrocodeType::T3DUX; }
    const char* GetName() const override { return "T3DUX (Turok/Acclaim)"; }
};

} // namespace Nova64::Video
