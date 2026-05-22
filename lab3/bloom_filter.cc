#include "bloom_filter.h"

#include <algorithm>
#include <cstdint>

namespace {
size_t PositiveModulo(uint64_t value, size_t mod) {
  return static_cast<size_t>(value % static_cast<uint64_t>(mod));
}

uint64_t MixKey(int key, uint64_t seed) {
  uint64_t x = static_cast<uint32_t>(key);
  x ^= seed + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

uint64_t SimpleHash(int key, size_t hash_function_index) {
  const uint64_t seed =
      0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(hash_function_index + 1);
  return MixKey(key, seed);
}

void SetBit(std::vector<uint8_t>* bits, size_t index) {
  (*bits)[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

bool GetBit(const std::vector<uint8_t>& bits, size_t index) {
  return (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

char NibbleToHex(uint8_t value) {
  return static_cast<char>(value < 10 ? ('0' + value) : ('a' + value - 10));
}

bool HexToNibble(char c, uint8_t* value) {
  if (c >= '0' && c <= '9') {
    *value = static_cast<uint8_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *value = static_cast<uint8_t>(10 + c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    *value = static_cast<uint8_t>(10 + c - 'A');
    return true;
  }
  return false;
}
}  // namespace

size_t BloomByteSize(size_t bit_count) { return (bit_count + 7) / 8; }

std::string EncodeBloomFilterBits(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  // Store the bit array in a text SSTable by converting each byte to hex.
  for (uint8_t byte : bytes) {
    out.push_back(NibbleToHex(static_cast<uint8_t>(byte >> 4)));
    out.push_back(NibbleToHex(static_cast<uint8_t>(byte & 0x0F)));
  }
  return out;
}

bool DecodeBloomFilterBits(const std::string& encoded,
                           std::vector<uint8_t>* out) {
  if (encoded.size() % 2 != 0) {
    return false;
  }

  out->clear();
  out->reserve(encoded.size() / 2);
  // Restore the original bloom-filter bytes from the hex string in SSTable.
  for (size_t i = 0; i < encoded.size(); i += 2) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!HexToNibble(encoded[i], &hi) || !HexToNibble(encoded[i + 1], &lo)) {
      out->clear();
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

BloomFilter BuildBloomFilterFromKeys(const std::vector<int>& keys,
                                     size_t bits_per_key,
                                     size_t hash_count) {
  BloomFilter filter;
  if (keys.empty() || bits_per_key == 0 || hash_count == 0) {
    return filter;
  }

  filter.bit_count = std::max<size_t>(8, keys.size() * bits_per_key);
  filter.hash_count = hash_count;
  filter.bits.assign(BloomByteSize(filter.bit_count), 0);
  // Insert every key once so the finished filter can be written into SSTable.
  for (int key : keys) {
    filter.Add(key);
  }
  return filter;
}

void BloomFilter::Add(int key) {
  if (bit_count == 0 || hash_count == 0 || bits.empty()) {
    return;
  }

  // One logical key is mapped through multiple seeded hashes.
  for (size_t i = 0; i < hash_count; ++i) {
    SetBit(&bits, PositiveModulo(SimpleHash(key, i), bit_count));
  }
}

bool BloomFilter::MayContain(int key) const {
  if (bit_count == 0 || hash_count == 0 || bits.empty()) {
    return true;
  }

  // A key can exist only if every hash position is already set.
  for (size_t i = 0; i < hash_count; ++i) {
    if (!GetBit(bits, PositiveModulo(SimpleHash(key, i), bit_count))) {
      return false;
    }
  }
  return true;
}

bool BloomFilter::Empty() const { return bit_count == 0 || bits.empty(); }
