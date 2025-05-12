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
	unordered_set<string> strs;
	vector<bool> inserted;
	vector<idx_t > count;
	vector<string> current_dict_ids;
	idx_t analysis_budget;
	idx_t current_analysis_count;
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
		// no selection vector analysis
		auto &dict = DictionaryVector::Child(input.data[col_idx]);
		auto size = DictionaryVector::DictionarySize(input.data[col_idx]);
		if(!size.IsValid()) {
			continue;
		}

		if(size.GetIndex() <= 1000){
			if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
				state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);
				USSR_insertion_loop(dict.GetData(), size.GetIndex(), context.client, {});
			}
		}else {
			if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
				// initialize the local state
				state.inserted.clear();
				state.count.clear();
				state.inserted.reserve(size.GetIndex());
				state.count.reserve(size.GetIndex());
				for (idx_t i = 0; i < size.GetIndex(); i++) {
					state.inserted.push_back(false);
					state.count.push_back(0);
				}
				state.analysis_budget = 0;
				state.current_analysis_count = 1;
				auto &sel = DictionaryVector::SelVector(input.data[col_idx]);
				for (idx_t i = 0; i < input.size(); i++) {
					state.count[sel.data()[i]]++;
				}

				vector<idx_t > priority_selection;
				for (idx_t i = 1; i < state.count.size(); i++) {
					if(state.count[i] > (50 * state.current_analysis_count)){
						priority_selection.push_back(i);
						state.inserted[i] = true;
					}
				}

				state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);
				USSR_insertion_loop(dict.GetData(), size.GetIndex(), context.client, priority_selection, true);
			}else if (insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) == state.current_dict_ids[col_idx] && state.current_analysis_count <= state.analysis_budget) {
				state.current_analysis_count++;
//				Printer::Print(to_string(++vec_counter));

				auto &sel = DictionaryVector::SelVector(input.data[col_idx]);
				for (idx_t i = 0; i < input.size(); i++) {
					state.count[sel.data()[i]]++;
				}

				vector<idx_t > priority_selection;
				for (idx_t i = 1; i < state.count.size(); ++i) {
					if(state.count[i] > (10 * state.current_analysis_count) && !state.inserted[i]){
						priority_selection.push_back(i);
						state.inserted[i] = true;
					}
				}
				USSR_insertion_loop(dict.GetData(), size.GetIndex(), context.client, priority_selection, true);
			}

		}


		chunk.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
		//	context.client.GetCurrentQueryUssr().insert();
	}

//
//		if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
//
//
//
//
//
//			Printer::Print(to_string(size.GetIndex()));
//
//			// initialize the local state
//			state.inserted.reserve(size.GetIndex());
//			for (idx_t i = 0; i < size.GetIndex(); ++i) {
//				state.inserted.push_back(false);
//			}
//
//			state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);
////
////			std::vector<idx_t> insertion_priority;
////
////			{
////				std::lock_guard<std::mutex> lock(gstateussr.budget_lock);
////				if(gstateussr.analyzing_budget > 0){
////					gstateussr.analyzing_budget--;
////				}
////			}
////
////			if(gstateussr.analyzing_budget && size.GetIndex() > 1000){
////				std::vector<uint32_t> count(size.GetIndex());
////
////				for (idx_t i = 0; i < input.size(); ++i) {
////					count[(DictionaryVector::SelVector(input.data[col_idx]).data()[i])]++;
////				}
////				for (idx_t i = 1; i < count.size(); ++i) {
////					if(count[i] > (input.size() / count.size())){
////						insertion_priority.push_back(i);
////					}
////				}
////			}
//			USSR_insertion_loop(dict.GetData(), size.GetIndex(), context.client, {});
////			auto start = reinterpret_cast<string_t *>(dict.GetData());
////
////			for (idx_t i = 1; i < size.GetIndex() - 1; ++i) {
////
//				start[i] = context.client.GetCurrentQueryUssr().insert(start[i]);
//			}
//		}
//	}

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

void PhysicalUnifiedString::USSR_insertion_loop(data_ptr_t dict_strings, idx_t count, ClientContext &context, const vector<idx_t > &priority_insertion, bool exists_prio) const{
	auto start = reinterpret_cast<string_t *>(dict_strings);

	if(priority_insertion.empty() && !exists_prio){
		for (idx_t i = 1; i < count; ++i) {
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
