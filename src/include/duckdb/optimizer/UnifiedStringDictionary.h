#pragma once

#include "duckdb/common/typedefs.hpp"

namespace duckdb {

static constexpr uint64_t USSR_MASK = 0xFFFFFFFFFFF80000;
static constexpr uint16_t USSR_SIZE = 0xFFFF;
static constexpr idx_t PROBING_LIMIT = 3;


struct LinearProbingHashTable{
private:
	uint16_t currentEmptySlot;
	unique_array<uint32_t> LinearProbingHT;

public:
	LinearProbingHashTable();
	optional_idx insert(uint32_t hashPrefix);






};

class UnifiedStringsDictionary{
private:

	unique_array<uint64_t> DataRegion;


public:

	UnifiedStringsDictionary();

	static uint64_t USSR_prefix;
};



} // namespace duckdb