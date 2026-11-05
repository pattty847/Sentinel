// Offline exact-parser microbenchmark; no provider or recorded-data writes.
#include "levelstore/ExactDecimal.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
using namespace sentinel::levelstore;
int main(int argc,char** argv){
    if(argc!=2)return 2;
    const std::string values[]={"0","0.00000001","138991023.41","64900","100408.50665577","11959208.1","201951143801","90791.05454545","1.00000000000000001","9223372036854775807"};
    constexpr uint64_t repeats=1000000;uint64_t checksum=0;
    const auto begin=std::chrono::steady_clock::now();
    for(uint64_t n=0;n<repeats;++n)for(const auto& text:values){const auto parsed=parseDecimal(text);
        if(parsed.status!=DecimalStatus::Exact)return 1;checksum+=uint64_t(parsed.value.mantissa)^parsed.value.digits;}
    const auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count();
    nlohmann::json report={{"values",repeats*std::size(values)},{"nsPerValue",elapsed/(repeats*std::size(values))},{"checksum",checksum},{"samples",values}};
    std::ofstream(argv[1])<<report.dump(2)<<'\n';std::cout<<report.dump()<<'\n';
}
