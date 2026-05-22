#include "sstable.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
struct SSTableHeader {
  bool has_metadata = false;
  int smallest_key = 0;
  int largest_key = 0;
  int64_t oldest_seq = 0;
  int64_t newest_seq = 0;
  bool has_bloom_filter = false;
  BloomFilter bloom_filter;
};

BloomFilter BuildBloomFilter(const std::vector<SSTableEntry>& entries,
                             size_t bits_per_key, size_t hash_count) {
  std::vector<int> keys;
  keys.reserve(entries.size());
  for (const auto& entry : entries) {
    keys.push_back(entry.key);
  }
  return BuildBloomFilterFromKeys(keys, bits_per_key, hash_count);
}

bool ReadSSTableHeader(std::ifstream* in, SSTableHeader* header) {
  std::string line;
  if (!std::getline(*in, line)) {
    return false;
  }

  // The first line always stores the key/sequence-number range metadata.
  std::istringstream meta(line);
  std::string tag;
  int bloom_enabled = 0;
  if (!(meta >> tag >> bloom_enabled >> header->smallest_key >>
        header->largest_key >> header->oldest_seq >> header->newest_seq)) {
    return false;
  }
  if (tag != "META") {
    return false;
  }

  header->has_metadata = true;
  header->has_bloom_filter = (bloom_enabled != 0);
  if (!header->has_bloom_filter) {
    return true;
  }

  if (!std::getline(*in, line)) {
    return false;
  }

  // When bloom filter is enabled, the second line stores its shape and bits.
  std::istringstream bloom(line);
  std::string bloom_tag;
  std::string encoded_bits;
  if (!(bloom >> bloom_tag >> header->bloom_filter.bit_count >>
        header->bloom_filter.hash_count >> encoded_bits)) {
    return false;
  }
  if (bloom_tag != "BLOOM") {
    return false;
  }
  if (!DecodeBloomFilterBits(encoded_bits, &header->bloom_filter.bits)) {
    return false;
  }
  return true;
}

bool ParseSSTableDataLine(const std::string& line, SSTableEntry* entry) {
  std::istringstream iss(line);
  std::string op;
  if (!(iss >> op)) {
    return false;
  }

  if (op == "P") {
    // Put entries keep key, sequence number, and value.
    if (!(iss >> entry->key >> entry->seq >> entry->value)) {
      return false;
    }
    entry->tombstone = false;
    return true;
  }

  if (op == "D") {
    // Delete entries keep only key and sequence number.
    if (!(iss >> entry->key >> entry->seq)) {
      return false;
    }
    entry->value.clear();
    entry->tombstone = true;
    return true;
  }

  return false;
}

std::optional<uint64_t> ParseSSTFileId(const std::string& name) {
  const std::string prefix = "sst_";
  const std::string suffix = ".txt";
  if (name.size() <= prefix.size() + suffix.size()) {
    return std::nullopt;
  }
  if (name.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  if (name.substr(name.size() - suffix.size()) != suffix) {
    return std::nullopt;
  }
  const std::string number = name.substr(
      prefix.size(), name.size() - prefix.size() - suffix.size());
  if (number.empty()) {
    return std::nullopt;
  }
  for (char c : number) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
  }
  return static_cast<uint64_t>(std::stoull(number));
}

SSTableHeader BuildHeaderFromEntries(const std::vector<SSTableEntry>& entries,
                                     bool has_bloom_filter,
                                     BloomFilter bloom_filter) {
  SSTableHeader header;
  header.has_bloom_filter = has_bloom_filter;
  header.bloom_filter = std::move(bloom_filter);
  if (entries.empty()) {
    throw std::invalid_argument("cannot write empty SSTable");
  }

  header.has_metadata = true;
  header.smallest_key = entries.front().key;
  header.largest_key = entries.front().key;
  header.oldest_seq = entries.front().seq;
  header.newest_seq = entries.front().seq;
  for (const auto& entry : entries) {
    header.smallest_key = std::min(header.smallest_key, entry.key);
    header.largest_key = std::max(header.largest_key, entry.key);
    header.oldest_seq = std::min(header.oldest_seq, entry.seq);
    header.newest_seq = std::max(header.newest_seq, entry.seq);
  }
  return header;
}
}  // namespace

void EnsureSSTDir(const std::string& sst_dir) {
  std::filesystem::create_directories(sst_dir);
}

bool IsSSTDirEmpty(const std::string& sst_dir) {
  EnsureSSTDir(sst_dir);
  return std::filesystem::directory_iterator(sst_dir) ==
         std::filesystem::directory_iterator();
}

std::vector<SSTableFile> ListSSTables(const std::string& sst_dir,
                                      bool load_bloom_filter) {
  EnsureSSTDir(sst_dir);
  std::vector<SSTableFile> out;

  for (const auto& entry : std::filesystem::directory_iterator(sst_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    // Only files that match sst_<id>.txt are part of the database state.
    auto file_id = ParseSSTFileId(entry.path().filename().string());
    if (!file_id.has_value()) {
      continue;
    }

    SSTableFile file;
    file.id = *file_id;
    file.path = entry.path().string();

    std::ifstream in(file.path);
    if (!in.is_open()) {
      throw std::runtime_error("failed to open SSTable: " + file.path);
    }

    SSTableHeader header;
    if (!ReadSSTableHeader(&in, &header) || !header.has_metadata) {
      throw std::runtime_error("failed to read SSTable header: " + file.path);
    }

    file.smallest_key = header.smallest_key;
    file.largest_key = header.largest_key;
    file.oldest_seq = header.oldest_seq;
    file.newest_seq = header.newest_seq;
    file.has_bloom_filter = header.has_bloom_filter;
    if (load_bloom_filter && header.has_bloom_filter) {
      file.bloom_filter = std::move(header.bloom_filter);
    } else if (header.has_bloom_filter) {
      file.bloom_filter.bit_count = header.bloom_filter.bit_count;
      file.bloom_filter.hash_count = header.bloom_filter.hash_count;
    }

    out.push_back(std::move(file));
  }

  std::sort(out.begin(), out.end(),
            [](const SSTableFile& a, const SSTableFile& b) {
              return a.id < b.id;
            });
  return out;
}

SSTableFile WriteSSTable(const std::string& sst_dir, uint64_t file_id,
                         const std::vector<SSTableEntry>& entries,
                         bool write_bloom_filter, size_t bloom_bits_per_key,
                         size_t bloom_hash_count) {
  EnsureSSTDir(sst_dir);

  SSTableFile file;
  file.id = file_id;
  file.path =
      (std::filesystem::path(sst_dir) / ("sst_" + std::to_string(file_id) + ".txt"))
          .string();

  BloomFilter bloom_filter;
  if (write_bloom_filter) {
    bloom_filter = BuildBloomFilter(entries, bloom_bits_per_key, bloom_hash_count);
  }
  SSTableHeader header =
      BuildHeaderFromEntries(entries, write_bloom_filter, std::move(bloom_filter));

  // SSTable format is text-based:
  // META ...
  // BLOOM ...   (optional)
  // P/D ...     (one line per newest visible key in the flushed memtable)
  std::ofstream out(file.path, std::ios::trunc);
  if (!out.is_open()) {
    throw std::runtime_error("failed to create SSTable: " + file.path);
  }

  out << "META " << (header.has_bloom_filter ? 1 : 0) << " "
      << header.smallest_key << " " << header.largest_key << " "
      << header.oldest_seq << " " << header.newest_seq << "\n";
  if (header.has_bloom_filter) {
    out << "BLOOM " << header.bloom_filter.bit_count << " "
        << header.bloom_filter.hash_count << " "
        << EncodeBloomFilterBits(header.bloom_filter.bits) << "\n";
  }
  for (const auto& entry : entries) {
    if (entry.tombstone) {
      out << "D " << entry.key << " " << entry.seq << "\n";
    } else {
      out << "P " << entry.key << " " << entry.seq << " " << entry.value << "\n";
    }
  }
  out.close();

  file.smallest_key = header.smallest_key;
  file.largest_key = header.largest_key;
  file.oldest_seq = header.oldest_seq;
  file.newest_seq = header.newest_seq;
  file.has_bloom_filter = header.has_bloom_filter;
  file.bloom_filter = std::move(header.bloom_filter);
  return file;
}

bool GetFromSSTable(const SSTableFile& file, int key, std::string* value,
                    bool* tombstone) {
  if (key < file.smallest_key || key > file.largest_key) {
    return false;
  }
  // Bloom filter can reject definitely-missing keys before disk scan.
  if (file.has_bloom_filter && !file.bloom_filter.Empty() &&
      !file.bloom_filter.MayContain(key)) {
    return false;
  }

  std::ifstream in(file.path);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open SSTable: " + file.path);
  }

  SSTableHeader header;
  if (!ReadSSTableHeader(&in, &header)) {
    throw std::runtime_error("failed to read SSTable header: " + file.path);
  }

  std::string line;
  while (std::getline(in, line)) {
    SSTableEntry entry;
    if (!ParseSSTableDataLine(line, &entry)) {
      continue;
    }
    // Entries are sorted by key, so search can stop once we pass the target.
    if (entry.key == key) {
      if (value != nullptr) {
        *value = entry.value;
      }
      if (tombstone != nullptr) {
        *tombstone = entry.tombstone;
      }
      return true;
    }
    if (entry.key > key) {
      return false;
    }
  }
  return false;
}

std::vector<SSTableEntry> RangeScanSSTable(const SSTableFile& file,
                                           int start_key, int end_key) {
  std::vector<SSTableEntry> out;
  if (start_key > end_key || end_key < file.smallest_key ||
      start_key > file.largest_key) {
    return out;
  }

  std::ifstream in(file.path);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open SSTable: " + file.path);
  }

  SSTableHeader header;
  if (!ReadSSTableHeader(&in, &header)) {
    throw std::runtime_error("failed to read SSTable header: " + file.path);
  }

  std::string line;
  while (std::getline(in, line)) {
    SSTableEntry entry;
    if (!ParseSSTableDataLine(line, &entry)) {
      continue;
    }
    // Because SSTable data is ordered by key, range scan can skip the prefix
    // and stop as soon as the upper bound is exceeded.
    if (entry.key < start_key) {
      continue;
    }
    if (entry.key > end_key) {
      break;
    }
    out.push_back(std::move(entry));
  }

  return out;
}
