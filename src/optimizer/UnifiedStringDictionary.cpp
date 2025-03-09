#include "duckdb/optimizer/UnifiedStringDictionary.h"

#include <cstring>
#include <algorithm>

namespace duckdb{

uint64_t UnifiedStringsDictionary::USSR_prefix = 3;
UnifiedStringsDictionary* UnifiedStringsDictionary::ussr_instance = nullptr;


void * round_up(void * ptr, size_t alignment) {
	return reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(ptr) + alignment - 1) & ~(alignment - 1));
}

UnifiedStringsDictionary::UnifiedStringsDictionary() {
	buffer = make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE);
	memset(buffer.get(), '\0', BUFFER_SIZE);
	USSR_prefix = cast_pointer_to_uint64( buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;

	DictionarySlot = reinterpret_cast<uint64_t *>(USSR_prefix);

	data_ptr_t HT_address;
	if(USSR_prefix - cast_pointer_to_uint64(buffer.get()) >= HT_SIZE * HT_BUCKET_SIZE){
		HT_address = buffer.get();
	} else{
		HT_address = cast_uint64_to_pointer(USSR_prefix) + USSR_SIZE * USSR_SLOT_SIZE;
	}


	LinearProbingHT = make_uniq<LinearProbingHashTable>(HT_address);

}

UnifiedStringsDictionary *UnifiedStringsDictionary::getInstance() {
	if(ussr_instance)
	static UnifiedStringsDictionary USSR;
	return &USSR;
}

string_t UnifiedStringsDictionary::insert(const char * str, uint32_t len) {
	hash_t h = Hash(str);
	uint32_t hashPrefix = Load<uint32_t>(const_data_ptr_cast(&h));
	auto res = LinearProbingHT.get()->insert(hashPrefix);
	if (res.IsValid()){
		auto slot = res.GetIndex();
		auto slot_ptr = DictionarySlot + slot * 8;
		memcpy(slot_ptr, str, len);
		memcpy(reinterpret_cast<void *>(slot_ptr[-1]), &h, 8);
		return string_t(const_char_ptr_cast(slot_ptr), len);
	}
	return nullptr;
}



LinearProbingHashTable::LinearProbingHashTable(data_ptr_t bufferHT){
	HT_atomic = reinterpret_cast<atomic<uint32_t>*>(bufferHT);
	HT = reinterpret_cast<uint32_t *>(bufferHT);
	currentEmptySlot = 0;
}

optional_idx LinearProbingHashTable::insert(uint32_t hashPrefix) {
	uint16_t slot;
	memcpy(&slot, &hashPrefix, 1U);

	uint16_t hashExtract;
	memcpy(&hashExtract, &hashPrefix + 1, 1U);


	for (idx_t i = 0; i < PROBING_LIMIT; ++i) {
		uint32_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + (4 * slot)+i));
		uint16_t bucket_hashExtract = UnsafeNumericCast<uint16_t >(bucket & 0xFFFF0000);

		if(bucket_hashExtract == hashExtract){
			return bucket & 0x0000FFFF;
		}

		if(bucket == 0){
			uint32_t expected = 0;
			uint32_t desired = UnsafeNumericCast<uint32_t >(hashExtract);
			desired = desired << 16;
			desired &= UnsafeNumericCast<uint32_t>(currentEmptySlot);

			if((HT_atomic + i)->compare_exchange_strong(expected, desired)){
				// if exchanged, return slot number
				return currentEmptySlot;
			}
		}
	}
	return optional_idx();
}

}// namespace duckdb