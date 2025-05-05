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
	explicit USSRInsertionGState(ClientContext &context){
//		analyze_row_group.emplace_back(false);
	    done_caching.store(false);
	}

	std::mutex cached_lock;
	vector<atomic<bool>> analyze_row_group;
	deque<unique_ptr<DataChunk>> cached;
	atomic<bool> done_caching;
};

OperatorResultType PhysicalUnifiedString::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {

	auto &state = state_p.Cast<USSRInsertionState>();
	auto &gstateussr = gstate.Cast<USSRInsertionGState>();

	std::lock_guard<std::mutex> lock(gstateussr.cached_lock);
	// caching
	if(gstateussr.cached.size() < 50 && gstateussr.done_caching.load() == false){
                // create an empty chunk with the right layout
                auto datachunk_ptr = make_uniq<DataChunk>();
		        datachunk_ptr->Initialize(Allocator::Get(context.client), input.GetTypes());
		        input.Flatten();
//		        datachunk_ptr->SetCardinality(input);
		        datachunk_ptr->SetCapacity(input);
		        input.Copy(*datachunk_ptr);
                gstateussr.cached.push_back(std::move(datachunk_ptr));
                return OperatorResultType::NEED_MORE_INPUT;
	}
	bool expected = false;
	bool desired = true;

//	vector<idx_t> priority_insertion
	if (gstateussr.done_caching.compare_exchange_strong(expected, desired)){
		// start analyzing
	}

	// done caching, analyze and insert
	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if(input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR){
			continue;
		}
		if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
			// if set, evaluate strings
			// evaluate_strings_colseg();
			// evaluate_strings_datachunk_delay();
			// evaluate_strings_incoming_vector();

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


//	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
	//	context.client.GetCurrentQueryUssr().insert();
}

unique_ptr<OperatorState> PhysicalUnifiedString::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<USSRInsertionState>(context, insert_to_ussr.size());
}
OperatorFinalizeResultType PhysicalUnifiedString::FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                        GlobalOperatorState &gstate, OperatorState &state) const {
		auto &gstateussr = gstate.Cast<USSRInsertionGState>();
		if(gstateussr.cached.empty()){
			return OperatorFinalizeResultType::FINISHED;
		} else{
		    		std::unique_ptr<DataChunk> cached_chunk;
		    		{
		    			std::lock_guard<std::mutex> lock(gstateussr.cached_lock);
		    			if (gstateussr.cached.empty()) {
		    				return OperatorFinalizeResultType::FINISHED;
		    			}
			            cached_chunk = std::move(gstateussr.cached.front());
			            gstateussr.cached.pop_front();
		    		}
		            chunk.Move(*cached_chunk);
		    		return OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
	    }
	return OperatorFinalizeResultType::FINISHED;
//	auto &gstateussr = gstate.Cast<USSRInsertionGState>();
//	if(gstateussr.cached.empty()){
//		return OperatorFinalizeResultType::FINISHED;
//	}
//	else{
//		optional_ptr<DataChunk> cached_chunk;
//		{
//			std::lock_guard<std::mutex> lock(gstateussr.cached_lock);
//			if (gstateussr.cached.empty()) {
//				return OperatorFinalizeResultType::FINISHED;
//			}
//			cached_chunk = gstateussr.cached.front();
//			gstateussr.cached.pop_front();
//		}
//		chunk.Reference(*cached_chunk);
//		return OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
//	}

}

void PhysicalUnifiedString::USSR_insertion_loop(data_ptr_t dict_strings, idx_t count, ClientContext &context, const vector<idx_t > &priority_insertion) const{
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
	return make_uniq<USSRInsertionGState>(context);
}


} // namespace duckdb
