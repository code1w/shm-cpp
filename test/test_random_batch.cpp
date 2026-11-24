#include "../bench/random_batch.hpp"
#include <array>
#include <cstdio>

int main() {
    using acceptance_bench::RandomBatch;
    RandomBatch golden(1,8,1);
    for (auto expected : {6u,6u,8u,6u,1u,2u,2u,6u}) {
        if(golden.Next()!=expected) {std::fputs("FAIL: golden stream\n",stderr);return 1;}
    }
    for(auto bounds : {std::array<unsigned,2>{1,2},{1,4},{1,8},{1,64},{3,17},{7,7}}) {
        RandomBatch a(bounds[0],bounds[1],12345),b(bounds[0],bounds[1],12345);
        std::array<unsigned,65> counts{};
        for(unsigned i=0;i<200000;++i) {
            const auto x=a.Next();
            if(x<bounds[0]||x>bounds[1]||x!=b.Next()) {std::fputs("FAIL: bounds/replay\n",stderr);return 1;}
            ++counts[x];
        }
        const auto expected=200000/(bounds[1]-bounds[0]+1);
        for(unsigned i=bounds[0];i<=bounds[1];++i)
            if(counts[i]<expected*8/10||counts[i]>expected*12/10+1) {
                std::fputs("FAIL: deterministic distribution sanity\n",stderr);return 1;
            }
        a.Reset(999);b.Reset(999);
        for(unsigned i=0;i<257;++i)
            if(a.Next()!=b.Next()) {std::fputs("FAIL: phase reset\n",stderr);return 1;}
    }
    std::puts("PASS: golden random stream, bounds, replay, phase reset and distribution sanity");
    return 0;
}
