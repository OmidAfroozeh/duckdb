#include "duckdb/execution/operator/helper/physical_ussr_insertion.h"
#include "duckdb/common/printer.hpp"

namespace duckdb {

class USSRInsertionState : public OperatorState {
public:
	explicit USSRInsertionState(ExecutionContext &context, idx_t cols) {
		for (idx_t i = 0; i < cols; ++i) {
			current_dict_ids.push_back("");
			inserted.push_back({});
			count.push_back({});
			analysis_budget.push_back(0);
			inserted_so_far.push_back(0);
			rows_seen_total.push_back(0);

		}
	}
	vector<idx_t> rows_seen_total;
	vector<idx_t > inserted_so_far;
	unordered_set<string> strs;
	vector<vector<uint8_t>> inserted;
	vector<vector<uint32_t>> count;
	vector<string> current_dict_ids;
	vector<idx_t> analysis_budget;
};

class USSRInsertionGState : public GlobalOperatorState {
public:
	explicit USSRInsertionGState(ClientContext &context) {
		analyzing_budget = 100;
	}
	mutex budget_lock;
	idx_t analyzing_budget;
	vector<optional_ptr<DataChunk>> cached;
};

OperatorResultType PhysicalUnifiedString::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<USSRInsertionState>();
	//	auto &gstateussr = gstate.Cast<USSRInsertionGState>();
	for (idx_t col_idx = 0; col_idx < input.data.size(); ++col_idx) {
		if (input.data[col_idx].GetVectorType() != VectorType::DICTIONARY_VECTOR ||
		    input.data[col_idx].GetType() != LogicalType::VARCHAR) {
			continue;
		}
		// no selection vector analysis
		auto &dict = DictionaryVector::Child(input.data[col_idx]);
		auto dict_encoded_val_size = DictionaryVector::GetDictionaryEncodedValuesSize(input.data[col_idx]);
		auto dict_size = DictionaryVector::DictionarySize(input.data[col_idx]);
		auto &dict_validity = FlatVector::Validity(dict);
		if (!dict_size.IsValid() || !dict_encoded_val_size.IsValid()) {
			continue;
		}
		if(DictionaryVector::DictionaryId(input.data[col_idx])[0] == 'x'){
			continue;
		}

		auto ratio = dict_encoded_val_size.GetIndex() / dict_size.GetIndex();

		// insert low-cardinality columns without sampling
		if (dict_size.GetIndex() <= 1000 && ratio > 10 && dict_encoded_val_size.GetIndex() > 20000) {
			if (insert_to_ussr[col_idx] &&
			    DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]) {
//				Printer::Print(to_string(dict_size.GetIndex()) + " | " + to_string(ratio) + " | " + to_string(dict_encoded_val_size.GetIndex()));
				state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);
				USSR_insertion_loop(dict.GetData(), dict_size.GetIndex(), context.client, {}, dict_validity);
			}
		} else {
			if (insert_to_ussr[col_idx] &&
			    DictionaryVector::DictionaryId(input.data[col_idx]) != state.current_dict_ids[col_idx]) {
				state.inserted_so_far[col_idx]    = 0;          // reset skew statistics
				state.rows_seen_total[col_idx]    = 0;

				if (dict_size.GetIndex() > state.inserted[col_idx].size()) {
					state.inserted[col_idx].resize(dict_size.GetIndex(), false);   // one memset
					state.count[col_idx].resize(dict_size.GetIndex(), 0);          // one memset
				} else {
					std::fill_n(state.inserted[col_idx].begin(), dict_size.GetIndex(), false);
					std::fill_n(state.count[col_idx].begin(), dict_size.GetIndex(), 0);
				}

				idx_t sampling_rate = 25;
				idx_t sampling_count = (dict_encoded_val_size.GetIndex()  * sampling_rate) / 100;
				state.analysis_budget[col_idx] = sampling_count;


				idx_t current_analysis_count;
				if(state.analysis_budget[col_idx] > input.size()){
					current_analysis_count = input.size();
					state.analysis_budget[col_idx] = state.analysis_budget[col_idx] - current_analysis_count;
				} else{
					current_analysis_count = state.analysis_budget[col_idx];
					state.analysis_budget[col_idx] = state.analysis_budget[col_idx] - current_analysis_count;
					D_ASSERT(state.analysis_budget[col_idx] == 0);
				}

				idx_t &rows_seen   = state.rows_seen_total[col_idx];
				rows_seen         += current_analysis_count;                   // add current batch size

				idx_t dict_k = dict_size.GetIndex()/4; // around 25% of the data will contain the majority of the values

				idx_t threshold = std::max<idx_t >((rows_seen + dict_k) / (dict_k + 1), 1);


				auto &sel = DictionaryVector::SelVector(input.data[col_idx]);
				for (idx_t i = 0; i < current_analysis_count; i++) {
					state.count[col_idx][sel.data()[i]]++;
				}

				vector<idx_t> priority_selection;
				for (idx_t i = 1; i < dict_size.GetIndex(); i++) {
					if (state.count[col_idx][i] >= (threshold)) {
						priority_selection.push_back(i);
						state.inserted[col_idx][i] = true;
					}
				}
				state.inserted_so_far[col_idx] = state.inserted_so_far[col_idx] + priority_selection.size();
				state.current_dict_ids[col_idx] = DictionaryVector::DictionaryId(input.data[col_idx]);
				USSR_insertion_loop(dict.GetData(), dict_size.GetIndex(), context.client, priority_selection, dict_validity, true);
			} else if (insert_to_ussr[col_idx] &&
			           DictionaryVector::DictionaryId(input.data[col_idx]) == state.current_dict_ids[col_idx] && state.analysis_budget[col_idx] > 0) {


				idx_t current_analysis_count;
				if(state.analysis_budget[col_idx] > input.size()){
					current_analysis_count = input.size();
					state.analysis_budget[col_idx] = state.analysis_budget[col_idx] - current_analysis_count;
				} else{
					current_analysis_count = state.analysis_budget[col_idx];
					state.analysis_budget[col_idx] = state.analysis_budget[col_idx] - current_analysis_count;
				}

				idx_t &rows_seen   = state.rows_seen_total[col_idx];
				rows_seen         += current_analysis_count;                   // add current batch size

				idx_t dict_k =
				    dict_size.GetIndex()/4; // around 25% of the data will contain the majority of the values

				idx_t threshold = std::max<idx_t >((rows_seen + dict_k) / (dict_k + 1), 1);

				auto &sel = DictionaryVector::SelVector(input.data[col_idx]);
				for (idx_t i = 0; i < current_analysis_count; i++) {
					state.count[col_idx][sel.data()[i]]++;
				}

				vector<idx_t> priority_selection;
				for (idx_t i = 1; i < dict_size.GetIndex(); ++i) {
					if (state.count[col_idx][i] >= (threshold) &&
					    !state.inserted[col_idx][i]) {
						priority_selection.push_back(i);
						state.inserted[col_idx][i] = true;
					}
				}
				state.inserted_so_far[col_idx] = state.inserted_so_far[col_idx] + priority_selection.size();

				USSR_insertion_loop(dict.GetData(), dict_size.GetIndex(), context.client, priority_selection, dict_validity, true);
			}
		}
	}
	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
}

unique_ptr<OperatorState> PhysicalUnifiedString::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<USSRInsertionState>(context, insert_to_ussr.size());
}

unique_ptr<GlobalOperatorState> PhysicalUnifiedString::GetGlobalOperatorState(ClientContext &context) const {
	return make_uniq<USSRInsertionGState>(context);
}
//atomic<idx_t > counter{0};
void PhysicalUnifiedString::USSR_insertion_loop(data_ptr_t dict_strings, idx_t count, ClientContext &context,
                                                const vector<idx_t> &priority_insertion, ValidityMask &validity, bool exists_prio) const {
	auto start = reinterpret_cast<string_t *>(dict_strings);

	if (priority_insertion.empty() && !exists_prio) {
		for (idx_t i = 0; i < count; i++) {
			if(!validity.RowIsValid(i)){
				continue;
			}
			start[i] = context.GetCurrentQueryUssr().insert(start[i]);
		}
	} else {
//		Printer::Print(to_string(priority_insertion.size()));
//		counter.fetch_add(priority_insertion.size());
		for (auto string_idx : priority_insertion) {
			if(!validity.RowIsValid(string_idx)){
				continue;
			}
			start[string_idx] = context.GetCurrentQueryUssr().insert(start[string_idx]);
		}
	}
}

} // namespace duckdb
