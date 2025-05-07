#include "duckdb/execution/operator/helper/physical_ussr_insertion.h"
#include "duckdb/common/printer.hpp"

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
	explicit USSRInsertionGState(ClientContext &context){
		analyzing_budget = 100;
	}

	mutex budget_lock;
	idx_t analyzing_budget;
	vector<optional_ptr<DataChunk>> cached;
};


OperatorResultType PhysicalUnifiedString::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<USSRInsertionState>();
	auto &gstateussr = gstate.Cast<USSRInsertionGState>();

	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if(input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR || input.data[col_idx].GetType() != LogicalType::VARCHAR){
			continue;
		}
		if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
			auto &dict = DictionaryVector::Child(input.data[col_idx]);
			auto size = DictionaryVector::DictionarySize(input.data[col_idx]);
//			auto segment_str = DictionaryVector::DictionaryId(input.data[col_idx]);
//			auto segment_ptr_value = std::strtoull(segment_str.c_str(), nullptr, 10); // base 10
//			auto segment = reinterpret_cast<ColumnSegment *>(segment_ptr_value);
//			segment->block.get()->Load();
			if(!size.IsValid()) {
				continue;
			}
			Printer::Print(to_string(size.GetIndex()));

			state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);

			std::vector<idx_t> insertion_priority;

			{
				std::lock_guard<std::mutex> lock(gstateussr.budget_lock);
				if(gstateussr.analyzing_budget > 0){
					gstateussr.analyzing_budget--;
				}
			}

			if(gstateussr.analyzing_budget && size.GetIndex() > 1000){
				std::vector<uint32_t> count(size.GetIndex());

				for (idx_t i = 0; i < input.size(); ++i) {
					count[(DictionaryVector::SelVector(input.data[col_idx]).data()[i])]++;
				}
				for (idx_t i = 1; i < count.size(); ++i) {
					if(count[i] > (input.size() / count.size())){
						insertion_priority.push_back(i);
					}
				}
			}
			USSR_insertion_loop(dict.GetData(), size.GetIndex(), context.client, std::move(insertion_priority));
//			auto start = reinterpret_cast<string_t *>(dict.GetData());
//
//			for (idx_t i = 1; i < size.GetIndex() - 1; ++i) {
//
//				start[i] = context.client.GetCurrentQueryUssr().insert(start[i]);
//			}
		}
	}

	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
	//	context.client.GetCurrentQueryUssr().insert();
}

unique_ptr<OperatorState> PhysicalUnifiedString::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<USSRInsertionState>(context, insert_to_ussr.size());
}

unique_ptr<GlobalOperatorState> PhysicalUnifiedString::GetGlobalOperatorState(ClientContext &context) const{
	return make_uniq<USSRInsertionGState>(context);
}

void PhysicalUnifiedString::USSR_insertion_loop(data_ptr_t dict_strings, idx_t count, ClientContext &context, const vector<idx_t > &priority_insertion) const{
	auto start = reinterpret_cast<string_t *>(dict_strings);

	if(priority_insertion.empty()){
		for (idx_t i = 0; i < count; ++i) {
			start[i] = context.GetCurrentQueryUssr().insert(start[i]);
		}
	} else{
		for (auto string_idx : priority_insertion) {
			if(string_idx != 0){
				start[string_idx] = context.GetCurrentQueryUssr().insert(start[string_idx]);
			}
		}
	}
}


} // namespace duckdb
