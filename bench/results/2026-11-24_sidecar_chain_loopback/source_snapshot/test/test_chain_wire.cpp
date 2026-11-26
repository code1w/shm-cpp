#include "../bench/chain_wire.hpp"
#include <cstdio>

int main()
{
    using namespace chain_bench;
    std::array<unsigned char, 32> bytes{};
    Write32(bytes.data(), kMagic);
    Write32(bytes.data() + 4, 16);
    WriteSequence(bytes.data(), UINT64_C(0x0102030405060708));
    Frame frame{bytes.data(), uint32_t(bytes.size())};
    for (unsigned i = 0; i < 8; ++i)
        if (bytes[8 + i] != i + 1)
            return 1;
    if (Sequence(frame) != UINT64_C(0x0102030405060708))
        return 2;
    for (unsigned n = 0; n < bytes.size(); ++n)
        if (Probe(bytes.data(), n) != 0)
            return 3;
    if (Probe(bytes.data(), bytes.size()) != 32)
        return 4;
    for (uint32_t mode = 0; mode < 3; ++mode)
    {
        Write32(bytes.data(), kMagic + mode);
        if (Probe(bytes.data(), bytes.size()) != 32)
            return 5;
    }
    Write32(bytes.data(), kMagic + 3);
    if (Probe(bytes.data(), bytes.size()) != -1)
        return 6;
    Write32(bytes.data(), kMagic);
    Write32(bytes.data() + 4, kMaxBody + 1);
    if (Probe(bytes.data(), bytes.size()) != -1)
        return 7;
    Write32(bytes.data() + 4, 16);
    WireCodec codec;
    std::any input{static_cast<const Frame*>(&frame)}, output;
    std::array<comm::byte, 32> encoded{};
    size_t need = 0;
    if (codec.EncodedSize(input) != 32 || codec.Encode(input, encoded.data(), 31, &need) != 0 || need != 32)
        return 8;
    if (codec.Encode(input, encoded.data(), 32, &need) != 32)
        return 9;
    if (codec.Decode(encoded.data(), 31, output, &need) != 0 || codec.Decode(encoded.data(), 32, output, &need) != 32)
        return 10;
    if (!WireCodec::Get(output) || Sequence(*WireCodec::Get(output)) != Sequence(frame))
        return 11;
    std::any wrong{17};
    if (codec.EncodedSize(wrong) != -1)
        return 12;
    std::puts("PASS: chain wire endian, fragmentation, bounds, modes and borrowed codec view");
    return 0;
}
