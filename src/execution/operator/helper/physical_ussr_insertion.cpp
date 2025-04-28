#include "duckdb/execution/operator/helper/physical_ussr_insertion.h"

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


OperatorResultType PhysicalUnifiedString::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<USSRInsertionState>();
	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if(input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR){
			continue;
		}
		if(insert_to_ussr[col_idx] && DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]){
			auto &dict = DictionaryVector::Child(input.data[col_idx]);
			auto size = DictionaryVector::DictionarySize(input.data[col_idx]);

			state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);

			auto start = reinterpret_cast<string_t *>(dict.GetData());

			for (idx_t i = 1; i < size.GetIndex(); ++i) {
				start[i] = context.client.GetCurrentQueryUssr().insert(start[i]);
			}
		}
	}

	chunk.Reference(input);

	return OperatorResultType::NEED_MORE_INPUT;
	//	context.client.GetCurrentQueryUssr().insert();
}

unique_ptr<OperatorState> PhysicalUnifiedString::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<USSRInsertionState>(context, insert_to_ussr.size());
}


} // namespace duckdb
