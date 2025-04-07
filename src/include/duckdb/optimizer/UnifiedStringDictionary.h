#pragma once

#include <cstddef>

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

// Singleton
class UnifiedStringsDictionary {
public:
	static constexpr uint64_t BUFFER_SIZE = static_cast<uint64_t>(1024 * 1024);

	static constexpr uint64_t USSR_MASK = 0xFFFFFFFFFFF80000;

	static constexpr uint64_t USSR_SLOT_SIZE = 8;
	static constexpr uint64_t USSR_SIZE = 0xFFFF;

	// first two bytes are the slot number into the data region
	// and the second two bytes are the hash extract (a part of the original string's hash)
	static constexpr uint64_t HT_BUCKET_SIZE = 4;
	static constexpr uint64_t HT_SIZE = 0xFFFF;

	static constexpr idx_t PROBING_LIMIT = 3;

private:
	// Overarching USSR buffer, contains DataRegion + HT + extra, 1MB size
	unsafe_unique_array<data_t> buffer;
	// Start of the DataRegion
	uint64_t *DataRegion;

	// temporary solution for concurrency
	std::mutex insertLock;

	uint64_t currentEmptySlot;

	uint32_t *HT;

	// every attempt on inserting a string
	atomic<uint64_t> candidates;
	// accepted strings into the USSR
	atomic<uint64_t> accepted;

	atomic<uint64_t> already_in;

	atomic<uint64_t> nRejections_SizeFull;
	atomic<uint64_t> nRejections_Probing;


public:
	UnifiedStringsDictionary();
	~UnifiedStringsDictionary();
	uint64_t USSR_prefix;

	string_t insert(string_t str);

private:
	string_t insertInternal(string_t str, hash_t hash = 0);

	void getStatistics();

};

} // namespace duckdb
