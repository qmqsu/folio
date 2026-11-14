#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace folio::weread::crypto {
std::string EncodeId(const std::string&);
std::string Sign(const std::string&);
std::string SortedQuery(const std::vector<std::pair<std::string, std::string>>&);
std::vector<std::pair<std::string, std::string>> ContentParams(
    const std::string&, const std::string&, const std::string&, uint32_t now, uint32_t random);
bool DecodeShards(std::vector<std::string>*, std::string*, size_t limit);
std::string Sha256Hex(const std::string&);
}
