/*
  Hatari - netusbee/core/StateArchive.hpp

  This file is distributed under the GNU General Public License, version 3
  or at your option any later version. Read the file gpl-3.0.txt for details.

  NeoST's state archive, for the NeoST devices in Hatari: the same calls
  (ar(x), ar.arr(), ar.vec(), ar.check(), ar.loading()), on top of Hatari's
  memory snapshots (MemorySnapShot_Store saves or restores, as the
  snapshot goes).
*/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

extern "C" void MemorySnapShot_Store(void *pData, int Size);

class StateArchive {
public:
    explicit StateArchive(bool loading) : loading_(loading) {}
    bool loading() const { return loading_; }
    // false when a restored value broke a check: the device state is not usable
    bool ok() const { return ok_; }
    const char* failed() const { return what_; }

    void check(bool cond, const char* what = nullptr) {
        if (loading_ && !cond && ok_) { ok_ = false; what_ = what; }
    }

    // a scalar or a plain struct
    template <class T>
    void operator()(T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "StateArchive: not a plain type");
        raw(&v, sizeof v);
        if (std::is_same<T, bool>::value && loading_) {
            unsigned char b;    // a restored bool is 0 or 1
            std::memcpy(&b, &v, 1);
            b = b != 0;
            std::memcpy(&v, &b, 1);
        }
    }

    template <class T, std::size_t N>
    void arr(T (&v)[N]) { raw(v, sizeof v); }
    template <class T, std::size_t N>
    void arr(std::array<T, N>& v) { raw(v.data(), sizeof(T) * N); }

    // a byte vector, its length first
    void vec(std::vector<uint8_t>& v) {
        uint32_t n = static_cast<uint32_t>(v.size());
        (*this)(n);
        if (loading_) {
            if (n > (64u << 20)) { check(false, "vector length"); n = 0; }
            v.resize(n);
        }
        raw(v.data(), n);
    }

private:
    bool loading_;
    bool ok_ = true;
    const char* what_ = nullptr;
    void raw(void* p, std::size_t n) {
        if (n) MemorySnapShot_Store(p, static_cast<int>(n));
    }
};
