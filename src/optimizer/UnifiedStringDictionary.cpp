#include "duckdb/optimizer/UnifiedStringDictionary.h"
#include "duckdb/common/types/hash.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>

namespace duckdb {

UnifiedStringsDictionary::UnifiedStringsDictionary() {

	buffer = make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE);
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

	HT = reinterpret_cast<uint32_t *>(HT_address);

	currentEmptySlot = 1;

	candidates = 0;
	accepted = 0;
	nRejections_Probing = 0;
	nRejections_SizeFull = 0;
	already_in = 0;
}

string_t UnifiedStringsDictionary::insert(string_t str) {
	// no support for short strings now
	if (str.IsInlined()) {
		return str;
	}

	return insertInternal(str);
}

string_t UnifiedStringsDictionary::insertInternal(duckdb::string_t str) {

	hash_t h = Hash(str);
	uint32_t hashPrefix = Load<uint32_t>(const_data_ptr_cast(&h));

	candidates++;

	uint16_t slot;
	memcpy(&slot, &hashPrefix, 2U);

	uint16_t hashExtract = hashPrefix >> 16;

	optional_idx insertion_slot;

	for (idx_t i = 0; i < PROBING_LIMIT; i++) {
		// currently no looping around
		if (slot + i > USSR_SIZE) {
			nRejections_Probing++;
			return str;
		}

		uint32_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + ((slot + i))));

		uint16_t bucket_hashExtract = bucket >> 16;

		if (bucket == 0) {
			insertion_slot = slot + i;
			break;
		}

		if (bucket_hashExtract == hashExtract) {
			already_in++;
			auto slot_ptr = DataRegion + (bucket & 0x0000FFFF);
			// double checking that the string found is equal to the original string
			auto len = strlen(const_char_ptr_cast(slot_ptr));
			if (len != str.GetSize()) {
				return str;
			}
			auto res_str = string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t>(str.GetSize()));
			return (res_str == str) ? res_str : str;
		}


	}
	if (!insertion_slot.IsValid()) {
		nRejections_Probing++;
		return str;
	}

	auto str_len = str.GetSize() + 1;
	// reject if not enough space left
	auto remaining = (USSR_SIZE - currentEmptySlot) * 8;
	if (str_len > remaining || currentEmptySlot > USSR_SIZE) {
		nRejections_SizeFull++;
		return str;
	}



	auto increasedSlot = (str_len % 8 == 0) ? 1 + (str_len / 8) : 2 + (str_len / 8);

	uint32_t newBucket = UnsafeNumericCast<uint32_t>(hashExtract);
	newBucket = newBucket << 16;
	newBucket |= UnsafeNumericCast<uint32_t>(currentEmptySlot);
	D_ASSERT((newBucket & 0x0000FFFF) == currentEmptySlot);


	std::unique_lock<std::mutex> lock(insertLock);

	if(HT[insertion_slot.GetIndex()] != 0){
		candidates--;
		insertInternal(str);
	}


	HT[insertion_slot.GetIndex()] = newBucket;


	accepted++;
	auto ret = currentEmptySlot;
	// 1 slot for the pre-computed hash,
	currentEmptySlot += increasedSlot;
	D_ASSERT(ret < currentEmptySlot);
	auto slot_ptr = DataRegion + ret;
	lock.unlock();


	D_ASSERT(cast_pointer_to_uint64(slot_ptr) > cast_pointer_to_uint64(DataRegion));
	D_ASSERT(cast_pointer_to_uint64(slot_ptr) < cast_pointer_to_uint64(DataRegion + USSR_SIZE * USSR_SLOT_SIZE));

	memcpy(slot_ptr, str.GetData(), str.GetSize());
	memcpy(slot_ptr - 1, &h, 8);
	memset(slot_ptr + str.GetSize(), '\0', 1);
	return string_t(const_char_ptr_cast(slot_ptr), UnsafeNumericCast<uint32_t>(str.GetSize()));
}

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
//	std::string statsStr;
//	statsStr += padRight(std::to_string(candidates), w1);
//	statsStr += padRight(std::to_string(accepted), w2);
//	statsStr += padRight(std::to_string(already_in), w5);
//	statsStr += padRight(std::to_string(nRejections_SizeFull), w3);
//	statsStr += padRight(std::to_string(nRejections_Probing), w4);
//
//	Printer::Print(statsStr);

//	Printer::PrintF("faster hash path triggered: %d, equal pointers for strings: %d",
//	                string_t::StringComparisonOperators::fast_hash_counter,
//	                string_t::StringComparisonOperators::eq_check_counter);
//	string_t::StringComparisonOperators::eq_check_counter = 0;
//	string_t::StringComparisonOperators::fast_hash_counter = 0;
}

} // namespace duckdb
