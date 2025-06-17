#include "duckdb/optimizer/UnifiedStringDictionary.h"
#include "duckdb/common/types/hash.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
#include "duckdb/common/helper.hpp"
#include <cmath>

namespace duckdb {

UnifiedStringsDictionary::UnifiedStringsDictionary(idx_t size) {
	UnifiedStringDictionarySize = size;

	if (UnifiedStringDictionarySize == 0) {
		return;
	}

	auto extra_bits = static_cast<idx_t>(std::log(size) / std::log(2));

	required_bits += extra_bits;
	slot_bits += extra_bits;

	USSR_SIZE = size * 65536;
	HT_SIZE = USSR_SIZE;

	slot_mask = (1ULL << (slot_bits)) - 1ULL;
	salt_mask = ~slot_mask;

	buffer = make_unsafe_uniq_array_uninitialized<data_t>(USSR_SIZE * USSR_SLOT_SIZE + HT_SIZE * HT_BUCKET_SIZE);
	HT = reinterpret_cast<atomic<uint32_t >*> (buffer.get());
	DataRegion = reinterpret_cast<uint64_t *>(buffer.get() + HT_SIZE * HT_BUCKET_SIZE);
//	auto raw_base = cast_pointer_to_uint64(buffer.get());
//	auto data_region_start =
//	    (raw_base + USSR_SIZE * USSR_SLOT_SIZE + ((1ULL << required_bits) - 1)) & USSR_MASK; // round **up**
//
//	DataRegion   = reinterpret_cast<uint64_t *>(data_region_start);

//	USSR_prefix = cast_pointer_to_uint64(buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;
//
//	DataRegion = reinterpret_cast<uint64_t *>(USSR_prefix);

//	// Double check that the DataRegion is contained within the buffer
//	D_ASSERT(cast_pointer_to_uint64(buffer.get()) < cast_pointer_to_uint64(DataRegion));
//	D_ASSERT(cast_pointer_to_uint64(DataRegion) < cast_pointer_to_uint64(buffer.get()) + size * BUFFER_SIZE);
//	D_ASSERT(cast_pointer_to_uint64(DataRegion) + USSR_SIZE * USSR_SLOT_SIZE <=
//	         cast_pointer_to_uint64(buffer.get()) + size * BUFFER_SIZE);

//	data_ptr_t HT_address;
//	// The hash table can be either before or after the data region
//	if (USSR_prefix - cast_pointer_to_uint64(buffer.get()) >= HT_SIZE * HT_BUCKET_SIZE) {
//		HT_address = buffer.get();
//	} else {
//		HT_address = cast_uint64_to_pointer(USSR_prefix) + USSR_SIZE * USSR_SLOT_SIZE;
//	}
//
//		const auto buffer_start = cast_pointer_to_uint64(buffer.get());
//		const auto buffer_end   = buffer_start + size * BUFFER_SIZE;
//		const auto ht_start     = cast_pointer_to_uint64(HT_address);
//		const auto ht_end       = ht_start + HT_SIZE * HT_BUCKET_SIZE;
//	//
//	//	D_ASSERT(ht_start >= buffer_start);          // HT begins inside the buffer
	//	D_ASSERT(ht_end   <= buffer_end);            // HT ends   inside the buffer
	// We zero the hashtable, since we need an indicator if a bucket as been filled or not
	memset(buffer.get(), '\0', HT_SIZE * HT_BUCKET_SIZE);
//	Printer::Print(to_string(reinterpret_cast<uint64_t >(HT_address)));
//	HT = reinterpret_cast<atomic<uint32_t> *>(HT_address);

	currentEmptySlot.store(2);

	candidates = 0;
	accepted = 0;
	nRejections_Probing = 0;
	nRejections_SizeFull = 0;
	already_in = 0;

	failed_attempt = 0;
}

bool UnifiedStringsDictionary::CheckEqualityAndUpdatePtr(string_t &str, idx_t bucket_idx) {
	auto slot_ptr = data_ptr_cast(DataRegion + (HT[bucket_idx].load(std::memory_order_relaxed) & slot_mask));
	auto materialized_str_length = UnsafeNumericCast<uint16_t>(*reinterpret_cast<uint16_t *>(slot_ptr));
	if (materialized_str_length == str.GetSize() &&
	    memcmp(slot_ptr + STR_LENGTH_BYTES, str.GetDataUnsafe(), str.GetSize()) == 0) {
		str.SetPointer(AddTag(char_ptr_cast(slot_ptr + STR_LENGTH_BYTES)));
		return true;
	}
	return false;
}

bool UnifiedStringsDictionary::WaitUntilSlotResolves(idx_t bucket_idx) {
	while (true) {
		auto bucket = HT[bucket_idx].load(std::memory_order_acquire);
		if (bucket == 0) {
			return false; // means REJECTED_FULL
		}
		if ((bucket & slot_mask) != HT_DIRTY_SENTINEL) {
			return true;
		}
	}
}


InsertResult UnifiedStringsDictionary::insert(string_t &str) {
	// no support for inlined strings
	if (str.IsInlined() || str.GetSize() > MAX_STRING_LENGTH) {
		return InsertResult::INVALID;
	}

	// disable Unified string dictionary if passed attempt threshold to stop performance loss
	if (failed_attempt > ATTEMPT_THRESHOLD) {
		return InsertResult::REJECTED_FULL;
	}

	hash_t string_hash = Hash(str);
	uint32_t string_hash_prefix = Load<uint32_t>(reinterpret_cast<const_data_ptr_t>(&string_hash));

	uint32_t bucket_index = string_hash_prefix & slot_mask;
	uint32_t hash_salt = string_hash_prefix >> slot_bits;
	uint32_t dirty_bucket_value = (hash_salt << slot_bits) | HT_DIRTY_SENTINEL;

	D_ASSERT(bucket_index <= USSR_SIZE);

	for (idx_t i = 0; i < PROBING_LIMIT; i++) {
		idx_t prob_index = i;
		if (bucket_index + i >= USSR_SIZE) {
			prob_index = (bucket_index + i) % USSR_SIZE;
		}
		uint32_t HT_bucket = HT[bucket_index + prob_index].load(std::memory_order_acquire);
		uint32_t HT_bucket_salt = HT_bucket >> slot_bits;
		if (HT_bucket == 0) {
			// dirty the bucket
			uint32_t expected = 0;
			if (HT[bucket_index + prob_index].compare_exchange_strong(
			        expected, dirty_bucket_value, std::memory_order_release, std::memory_order_relaxed)) {
				auto total_bytes_needed = str.GetSize() + STR_LENGTH_BYTES + sizeof(hash_t);
				auto slots_needed =
				    (total_bytes_needed % 8 == 0) ? total_bytes_needed / 8 : 1 + (total_bytes_needed / 8);
				auto slot_to_insert = currentEmptySlot.fetch_add(slots_needed);
				if (slot_to_insert + slots_needed > USSR_SIZE ||
				    total_bytes_needed > (USSR_SIZE - slot_to_insert) * 8) {
					// give back the reserved slots
					currentEmptySlot.fetch_sub(slots_needed, std::memory_order_relaxed);
					// clear the dirtied bucket
					HT[bucket_index + prob_index].store(0, std::memory_order_release);
					return InsertResult::REJECTED_FULL;
				}
				uint32_t new_bucket = UnsafeNumericCast<uint32_t>(hash_salt);
				new_bucket = new_bucket << (slot_bits);
				new_bucket |= slot_to_insert;

				auto slot_ptr = data_ptr_cast(DataRegion + slot_to_insert);

				idx_t str_len = str.GetSize();
				memcpy(slot_ptr, &str_len, STR_LENGTH_BYTES);
				memcpy(slot_ptr + STR_LENGTH_BYTES, str.GetData(), str.GetSize());
				Store<uint64_t>(string_hash, slot_ptr - sizeof(hash_t));
				HT[bucket_index + prob_index].store(new_bucket, std::memory_order_release);
				str.SetPointer(AddTag(char_ptr_cast(slot_ptr + STR_LENGTH_BYTES)));
				return InsertResult::SUCCESS;
			} else { // lost the race to dirty the bucket, check if the dirt = HT_salt, if so wait, else continue
				     // probing
				if (expected == dirty_bucket_value) {
					// the thread that won is most likely inserting the same string, wait
					if(!WaitUntilSlotResolves(bucket_index + prob_index)){
						return InsertResult::REJECTED_FULL;
					}
					if (CheckEqualityAndUpdatePtr(str, bucket_index + prob_index)) {
						return InsertResult::ALREADY_EXISTS;
					} else {
						continue;
					}
				} else {
					continue;
				}
			}
		} else if (HT_bucket_salt == hash_salt &&
		           (HT_bucket & slot_mask) == HT_DIRTY_SENTINEL) { // dirtied but the salt matches, wait until the other
			                                                       // thread finishes, then check again
			if(!WaitUntilSlotResolves(bucket_index + prob_index)){
				return InsertResult::REJECTED_FULL;
			}
			if (CheckEqualityAndUpdatePtr(str, bucket_index + prob_index)) {
				return InsertResult::ALREADY_EXISTS;
			} else {
				continue;
			}
		} else if (HT_bucket_salt == hash_salt) { // the salt matches, string already exists, set the input string to
			// point into the unified string dictionary
			if (CheckEqualityAndUpdatePtr(str, bucket_index + prob_index)) {
				return InsertResult::ALREADY_EXISTS;
			} else {
				continue;
			}
		}
	}
	return InsertResult::REJECTED_PROBING;
}

void UnifiedStringsDictionary::UpdateFailedAttempts(idx_t n_failed) {
	failed_attempt += n_failed;
}

UnifiedStringsDictionary::~UnifiedStringsDictionary() {
	this->buffer.reset();
}

char *UnifiedStringsDictionary::AddTag(char *ptr) {
#ifndef DUCKDB_DISABLE_POINTER_SALT
	return reinterpret_cast<char *>(reinterpret_cast<uint64_t>(ptr) | string_t::UNIFIED_STRING_DICTIONARY_SALT_MASK);
#else
	return ptr;
#endif
}
} // namespace duckdb