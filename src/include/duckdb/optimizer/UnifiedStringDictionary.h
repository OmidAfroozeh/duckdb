#pragma once

#include <cstddef>

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

static constexpr uint64_t BUFFER_SIZE = static_cast<uint64_t>(1024 * 1024);

static constexpr uint64_t USSR_MASK = 0xFFFFFFFFFFF80000;

static constexpr uint64_t USSR_SLOT_SIZE = 8;
static constexpr uint64_t USSR_SIZE = 0xFFFF;

// first two bytes are the slot number into the data region
// and the second two bytes are the hash extract (a part of the original string's hash)
static constexpr uint64_t HT_BUCKET_SIZE = 4;
static constexpr uint64_t HT_SIZE = 0xFFFF;

static constexpr idx_t PROBING_LIMIT = 3;

//struct LinearProbingHashTable {
//private:
//	uint64_t currentEmptySlot;
//
//	uint32_t *HT;
//
//	// every attempt on inserting a string
//	uint64_t candidates;
//	// accepted strings into the USSR
//	uint64_t accepted;
//
//	uint64_t nRejections_SizeFull;
//	uint64_t nRejections_Probing;
//
//public:
//	explicit LinearProbingHashTable(data_ptr_t bufferHT);
//	optional_idx insert(uint32_t hashPrefix, uint32_t len);
//
//	optional_idx lookup(uint32_t hashPrefix);
//
//	void getStatistics();
//};

// Singleton
class UnifiedStringsDictionary {
private:
	static UnifiedStringsDictionary *ussr_instance;

	// Overarching USSR buffer, contains DataRegion + HT + extra, 1MB size
	unsafe_unique_array<data_t> buffer;
	// Start of the DataRegion
	uint64_t *DataRegion;
	uint32_t *HT;

	uint64_t currentEmptySlot;

#ifdef DEBUG
	// every attempt on inserting a string
	uint64_t candidates;
	// accepted strings into the USSR
	uint64_t accepted;

	uint64_t nRejections_SizeFull;
	uint64_t nRejections_Probing;
#endif
	// private constructor
	UnifiedStringsDictionary();

	// temporary solution for concurrency
	std::mutex insertLock;


	string_t insertInternal(string_t str);


public:
	static UnifiedStringsDictionary *getInstance();
	static uint64_t USSR_prefix;

	string_t insert(string_t str);

	static void destroy_UnifiedStrings();
	void getStatistics();

};

} // namespace duckdb
