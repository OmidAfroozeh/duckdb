#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/update_extensions_info.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

class PhysicalUnifiedString : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::UNIFIED_STRINGS;

public:
	PhysicalUnifiedString(vector<LogicalType> types, vector<bool> insert_to_ussr, idx_t estimated_cardinality) : PhysicalOperator(TYPE, std::move(types), estimated_cardinality){
		this->insert_to_ussr = std::move(insert_to_ussr);
	};

	bool ParallelOperator() const override {
		return true;
	}

	bool IsSource() const override{
		return false;
	}

	bool IsSink() const override{
		return false;
	}
	bool RequiresFinalExecute() const override{
		return true;
	}

	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;

	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk,
	                                        GlobalOperatorState &gstate, OperatorState &state) const override;

	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override;


	vector<DataChunk> cached;

	static constexpr const idx_t USSR_CACHING_THRESHOLD = 50;

public:

private:
	vector<bool> insert_to_ussr;
//	void evaluate_strings(ColumnSegment col_seg, BlockHandle &handle);
	void evaluate_strings_colseg(string& segment_str, vector<bool>& result) const;
	void USSR_insertion_loop(data_ptr_t dict_strings, idx_t count, ClientContext &context,
	                         const vector<idx_t> &priority_insertion) const;
};

} // namespace duckdb
