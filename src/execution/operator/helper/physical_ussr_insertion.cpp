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
		done_caching.store(false);

		for (idx_t i = 0; i < cols; ++i) {
			priority_insertion.push_back({});
			evaluate_col.push_back(false);
			sel_vec_buffers.push_back(nullptr);
			unique_value_sizes.push_back(0);
		}
	}
	vector<vector<idx_t > > priority_insertion;
	vector<idx_t > unique_value_sizes;
	vector<std::unique_ptr<sel_t[]>> sel_vec_buffers;
	std::mutex cached_lock;
	vector<atomic<bool>> analyze_row_group;
	deque<unique_ptr<DataChunk>> cached;
	atomic<bool> done_caching;
	std::mutex evaluation_lock;
	vector<bool> evaluate_col;
};

vector<idx_t> PhysicalUnifiedString::evaluate_strings_cached_flat(optional_ptr<uint32_t> cached_sel, idx_t size) const{

	std::vector<uint32_t> count(size);

	for (idx_t i = 0; i < 50 * STANDARD_VECTOR_SIZE; ++i) {
		 count[*(cached_sel.get() + i)]++;
	}

	std::vector<idx_t> insertion_priority;
	for (idx_t i = 0; i < count.size(); ++i) {
		if(count[i] > (50 * STANDARD_VECTOR_SIZE / count.size())){
			insertion_priority.push_back(i);
		}
	}
	return insertion_priority;
}


OperatorResultType PhysicalUnifiedString::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {

	auto &state = state_p.Cast<USSRInsertionState>();
	auto &gstateussr = gstate.Cast<USSRInsertionGState>();
	bool fl = false;
	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if(input.data[col_idx].GetVectorType() == VectorType::DICTIONARY_VECTOR){
			fl = true;
			break;
		}

	}
	if(!fl) {
		chunk.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
	}


	{
	std::lock_guard<std::mutex> lock(gstateussr.cached_lock);
	// caching
	if(gstateussr.cached.size() < 50 && gstateussr.done_caching.load() == false){
                // create an empty chunk with the right layout
                auto datachunk_ptr = make_uniq<DataChunk>();
			    datachunk_ptr->Initialize(Allocator::Get(context.client), input.GetTypes());
			    // add the dict_vectors selection vector to the buffer
			    for (idx_t i = 0; i < input.ColumnCount(); ++i) {
					if(input.data[i].GetVectorType() == VectorType::DICTIONARY_VECTOR){
					    if(!DictionaryVector::DictionarySize(input.data[i]).IsValid()) {
						    continue;
					    }
					    gstateussr.unique_value_sizes[i] = DictionaryVector::DictionarySize(input.data[i]).GetIndex();
					    if(gstateussr.sel_vec_buffers[i] == nullptr){
						    gstateussr.sel_vec_buffers[i] =  std::move(make_unsafe_uniq_array_uninitialized<sel_t >(static_cast<size_t > (50) * STANDARD_VECTOR_SIZE));
					    }
					    memcpy(reinterpret_cast<void *>(gstateussr.sel_vec_buffers[i].get() + gstateussr.cached.size() * STANDARD_VECTOR_SIZE), reinterpret_cast<void *>(DictionaryVector::SelVector(input.data[i]).data()), STANDARD_VECTOR_SIZE * sizeof(sel_t));
				    }
			    }
			    input.Flatten();
//		        datachunk_ptr->SetCardinality(input);
		        datachunk_ptr->SetCapacity(input);
		        input.Copy(*datachunk_ptr);
                gstateussr.cached.push_back(std::move(datachunk_ptr));
                return OperatorResultType::NEED_MORE_INPUT;
	}
	{
		if (gstateussr.cached.size() == 50) {
			gstateussr.done_caching.store(true);

			for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
				if (input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR) {
					continue;
				}
				bool eval_flag = false;
				{
					std::lock_guard<std::mutex> lock2(gstateussr.evaluation_lock);
					if (!gstateussr.evaluate_col[col_idx]) {
						gstateussr.evaluate_col[col_idx] = true;
						eval_flag = true;
					}
				}
				if (eval_flag) {
					gstateussr.priority_insertion[col_idx] = evaluate_strings_cached_flat(
						gstateussr.sel_vec_buffers[col_idx].get(), gstateussr.unique_value_sizes[col_idx]);
				}

			}
		}
	}


	}
	// done caching, analyze and insert
	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if(input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR){
			continue;
		}
		if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){

			auto &dict = DictionaryVector::Child(input.data[col_idx]);
			auto size = DictionaryVector::DictionarySize(input.data[col_idx]);
			if(!size.IsValid()) {
				continue;
			}

			state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);

			USSR_insertion_loop(dict.GetData(), size.GetIndex(), context.client, gstateussr.priority_insertion[col_idx]);
		}
	}
	std::unique_ptr<DataChunk> cached_chunk;
	{
		std::lock_guard<std::mutex> lock(gstateussr.cached_lock);
		if (gstateussr.cached.empty()) {
			chunk.Reference(input);

			return OperatorResultType::NEED_MORE_INPUT;
		}
		cached_chunk = std::move(gstateussr.cached.front());
		gstateussr.cached.pop_front();
	}
	chunk.Move(*cached_chunk);
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
	return make_uniq<USSRInsertionGState>(context, insert_to_ussr.size());
}


} // namespace duckdb
