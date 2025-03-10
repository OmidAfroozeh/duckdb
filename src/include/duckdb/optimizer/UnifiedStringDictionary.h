#pragma once

#include <cstddef>

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/printer.hpp"

namespace duckdb {

static constexpr uint64_t BUFFER_SIZE = static_cast<const uint64_t>(1024 * 1024);

static constexpr uint64_t USSR_MASK = 0xFFFFFFFFFFF80000;
static constexpr uint64_t USSR_SLOT_SIZE = 8;

static constexpr uint64_t USSR_SIZE = 0xFFFF;
static constexpr uint64_t HT_SIZE = 0xFFFF;
// first two bytes are the slot number and the second two bytes are the hash extract
static constexpr uint64_t HT_BUCKET_SIZE = 4;

static constexpr idx_t PROBING_LIMIT = 3;


struct LinearProbingHashTable{
private:
	uint64_t currentEmptySlot;

	atomic<uint32_t> *HT_atomic;
	uint32_t * HT;

	// number of filled buckets in HT
	uint64_t nFullBuckets;

//	// every attempt on inserting a string
//	uint64_t candidates;
//	// accepted strings into the USSR
//	uint64_t accepted;
//
//	uint64_t nRejections_SizeFull;
//	uint64_t nRejection_LongChain;
public:
	explicit LinearProbingHashTable(data_ptr_t bufferHT);
	optional_idx insert(uint32_t hashPrefix, uint32_t len);

	optional_idx lookup(uint32_t hashPrefix);

	void getStatistics();



	std::mutex t;



};

class UnifiedStringsDictionary{
private:

	static UnifiedStringsDictionary* ussr_instance;


	unsafe_unique_array<data_t> buffer;
	uint64_t *DictionarySlot;

	unique_ptr<LinearProbingHashTable> LinearProbingHT;

	UnifiedStringsDictionary();

	static std::mutex singletonLock;

public:


	static UnifiedStringsDictionary* getInstance();
	static uint64_t USSR_prefix;

	string_t insert(const char * str, uint32_t len);
};



} // namespace duckdb