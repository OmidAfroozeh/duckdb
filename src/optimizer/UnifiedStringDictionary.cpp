#include "duckdb/optimizer/UnifiedStringDictionary.h"

#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
namespace duckdb {

uint64_t UnifiedStringsDictionary::USSR_prefix = 0;
UnifiedStringsDictionary *UnifiedStringsDictionary::ussr_instance {nullptr};
std::mutex UnifiedStringsDictionary::singletonLock;

UnifiedStringsDictionary::UnifiedStringsDictionary() {

	buffer = make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE);
	//	memset(buffer.get(), '\0', BUFFER_SIZE);
	USSR_prefix = cast_pointer_to_uint64(buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;

	DataRegion = reinterpret_cast<uint64_t *>(USSR_prefix);

	// Double check that the DataRegion is contained within the buffer
	D_ASSERT(cast_pointer_to_uint64(buffer.get()) < cast_pointer_to_uint64(DataRegion));
	D_ASSERT(cast_pointer_to_uint64(DataRegion) < cast_pointer_to_uint64(buffer.get()) + BUFFER_SIZE);
	D_ASSERT(cast_pointer_to_uint64(DataRegion) + USSR_SIZE * USSR_SLOT_SIZE <=
	         cast_pointer_to_uint64(buffer.get()) + BUFFER_SIZE);

	data_ptr_t HT_address;
	// The hash table can be either before or after the data region
	if (USSR_prefix - cast_pointer_to_uint64(buffer.get()) >= HT_SIZE * HT_BUCKET_SIZE) {
		HT_address = buffer.get();
	} else {
		HT_address = cast_uint64_to_pointer(USSR_prefix) + USSR_SIZE * USSR_SLOT_SIZE;
	}

	// We zero the hashtable, since we need an indicator if a bucket as been filled or not
	memset(HT_address, '\0', HT_SIZE * HT_BUCKET_SIZE);
	LinearProbingHT = make_uniq<LinearProbingHashTable>(HT_address);
}

UnifiedStringsDictionary *UnifiedStringsDictionary::getInstance() {
	if (ussr_instance == nullptr) {
		lock_guard<std::mutex> guard(singletonLock);
		if (ussr_instance == nullptr) {
			//			Printer::Print("USSR CREATED");
			ussr_instance = new UnifiedStringsDictionary();
			return ussr_instance;
		} else {
			return ussr_instance;
		}
	} else {
		return ussr_instance;
	}
}

string_t UnifiedStringsDictionary::insert(string_t str) {
	// no support for short strings now
	if(str.IsInlined()){
		return str;
	}

	lock_guard<std::mutex> guard(insertLock);

	hash_t h = Hash(str);
	uint32_t hashPrefix = Load<uint32_t>(const_data_ptr_cast(&h));

	auto lookup_res = LinearProbingHT.get()->lookup(hashPrefix);
	if (lookup_res.IsValid()) {
		auto slot = lookup_res.GetIndex();
		auto slot_ptr = DataRegion + slot;
		// double checking that the string found is equal to the original string
		auto res_str = string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t >(str.GetSize()));
		return (res_str == str) ? res_str : str;
	}

	auto res = LinearProbingHT.get()->insert(hashPrefix, UnsafeNumericCast<uint32_t >(str.GetSize()));
	if (res.IsValid()) {
		auto slot = res.GetIndex();
		auto slot_ptr = DataRegion + slot;

		D_ASSERT(cast_pointer_to_uint64(slot_ptr) > cast_pointer_to_uint64(DataRegion));
		D_ASSERT(cast_pointer_to_uint64(slot_ptr) < cast_pointer_to_uint64(DataRegion) + USSR_SIZE * USSR_SLOT_SIZE);

		memcpy(slot_ptr, str.GetData(),  str.GetSize());
		memcpy(slot_ptr - 1, &h, 8);
		return string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t >(str.GetSize()));
	}

	return str;
}

LinearProbingHashTable::LinearProbingHashTable(data_ptr_t bufferHT) {
	//	HT_atomic = reinterpret_cast<atomic<uint32_t>*>(bufferHT);
	HT = reinterpret_cast<uint32_t *>(bufferHT);
	currentEmptySlot = 1;
	//	nFullBuckets = 0;
}

optional_idx LinearProbingHashTable::insert(uint32_t hashPrefix, uint32_t len) {
	// reject if not enough space left
	auto remaining = (USSR_SIZE - 1 - currentEmptySlot) * 8;
	if (len > remaining) {
		return optional_idx();
	}

	uint16_t slot;
	memcpy(&slot, &hashPrefix, 2U);

	uint16_t hashExtract = hashPrefix >> 16;

	for (idx_t i = 0; i < PROBING_LIMIT; i++) {
		if (slot + i > USSR_SIZE) {
			return optional_idx();
		}

		uint32_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + ((slot + i))));

		uint16_t bucket_hashExtract = bucket >> 16;

		if (bucket_hashExtract == hashExtract) {
			auto res = bucket & 0x0000FFFF;
			// the hashExtract could be zero,
			// we also need to check that the slot is also zero to 100% be sure that this is not filled
			if (res != 0) {
				optional_idx(bucket & 0x0000FFFF);
			}
		}

		if (bucket == 0) {
			auto increasedSlot = (len % 8 == 0) ? 1 + (len / 8) : 2 + (len / 8);

			// FIXME: probably can remove this check!
			if (currentEmptySlot + increasedSlot >= USSR_SIZE) {
				//				nRejections_SizeFull++;
				return optional_idx();
			}
//			uint32_t expected = 0;
			uint32_t desired = UnsafeNumericCast<uint32_t>(hashExtract);
			desired = desired << 16;
			desired |= UnsafeNumericCast<uint32_t>(currentEmptySlot);
			D_ASSERT((desired & 0x0000FFFF) == currentEmptySlot);

			//			if((HT_atomic + ((slot + i)))->compare_exchange_strong(expected, desired)){
			//				accepted++;
			HT[slot + i] = desired;
			auto ret = currentEmptySlot;

			// 1 slot for the pre-computed hash,
			currentEmptySlot += increasedSlot;
			D_ASSERT(ret < currentEmptySlot);
			//			    Printer::Print(std::to_string(ret));
			// if exchanged, return slot number
			return optional_idx(ret);
			//			}
		}
	}
	//	nRejection_LongChain++;
	return optional_idx();
}

optional_idx LinearProbingHashTable::lookup(uint32_t hashPrefix) {
	uint16_t slot;
	memcpy(&slot, &hashPrefix, 2U);

	uint16_t hashExtract = hashPrefix >> 16;

	for (idx_t i = 0; i < PROBING_LIMIT; i++) {

		uint32_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + ((slot + i))));
		uint16_t bucket_hashExtract = bucket >> 16;

		if (bucket == 0) {
			return optional_idx();
		}
		if (bucket_hashExtract == hashExtract) {
			return optional_idx(bucket & 0x0000FFFF);
		}
	}
	return optional_idx();
}

void LinearProbingHashTable::getStatistics() {
	//	Printer::Print()
}

} // namespace duckdb
