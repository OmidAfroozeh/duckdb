#include "duckdb/optimizer/UnifiedStringDictionary.h"

#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
namespace duckdb{

uint64_t UnifiedStringsDictionary::USSR_prefix = 3;
UnifiedStringsDictionary* UnifiedStringsDictionary::ussr_instance{nullptr};
std::mutex UnifiedStringsDictionary::singletonLock;


void * round_up(void * ptr, size_t alignment) {
	return reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(ptr) + alignment - 1) & ~(alignment - 1));
}

UnifiedStringsDictionary::UnifiedStringsDictionary() {
	buffer = make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE);
//	memset(buffer.get(), '\0', BUFFER_SIZE);
	USSR_prefix = cast_pointer_to_uint64( buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;

	DictionarySlot = reinterpret_cast<uint64_t *>(USSR_prefix);

	data_ptr_t HT_address;
	if(USSR_prefix - cast_pointer_to_uint64(buffer.get()) >= HT_SIZE * HT_BUCKET_SIZE){
		HT_address = buffer.get();
	} else{
		HT_address = cast_uint64_to_pointer(USSR_prefix) + USSR_SIZE * USSR_SLOT_SIZE;
	}

	memset(HT_address, '\0', HT_SIZE * HT_BUCKET_SIZE);
	LinearProbingHT = make_uniq<LinearProbingHashTable>(HT_address);

}

UnifiedStringsDictionary *UnifiedStringsDictionary::getInstance() {
	Printer printer;

//	printer.Print("TESTTTT");
	if(ussr_instance == nullptr){
		lock_guard<std::mutex> guard(singletonLock);
		if(ussr_instance == nullptr){
			ussr_instance = new UnifiedStringsDictionary();
			return ussr_instance;
		} else{
			return ussr_instance;
		}
	} else{
		return ussr_instance;
	}
}

string_t UnifiedStringsDictionary::insert(const char * str, uint32_t len) {
	hash_t h = Hash(str);
	uint32_t hashPrefix = Load<uint32_t>(const_data_ptr_cast(&h));

	auto lookup_res = LinearProbingHT.get()->lookup(hashPrefix);
	if(lookup_res.IsValid()){
		auto slot = lookup_res.GetIndex();
		auto slot_ptr = DictionarySlot + slot;
		return string_t(const_char_ptr_cast<uint64_t>(slot_ptr), len);
	}

	auto res = LinearProbingHT.get()->insert(hashPrefix, len);
	if (res.IsValid()){
		auto slot = res.GetIndex();
		auto slot_ptr = DictionarySlot + slot;
		memcpy(slot_ptr, str, len);
		memcpy(slot_ptr - 1, &h, 8);
		return string_t(const_char_ptr_cast<uint64_t>(slot_ptr), len);
//		return nullptr;
	}
	return string_t((uint32_t) 0);
}



LinearProbingHashTable::LinearProbingHashTable(data_ptr_t bufferHT){
	HT_atomic = reinterpret_cast<atomic<uint32_t>*>(bufferHT);
	HT = reinterpret_cast<uint32_t *>(bufferHT);
	currentEmptySlot = 1;
	nFullBuckets = 0;
//	nRejection_LongChain = 0;
//	nRejections_SizeFull = 0;
//	candidates = 0;
//	accepted =0;
}



optional_idx LinearProbingHashTable::insert(uint32_t hashPrefix, uint32_t len) {

//	candidates++;

	uint16_t slot;
	memcpy(&slot, &hashPrefix, 1U);

	uint16_t hashExtract = hashPrefix >> 16;


	for (idx_t i = 0; i < PROBING_LIMIT; ++i) {
		// loop around
		uint32_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + ((slot + i) % USSR_SIZE)));
		uint16_t bucket_hashExtract = bucket >> 16;

		if(bucket_hashExtract == hashExtract){
			return bucket & 0x0000FFFF;
		}

		if(bucket == 0){
			if(currentEmptySlot > USSR_SIZE){
//				nRejections_SizeFull++;
				return optional_idx();
			}
			uint32_t expected = 0;
			uint32_t desired = UnsafeNumericCast<uint32_t >(hashExtract);
			desired = desired << 16;
			desired |= UnsafeNumericCast<uint32_t>(currentEmptySlot);

			if((HT_atomic + ((slot + i) % USSR_SIZE))->compare_exchange_strong(expected, desired)){
//				accepted++;
				nFullBuckets++;
				auto ret = currentEmptySlot;
				// 1 slot for the pre-computed hash,
				currentEmptySlot += (len % 8 == 0) ? 1 + len / 8 : 2 + len / 8;
				// if exchanged, return slot number
				return ret;
			}
		}
	}
//	nRejection_LongChain++;
	return optional_idx();
}

optional_idx LinearProbingHashTable::lookup(uint32_t hashPrefix) {
	uint16_t slot;
	memcpy(&slot, &hashPrefix, 1U);

	uint16_t hashExtract = hashPrefix >> 16;

	for (idx_t i = 0; i < PROBING_LIMIT; ++i) {

		uint32_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + ((slot + i) % USSR_SIZE)));
		uint16_t bucket_hashExtract = bucket >> 16;

		if (bucket_hashExtract == hashExtract) {
			return bucket & 0x0000FFFF;
		}

		return optional_idx();
	}
}

void LinearProbingHashTable::getStatistics() {
//	Printer::Print()

}




}// namespace duckdb