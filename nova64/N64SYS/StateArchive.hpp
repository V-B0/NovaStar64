/**
 * @file StateArchive.hpp
 * @brief Byte stream that machine state is written to and read back from.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Nova64 {

/// Save states and rewind points are a flat byte stream. Each component has a
/// single SerializeState(StateArchive&) that both writes and reads it, so a
/// field can't be saved without also being restored.
///
/// Everything written must have the same size every time: plain values and
/// fixed arrays, no containers or pointers. That fixed layout is what lets
/// RestoreMachineState() check a state against this build before it changes
/// anything in the running machine.
class StateArchive {
public:
    static StateArchive Writer(std::vector<uint8_t>& out) { return StateArchive(&out, nullptr, 0); }
    static StateArchive Reader(const std::vector<uint8_t>& in) { return StateArchive(nullptr, in.data(), in.size()); }

    [[nodiscard]] bool IsLoading() const { return m_out == nullptr; }

    /// False after a read ran past the end of the data or found the wrong section.
    [[nodiscard]] bool Ok() const { return m_ok; }

    void Bytes(void* data, size_t size) {
        if (m_out) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            m_out->insert(m_out->end(), bytes, bytes + size);
        } else if (m_ok && size <= m_size - m_pos) {
            std::memcpy(data, m_in + m_pos, size);
            m_pos += size;
        } else {
            m_ok = false;
        }
    }

    template <class T> void Value(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "machine state must be plain data");
        Bytes(&value, sizeof(T));
    }

    /// Starts a component's data with a four-character tag.
    void Section(const char* tag) {
        uint32_t expected = 0;
        std::memcpy(&expected, tag, sizeof(expected));
        if (m_out) m_sections.push_back(m_out->size());
        uint32_t stored = expected;
        Value(stored);
        if (stored != expected) m_ok = false;
    }

    /// For a writer: true if `data` has the size and section tags of what was
    /// written, i.e. it was produced with the same layout.
    [[nodiscard]] bool SameLayoutAs(const std::vector<uint8_t>& data) const {
        if (!m_out || data.size() != m_out->size()) return false;
        for (size_t offset : m_sections) {
            if (std::memcmp(data.data() + offset, m_out->data() + offset, sizeof(uint32_t)) != 0) return false;
        }
        return true;
    }

private:
    StateArchive(std::vector<uint8_t>* out, const uint8_t* in, size_t size) : m_out(out), m_in(in), m_size(size) {}

    std::vector<uint8_t>* m_out;
    const uint8_t* m_in;
    size_t m_size;
    size_t m_pos = 0;
    bool m_ok = true;
    std::vector<size_t> m_sections;
};

} // namespace Nova64
