#include "../bench/chain_protobuf.hpp"
#include <cstdio>
#include <memory>
using namespace chain_bench;
int main() {
    const uint32_t sizes[] = {16,   128,   256,   512,   1024,   2048,   3072,   4096,
                              8192, 16384, 32768, 65536, 131072, 524288, 1048576};
    auto storage = std::unique_ptr<unsigned char[]>(new (std::nothrow) unsigned char[kMaxBody + kHeader]);
    if (!storage)
        return 1;
    ProtobufCodec encoder, decoder;
    decoder.Configure(true);
    for (auto size : sizes) {
        chain_bench_pb::Rpc message;
        message.set_payload(std::string(ProtoPayloadBytes(size), 'x'));
        for (uint64_t seq : {uint64_t(0), UINT64_MAX}) {
            message.set_sequence(seq);
            ProtoOutgoing out;
            if (message.ByteSizeLong() != size ||
                encoder.Prepare(message, kMagic | kProtoFlag, &out) != shm::ErrorCode::kOk ||
                encoder.EncodePrepared(out, storage.get(), size + kHeader) != shm::ErrorCode::kOk)
                return 2;
            std::any decoded;
            if (decoder.Decode(reinterpret_cast<comm::byte *>(storage.get()), size + kHeader, decoded, nullptr) !=
                int(size + kHeader))
                return 3;
            auto frame = ProtobufCodec::Get(decoded);
            if (!frame || !frame->message || frame->message->sequence() != seq ||
                frame->message->payload() != message.payload())
                return 4;
            if (decoder.Decode(reinterpret_cast<comm::byte *>(storage.get()), size + kHeader - 1, decoded, nullptr) !=
                0)
                return 5;
        }
    }
    if (decoder.Stats().parses != 30 || decoder.Stats().payload_address_changes != 0)
        return 6;
    storage[kHeader] = 0;
    std::any decoded;
    if (decoder.Decode(reinterpret_cast<comm::byte *>(storage.get()), kMaxBody + kHeader, decoded, nullptr) != -1)
        return 7;
    std::puts("protobuf exact lengths, sequences, partial frames, corruption and reuse passed");
    return 0;
}
