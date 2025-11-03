#include "includes/hermes_core.h"

void PrintSchemas() {
    std::cout << "[SCHEMA] 7202: Token,Type,MarketType,OI\n";
    std::cout << "[SCHEMA] 7208: "
        << "Token,Type,LTP,ATP,BDP,BDQ,ASP,ASQ,BQ,SQ,Time,"
        << "B1P,B1Q,B2P,B2Q,B3P,B3Q,B4P,B4Q,B5P,B5Q,"
        << "A1P,A1Q,A2P,A2Q,A3P,A3Q,A4P,A4Q,A5P,A5Q\n";
}
