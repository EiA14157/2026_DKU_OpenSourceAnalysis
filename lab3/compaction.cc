#include "compaction.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
void RemoveSSTableFile(const SSTableFile& file) {
  std::filesystem::remove(file.path);
}
}  // namespace

std::optional<SSTableFile>
CompactAllSSTables(const std::string& sst_dir,
                   const std::vector<SSTableFile>& files,
                   uint64_t output_file_id, bool write_bloom_filter,
                   size_t bloom_bits_per_key, size_t bloom_hash_count) {
  if (files.empty()) {
    return std::nullopt;
  }

  // Read SSTables from newest to oldest so the first entry seen for a key is
  // the version that should survive compaction.
  std::map<int, SSTableEntry> latest;
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    auto entries = RangeScanSSTable(*it, std::numeric_limits<int>::min(),
                                    std::numeric_limits<int>::max());
    for (const auto& entry : entries) {
      if (latest.find(entry.key) != latest.end()) {
        continue;
      }
      latest[entry.key] = entry;
    }
  }

  std::vector<SSTableEntry> compacted_entries;
  compacted_entries.reserve(latest.size());
  for (const auto& kv : latest) {
    // Final tombstones are dropped because compaction sees the full file set.
    if (!kv.second.tombstone) {
      compacted_entries.push_back(kv.second);
    }
  }

  std::optional<SSTableFile> compacted_file;
  if (!compacted_entries.empty()) {
    compacted_file = WriteSSTable(sst_dir, output_file_id, compacted_entries,
                                  write_bloom_filter, bloom_bits_per_key,
                                  bloom_hash_count);
  }

  // Replace all old SSTables only after the new compacted file is ready.
  for (const auto& file : files) {
    RemoveSSTableFile(file);
  }

  return compacted_file;
}
