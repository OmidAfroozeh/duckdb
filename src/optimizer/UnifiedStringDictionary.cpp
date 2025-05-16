#include "duckdb/optimizer/UnifiedStringDictionary.h"
#include "duckdb/common/types/hash.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
#include <cmath>

namespace duckdb {

//UnifiedStringsDictionary::UnifiedStringsDictionary() {
//
//	buffer = make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE);
//	USSR_prefix = cast_pointer_to_uint64(buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;
//
//	DataRegion = reinterpret_cast<uint64_t *>(USSR_prefix);
//
//	// Double check that the DataRegion is contained within the buffer
//	D_ASSERT(cast_pointer_to_uint64(buffer.get()) < cast_pointer_to_uint64(DataRegion));
//	D_ASSERT(cast_pointer_to_uint64(DataRegion) < cast_pointer_to_uint64(buffer.get()) + BUFFER_SIZE);
//	D_ASSERT(cast_pointer_to_uint64(DataRegion) + USSR_SIZE * USSR_SLOT_SIZE <=
//	         cast_pointer_to_uint64(buffer.get()) + BUFFER_SIZE);
//
//	data_ptr_t HT_address;
//	// The hash table can be either before or after the data region
//	if (USSR_prefix - cast_pointer_to_uint64(buffer.get()) >= HT_SIZE * HT_BUCKET_SIZE) {
//		HT_address = buffer.get();
//	} else {
//		HT_address = cast_uint64_to_pointer(USSR_prefix) + USSR_SIZE * USSR_SLOT_SIZE;
//	}
//
//	// We zero the hashtable, since we need an indicator if a bucket as been filled or not
//	memset(HT_address, '\0', HT_SIZE * HT_BUCKET_SIZE);
//
//	HT = reinterpret_cast<uint32_t *>(HT_address);
//
//	currentEmptySlot = 1;
//
//	candidates = 0;
//	accepted = 0;
//	nRejections_Probing = 0;
//	nRejections_SizeFull = 0;
//	already_in = 0;
//}

uint64_t UnifiedStringsDictionary::USSR_MASK{0};

UnifiedStringsDictionary::UnifiedStringsDictionary(idx_t size) {
	required_bits += static_cast<idx_t > (std::log(size) / std::log(2));
	// base size is 512kB, I need to calculate how many bits do I need to given the size,
	// size 1 = 512kb -> 16bits (to slot into a  64k 8 bytes)
	// size 2 = 1024kB -> 17 bits (
	// size 3 = 2mB -> 18 bits

	USSR_MASK = ~((1ULL << (required_bits)) - 1);
	USSR_SIZE = size * 0xFFFF;
	HT_SIZE = USSR_SIZE;


	slot_mask = (1ULL << (required_bits-3)) - 1ULL;

	buffer = make_unsafe_uniq_array_uninitialized<data_t>( (size * 2) * BUFFER_SIZE);
	USSR_prefix = cast_pointer_to_uint64(buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;

	DataRegion = reinterpret_cast<uint64_t *>(USSR_prefix);

	// Double check that the DataRegion is contained within the buffer
	D_ASSERT(cast_pointer_to_uint64(buffer.get()) < cast_pointer_to_uint64(DataRegion));
	D_ASSERT(cast_pointer_to_uint64(DataRegion) < cast_pointer_to_uint64(buffer.get()) + (size * 2) * BUFFER_SIZE);
	D_ASSERT(cast_pointer_to_uint64(DataRegion) + USSR_SIZE * USSR_SLOT_SIZE <=
	         cast_pointer_to_uint64(buffer.get()) + (size * 2) * BUFFER_SIZE);

	data_ptr_t HT_address;
	// The hash table can be either before or after the data region
	if (USSR_prefix - cast_pointer_to_uint64(buffer.get()) >= HT_SIZE * HT_BUCKET_SIZE) {
		HT_address = buffer.get();
	} else {
		HT_address = cast_uint64_to_pointer(USSR_prefix) + USSR_SIZE * USSR_SLOT_SIZE;
	}

	// We zero the hashtable, since we need an indicator if a bucket as been filled or not
	memset(HT_address, '\0', HT_SIZE * HT_BUCKET_SIZE);

	HT = reinterpret_cast<uint64_t *>(HT_address);

	currentEmptySlot = 1;

	candidates = 0;
	accepted = 0;
	nRejections_Probing = 0;
	nRejections_SizeFull = 0;
	already_in = 0;
}


string_t UnifiedStringsDictionary::insert(string_t str) {
	// no support for short strings now
	if (str.IsInlined() || str.GetSize() > MAX_STRING_LENGTH) {
		return str;
	}

	return insertInternal(str);
}

string_t UnifiedStringsDictionary::insertInternal(string_t str) {

	//	if (nRejections_SizeFull.load(std::memory_order_relaxed) > ATTEMPT_THRESHOLD + 1000000) {
	//		return str;
	//	}

	hash_t h = Hash(str);

//	uint32_t hashPrefix = Load<uint32_t>(const_data_ptr_cast(&h));

//		candidates++;

	uint64_t slot;
	memcpy(&slot, &h, slot_size);
	slot = slot & slot_mask;
	D_ASSERT(slot <= USSR_SIZE);

	uint16_t hashExtract = h >> slot_size * 8;

	for (idx_t i = 0; i < PROBING_LIMIT + 16; i++) {
		idx_t prob_index = i;
		// currently no looping around
		if (slot + i > USSR_SIZE) {
			prob_index = (slot + i) % USSR_SIZE;
//						nRejections_Probing++;
//			return str;
		}

		uint64_t bucket = reinterpret_cast<atomic<uint64_t > *>(HT + ((slot + prob_index)))->load(std::memory_order_acquire);

		uint16_t bucket_hashExtract = bucket >> (slot_size * 8);

		if (bucket_hashExtract == hashExtract) {
			// wrong already_in, do this after checking if equal
//						already_in++;
			auto slot_ptr = data_ptr_cast(DataRegion + (bucket & slot_mask));
			// double checking that the string found is equal to the original string

			auto res_str = string_t(const_char_ptr_cast(slot_ptr + 1), UnsafeNumericCast<uint32_t>(*slot_ptr));
			return (res_str == str) ? res_str : str;
		}

		if (bucket == 0) {
			auto str_len = str.GetSize() + 1;
			std::lock_guard<std::mutex> guard(insertLock);

			// reject if not enough space left
			auto remaining = (USSR_SIZE - currentEmptySlot) * 8;
			if (str_len > remaining || currentEmptySlot > USSR_SIZE) {
//								nRejections_SizeFull++;
				return str;
			}

			auto increasedSlot = (str_len % 8 == 0) ? 1 + (str_len / 8) : 2 + (str_len / 8);

			uint64_t newBucket = UnsafeNumericCast<uint64_t >(hashExtract);
			newBucket = newBucket << (slot_size * 8);
			newBucket |= currentEmptySlot;

			D_ASSERT((newBucket & slot_mask) == currentEmptySlot);

			// another thread inserted
			if (HT[slot + prob_index] != 0) {
				auto slot_hashExtract = HT[slot + prob_index] >> (slot_size * 8);
				if (slot_hashExtract == hashExtract) {
//										already_in++;
					auto slot_ptr = data_ptr_cast(DataRegion + (HT[slot + prob_index] & slot_mask));

					auto res_str = string_t(const_char_ptr_cast(slot_ptr + 1), UnsafeNumericCast<uint32_t>(*slot_ptr));
					if (res_str == str) {
						return res_str;
					} else {
						continue;
					}
				} else {
					continue;
				}
			}

//						accepted++;
			auto ret = currentEmptySlot;
			// 1 slot for the pre-computed hash,
			currentEmptySlot += increasedSlot;
			D_ASSERT(ret < currentEmptySlot);
			auto slot_ptr = data_ptr_cast(DataRegion + ret);

			D_ASSERT(cast_pointer_to_uint64(slot_ptr) > cast_pointer_to_uint64(DataRegion));
			D_ASSERT(cast_pointer_to_uint64(slot_ptr) <
			         cast_pointer_to_uint64(DataRegion + USSR_SIZE * USSR_SLOT_SIZE));

			memset(slot_ptr, UnsafeNumericCast<uint8_t>(str.GetSize()), 1);
			memcpy(slot_ptr + 1, str.GetData(), str.GetSize());
			memcpy(slot_ptr - 8, &h, 8);

			(reinterpret_cast<atomic<uint64_t > *>(HT + slot + prob_index))->store(newBucket, std::memory_order_release);
			auto res_str = string_t(const_char_ptr_cast(slot_ptr + 1), UnsafeNumericCast<uint32_t>(*slot_ptr));
			return res_str;
		}
	}
//		nRejections_Probing++;
	return str;
}
// string_t UnifiedStringsDictionary::insertInternal(duckdb::string_t str, hash_t hash) {
//
//	if (nRejections_Probing.load() + nRejections_SizeFull.load() > ATTEMPT_THRESHOLD / 10) {
//		return str;
//	}
//
//	hash_t h = Hash(str);
//	//	if(!hash){
//	//		h = Hash(str);
//	//	} else{
//	//		h = hash;
//	//	}
//	uint32_t hashPrefix = Load<uint32_t>(const_data_ptr_cast(&h));
//
//	candidates++;
//
//	uint16_t slot;
//	memcpy(&slot, &hashPrefix, 2U);
//
//	uint16_t hashExtract = hashPrefix >> 16;
//
//	for (idx_t i = 0; i < PROBING_LIMIT; i++) {
//		// currently no looping around
//		if (slot + i > USSR_SIZE) {
//			nRejections_Probing++;
//			return str;
//		}
//
//		uint32_t bucket = reinterpret_cast<atomic<uint32_t> *>(HT + ((slot + i)))->load(std::memory_order_acquire);
//
//		uint16_t bucket_hashExtract = bucket >> 16;
//
//		if (bucket_hashExtract == hashExtract) {
//			// wrong already_in, do this after checking if equal
//			already_in++;
//			auto slot_ptr = DataRegion + (bucket & 0x0000FFFF);
//			// double checking that the string found is equal to the original string
//			auto len = strlen(const_char_ptr_cast(slot_ptr));
//			if (len != str.GetSize()) {
//				return str;
//			}
//			auto res_str = string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t>(str.GetSize()));
//			return (res_str == str) ? res_str : str;
//		}
//
//		if (bucket == 0) {
//			auto str_len = str.GetSize() + 1;
//			std::lock_guard<std::mutex> guard(insertLock);
//
//			// reject if not enough space left
//			auto remaining = (USSR_SIZE - currentEmptySlot) * 8;
//			if (str_len > remaining || currentEmptySlot > USSR_SIZE) {
//				nRejections_SizeFull++;
//				return str;
//			}
//
//			auto increasedSlot = (str_len % 8 == 0) ? 1 + (str_len / 8) : 2 + (str_len / 8);
//
//			uint32_t newBucket = UnsafeNumericCast<uint32_t>(hashExtract);
//			newBucket = newBucket << 16;
//			newBucket |= UnsafeNumericCast<uint32_t>(currentEmptySlot);
//			//	Printer::Print(to_string(currentEmptySlot));
//
//			//			if((newBucket & 0x0000FFFF) != currentEmptySlot){
//			//				Printer::Print(to_string(currentEmptySlot));
//			//				//		Printer::Print("fuck");
//			//			}
//			D_ASSERT((newBucket & 0x0000FFFF) == currentEmptySlot);
//
//			// another thread inserted
//			if (HT[slot + i] != 0) {
//				auto slot_hashExtract = HT[slot + i] >> 16;
//				if (slot_hashExtract == hashExtract) {
//					already_in++;
//					auto slot_ptr = DataRegion + (HT[slot + i] & 0x0000FFFF);
//					// double checking that the string found is equal to the original string
//					auto len = strlen(const_char_ptr_cast(slot_ptr));
//					if (len != str.GetSize()) {
//						return str;
//					}
//					auto res_str = string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t>(str.GetSize()));
//					if (res_str == str) {
//						return res_str;
//					} else {
//						continue;
//					}
//				} else {
//					continue;
//				}
//			}
//
//			accepted++;
//			auto ret = currentEmptySlot;
//			// 1 slot for the pre-computed hash,
//			currentEmptySlot += increasedSlot;
//			D_ASSERT(ret < currentEmptySlot);
//			auto slot_ptr = DataRegion + ret;
//
//			D_ASSERT(cast_pointer_to_uint64(slot_ptr) > cast_pointer_to_uint64(DataRegion));
//			D_ASSERT(cast_pointer_to_uint64(slot_ptr) <
//			         cast_pointer_to_uint64(DataRegion + USSR_SIZE * USSR_SLOT_SIZE));
//
//			memcpy(slot_ptr, str.GetData(), str.GetSize());
//			memcpy(slot_ptr - 1, &h, 8);
//			memset(slot_ptr + str.GetSize(), '\0', 1);
//			(reinterpret_cast<atomic<uint32_t> *>(HT + slot + i))->store(newBucket, std::memory_order_release);
//
//			return string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t>(str.GetSize()));
//		}
//	}
//	nRejections_Probing++;
//	return str;
//
//	//	if(HT[insertion_slot.GetIndex()] != 0){
//	//		candidates--;
//	//		Printer::Print("OH UH");
//	//		guard.unlock();
//	//		insertInternal(str, h);
//	//	}
//}

UnifiedStringsDictionary::~UnifiedStringsDictionary() {
	this->buffer.reset();
	//	this->LinearProbingHT.reset();
//		this->getStatistics();
}

void UnifiedStringsDictionary::getStatistics() {
	// A small helper to pad strings on the right
	Printer::Print("");
	auto padRight = [](const std::string &text, std::size_t width) {
		if (text.size() >= width) {
			return text; // If it already exceeds or matches the width, just return it
		}
		return text + std::string(width - text.size(), ' ');
	};

	// Specify column widths as needed
	const std::size_t w1 = 15;
	const std::size_t w2 = 15;
	const std::size_t w3 = 20;
	const std::size_t w4 = 25;
	const std::size_t w5 = 15;

	// Build header row
	std::string header;
	header += padRight("candidates", w1);
	header += padRight("accepted", w5);
	header += padRight("already in", w2);
	header += padRight("Rejected(full USSR)", w3);
	header += padRight("Rejected(failed probing)", w4);

	Printer::Print(header);

	// Build stats row
	std::string statsStr;
	statsStr += padRight(std::to_string(candidates), w1);
	statsStr += padRight(std::to_string(accepted), w2);
	statsStr += padRight(std::to_string(already_in), w5);
	statsStr += padRight(std::to_string(nRejections_SizeFull), w3);
	statsStr += padRight(std::to_string(nRejections_Probing), w4);
	//
	//
	Printer::Print(statsStr);

			    	Printer::PrintF("faster hash path triggered: %d, equal pointers for strings: %d",
				                string_t::StringComparisonOperators::faster_hash.load(),
				                string_t::StringComparisonOperators::faster_equality.load());
				string_t::StringComparisonOperators::faster_equality = 0;
				string_t::StringComparisonOperators::faster_hash = 0;
}

} // namespace duckdb
