#pragma once

#include <cstddef>

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

enum class InsertResult {
	// string is inserted for the first time
	SUCCESS,
	// string already exists and is returned
	ALREADY_EXISTS,
	// insertion failure, maximum capacity for new strings
	REJECTED_FULL,
	// insertion failure, going over the probing limit
	REJECTED_PROBING,
	// string not suitable for dictionary due to its size
	INVALID
};

class UnifiedStringsDictionary {
public:
	static constexpr const uint64_t ATTEMPT_THRESHOLD = 1000;

	static constexpr const uint64_t MAX_STRING_LENGTH = 512;

	static constexpr const uint64_t BUFFER_SIZE = static_cast<uint64_t>(1024 * 1024);

	uint64_t USSR_MASK;
	uint64_t USSR_prefix;

	static constexpr const uint64_t USSR_SLOT_SIZE = 8;
	idx_t USSR_SIZE;

	// first two bytes are the slot number into the data region
	// and the second two bytes are the hash extract (a part of the original string's hash)
	static constexpr const uint64_t HT_BUCKET_SIZE = 4;
	idx_t HT_SIZE;

	static constexpr const idx_t PROBING_LIMIT = 32;

	idx_t slot_bits = 16;
	uint64_t slot_mask;
	uint64_t salt_mask;

	idx_t required_bits = 19;

	static constexpr const idx_t STR_LENGTH_BYTES = 2;

	idx_t UnifiedStringDictionarySize;

private:
	// Overarching USSR buffer, contains DataRegion + HT + extra, 1MB size
	unsafe_unique_array<data_t> buffer;
	// Start of the DataRegion
	uint64_t *DataRegion;

	// temporary solution for concurrency
	std::mutex insertLock;

	atomic<uint64_t> currentEmptySlot;

	atomic<uint32_t> *HT;

	// every attempt on inserting a string
	atomic<uint64_t> candidates;
	// accepted strings into the USSR
	atomic<uint64_t> accepted;

	atomic<uint64_t> already_in;

	atomic<uint64_t> nRejections_SizeFull;
	atomic<uint64_t> nRejections_Probing;

	idx_t failed_attempt;

	static constexpr const idx_t HT_DIRTY_SENTINEL = 1;

public:
	UnifiedStringsDictionary(idx_t size);

	~UnifiedStringsDictionary();

	void UpdateFailedAttempts(idx_t n_failed);

	InsertResult insert(string_t &str);

private:
	bool WaitForSlotResolution(uint32_t *HT_bucket_ptr);

	char *AddTag(char *ptr);
	void getStatistics();
	bool CheckEqualityAndUpdatePtr(string_t &str, idx_t bucket_idx);
	bool WaitUntilSlotResolves(idx_t bucket_idx);
};

} // namespace duckdb
