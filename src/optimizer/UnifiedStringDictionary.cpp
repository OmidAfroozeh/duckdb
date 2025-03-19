#include "duckdb/optimizer/UnifiedStringDictionary.h"

#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
namespace duckdb {

uint64_t UnifiedStringsDictionary::USSR_prefix = 0;
UnifiedStringsDictionary *UnifiedStringsDictionary::ussr_instance {nullptr};

void UnifiedStringsDictionary::destroy_UnifiedStrings() {
	// error prone, don't know how to fix
	// for now only used for getting statistics, singleton causes memory leak!!!
	if (ussr_instance) {
		ussr_instance->buffer.reset();
#ifdef DEBUG
		ussr_instance->getStatistics();
#endif
		ussr_instance = nullptr;
	}
}

UnifiedStringsDictionary::UnifiedStringsDictionary() {

	buffer = make_unsafe_uniq_array_uninitialized<data_t>(BUFFER_SIZE);

	USSR_prefix = cast_pointer_to_uint64(buffer.get() + USSR_SIZE * USSR_SLOT_SIZE) & USSR_MASK;
	DataRegion = reinterpret_cast<uint64_t *>(USSR_prefix);

	// Double check that the DataRegion is contained within the buffer
	D_ASSERT(cast_pointer_to_uint64(buffer.get()) <= cast_pointer_to_uint64(DataRegion));
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

#ifdef DEBUG
	candidates = 0;
	accepted = 0;
	nRejections_Probing = 0;
	nRejections_SizeFull = 0;
#endif
}

UnifiedStringsDictionary *UnifiedStringsDictionary::getInstance() {
	// memory leak, to suppress compiler warning
	static std::mutex &singletonLock = *new std::mutex();
	lock_guard<std::mutex> guard(singletonLock);
	if (!ussr_instance) {
		ussr_instance = new UnifiedStringsDictionary();
	}
	return ussr_instance;
}

string_t UnifiedStringsDictionary::insertInternal(string_t str) {
#ifdef DEBUG
	candidates++;
#endif

	hash_t h = Hash(str);
	uint64_t hash_prefix = h & 0x00000000FFFFFFFF;

	uint16_t slot;
	memcpy(&slot, &hash_prefix, 2U);

	uint16_t hashExtract = UnsafeNumericCast<uint16_t>(hash_prefix >> 16);

	for (idx_t i = 0; i < PROBING_LIMIT; i++) {
		// currently no looping around
		if (slot + i > USSR_SIZE) {
#ifdef DEBUG
			nRejections_Probing++;
#endif
			return str;
		}

		uint64_t bucket = Load<uint32_t>(const_data_ptr_cast(HT + ((slot + i))));
		uint64_t bucket_hashExtract = bucket >> 16;

		if (bucket_hashExtract == hashExtract) {
			auto data_region_slot = bucket & 0x000000000000FFFF;
			// the string_hash_extract could be zero,
			// we also need to check that the slot is also zero to 100% be sure that this is not filled
			if (data_region_slot != 0) {
#ifdef DEBUG
				accepted++;
#endif
				auto len = strlen(const_char_ptr_cast(DataRegion + data_region_slot));
				if (len != str.GetSize()) {
					return str;
				}
				auto res_str = string_t(const_char_ptr_cast(DataRegion + data_region_slot),
				                        UnsafeNumericCast<uint32_t>(str.GetSize()));
				return (res_str == str) ? res_str : str;
			}
		}

		if (bucket == 0) {
			// reject if not enough space left
			auto remaining = (USSR_SIZE - currentEmptySlot) * 8;
			if (str.GetSize() + 1 > remaining) {
#ifdef DEBUG
				nRejections_SizeFull++;
#endif
				return str;
			}
			auto increasedSlot = (str.GetSize() + 1 % 8 == 0) ? 1 + (str.GetSize() / 8) : 2 + (str.GetSize() / 8);

			uint32_t new_bucket = UnsafeNumericCast<uint32_t>(hashExtract);
			new_bucket = new_bucket << 16;
			new_bucket |= UnsafeNumericCast<uint32_t>(currentEmptySlot);
			//			Printer::PrintF("currentEmptySlot = %d | stored = %d", currentEmptySlot, new_bucket &
			//0x0000FFFF);
			D_ASSERT((new_bucket & 0x0000FFFF) == currentEmptySlot);
#ifdef DEBUG
			accepted++;
#endif
			HT[slot + i] = new_bucket;
			// 1 slot for the pre-computed hash,
			//			D_ASSERT(ret < currentEmptySlot);
			memcpy(DataRegion + currentEmptySlot, str.GetData(), str.GetSize());
			memcpy((DataRegion + currentEmptySlot) - 1, &h, 8);
			memset(DataRegion + currentEmptySlot + str.GetSize(), '\0', 1);
			auto result_string = string_t(const_char_ptr_cast(DataRegion + currentEmptySlot),
			                              UnsafeNumericCast<uint32_t>(str.GetSize()));
			currentEmptySlot += increasedSlot;

			return result_string;
		}
	}
#ifdef DEBUG
	nRejections_Probing++;
#endif
	return str;
}

void UnifiedStringsDictionary::getStatistics() {
#ifdef DEBUG
	// A small helper to pad strings on the right
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

	// Build header row
	std::string header;
	header += padRight("candidates", w1);
	header += padRight("accepted", w2);
	header += padRight("Rejected(full USSR)", w3);
	header += padRight("Rejected(failed probing)", w4);

	Printer::Print(header);

	// Build stats row
	std::string statsStr;
	statsStr += padRight(std::to_string(candidates), w1);
	statsStr += padRight(std::to_string(accepted), w2);
	statsStr += padRight(std::to_string(nRejections_SizeFull), w3);
	statsStr += padRight(std::to_string(nRejections_Probing), w4);

	Printer::Print(statsStr);
#endif
}
string_t UnifiedStringsDictionary::insert(string_t str) {
	if (str.IsInlined()) {
		return str;
	}
	// grab the lock
	lock_guard<std::mutex> guard(insertLock);

	return insertInternal(str);
}

} // namespace duckdb
