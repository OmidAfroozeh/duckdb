#include "duckdb/execution/operator/helper/physical_ussr_insertion.h"
#include "duckdb/common/printer.hpp"
#include "duckdb/storage/compression/dictionary/common.hpp"

namespace duckdb {

class USSRInsertionState : public OperatorState {
public:
	explicit USSRInsertionState(ExecutionContext &context, idx_t cols){
		for (idx_t i = 0; i < cols; ++i) {
			current_dict_ids.push_back("");
		}
	}

	vector<string> current_dict_ids;
};

class USSRInsertionGState : public GlobalOperatorState {
public:
	explicit USSRInsertionGState(ClientContext &context, idx_t cols){
//		analyze_row_group.emplace_back(false);

	}

	std::mutex cached_lock;
	vector<atomic<bool>> analyze_row_group;
	deque<unique_ptr<DataChunk>> cached;
	atomic<bool> done_caching;
	std::mutex evaluation_lock;
	vector<atomic<bool>> evaluate_col;
};

void PhysicalUnifiedString::evaluate_strings_colseg(string& segment_str, vector<bool>& result) const{
	// returns a vector of idx of which string to insert
	auto segment_ptr_value = std::strtoull(segment_str.c_str(), nullptr, 10);
	if(!segment_ptr_value){
		return;
	}
	auto segment = reinterpret_cast<ColumnSegment *>(segment_ptr_value);
	data_ptr_t block_ptr;
	// FIXME: with the columnsegment, we have access to block_id, we just need to get the block directly
	if(!segment->block->IsUnloaded()){
		block_ptr = segment->block.get()->Load().Ptr();
	} else{
		return;
	}

	auto baseptr = block_ptr + segment->GetBlockOffset();
	auto dict_header = reinterpret_cast<dictionary_compression_header_t *>(baseptr);
	data_ptr_t base_data = data_ptr_cast(baseptr + DictionaryCompression::DICTIONARY_HEADER_SIZE);

	auto sel_vec = make_buffer<SelectionVector>(BitpackingPrimitives::RoundUpToAlgorithmGroupSize(100000ull));

	BitpackingPrimitives::UnPackBuffer<sel_t>(data_ptr_cast(sel_vec->data()), base_data, BitpackingPrimitives::RoundUpToAlgorithmGroupSize(100000ull), dict_header->bitpacking_width);
	std::vector<uint32_t> count(dict_header->dict_size);
	count.reserve(dict_header->dict_size);
	for (idx_t i = 0; i < 100000; ++i) {
		count[sel_vec->data()[i]]++;
	}
	std::vector<bool> insertion_priority(dict_header->dict_size);
	for (idx_t i = 0; i < 100000; ++i) {
		insertion_priority[i] = (count[i] > (100000 / dict_header->dict_size)) ? true: false;
	}
	result = std::move(insertion_priority);
	    //	for (auto x : count) {
//		Printer::Print(to_string(x));
//	}
}

OperatorResultType PhysicalUnifiedString::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {

	auto &state = state_p.Cast<USSRInsertionState>();
	auto &gstateussr = gstate.Cast<USSRInsertionGState>();

	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if(input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR){
			continue;
		}
		if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
			gstateussr.evaluate_col[col_idx].compare_exchange_strong()
			gstateussr.evaluate_col[col_idx]
			 evaluate_strings_colseg();




			auto &dict = DictionaryVector::Child(input.data[col_idx]);
			auto size = DictionaryVector::DictionarySize(input.data[col_idx]);
			if(!size.IsValid()) {
				continue;
			}

			state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);

			auto start = reinterpret_cast<string_t *>(dict.GetData());

			for (idx_t i = 1; i < size.GetIndex() - 1; ++i) {

				start[i] = context.client.GetCurrentQueryUssr().insert(start[i]);
			}
		}
	}

	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
}

unique_ptr<OperatorState> PhysicalUnifiedString::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<USSRInsertionState>(context, insert_to_ussr.size());
}

void PhysicalUnifiedString::USSR_insertion_loop(data_ptr_t dict_strings, idx_t count, ClientContext &context, const vector<idx_t > &priority_insertion){
	auto start = reinterpret_cast<string_t *>(dict_strings);

	if(priority_insertion.empty()){
		for (idx_t i = 1; i < count - 1; ++i) {
			start[i] = context.GetCurrentQueryUssr().insert(start[i]);
		}
	} else{
		for (auto string_idx : priority_insertion) {
			start[string_idx] = context.GetCurrentQueryUssr().insert(start[string_idx]);
		}
	}
}


unique_ptr<GlobalOperatorState> PhysicalUnifiedString::GetGlobalOperatorState(ClientContext &context) const{
	return make_uniq<USSRInsertionGState>(context, insert_to_ussr.size());
}


} // namespace duckdb
