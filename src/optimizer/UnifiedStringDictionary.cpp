#include "duckdb/optimizer/UnifiedStringDictionary.h"

#include <cstring>
#include <algorithm>

namespace duckdb{

uint64_t UnifiedStringsDictionary::USSR_prefix = 3;


UnifiedStringsDictionary::UnifiedStringsDictionary() {
	DataRegion = make_uniq_array<uint64_t >(USSR_SIZE);
}




LinearProbingHashTable::LinearProbingHashTable(){
	LinearProbingHT = make_uniq_array_uninitialized<uint32_t>(USSR_SIZE);
	memset(LinearProbingHT.get(), '\0', USSR_SIZE * sizeof (uint32_t));
	currentEmptySlot = 0;
}

optional_idx LinearProbingHashTable::insert(uint32_t hashPrefix) {
	uint16_t slot;
	memcpy(&slot, &hashPrefix, 1U);

	uint16_t hashExtract;
	memcpy(&hashExtract, &hashPrefix + 1, 1U);


	// problematic, should use std::atomic::compare_exchange_strong

	uint32_t bucket = LinearProbingHT[slot];

	uint16_t storedHash;
	memcpy(&storedHash, &bucket, 1U);

	for (idx_t i = 0; i < PROBING_LIMIT; ++i) {
		if (storedHash == hashExtract){
			uint16_t slotIntoDataRegion;
			memcpy(&slotIntoDataRegion, &bucket + 1, 1U);

			return slotIntoDataRegion;
		}
	}


	return optional_idx();
}



}