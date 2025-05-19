#include "duckdb/optimizer/UnifiedStringDictionary_Optimizer.h"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/planner/operator/logical_ussr_insertion.h"

namespace duckdb {

unique_ptr<LogicalOperator> USSR_optimizer::CheckUnifiedDictionary(unique_ptr<LogicalOperator> op) {
	op = Rewrite(std::move(op));
	for (auto &ds : data_sources) {
		Insert_USSR_Operator(ds);
	}
	return op;
}

void USSR_optimizer::Insert_USSR_Operator(optional_ptr<LogicalOperator> op) {
	for (idx_t i = 0; i < op->children.size(); ++i) {
		vector<bool> ussr_insert_vec;
		//		D_ASSERT(op->children[i]->type == LogicalOperatorType::LOGICAL_GET);
		for (auto &type : op->children[i]->types) {
			if (type == LogicalType::VARCHAR) {
				ussr_insert_vec.push_back(true);
			} else {
				ussr_insert_vec.push_back(false);
			}
		}

		auto new_operator = make_uniq<LogicalUSSRInsertion>(std::move(ussr_insert_vec));
		new_operator->children.push_back(std::move(op->children[i]));
		op->children[i] = std::move(new_operator);

		op->ResolveOperatorTypes();
	}
}

unique_ptr<LogicalOperator> USSR_optimizer::Rewrite(unique_ptr<LogicalOperator> op) {
	op->ResolveOperatorTypes();
	// Depth-first-search post-order
	for (idx_t i = 0; i < op->children.size(); ++i) {
		op->children[i] = Rewrite(std::move(op->children[i]));
	}

	for (idx_t i = 0; i < op->children.size(); ++i) {
		if (op->children[i]->type == LogicalOperatorType::LOGICAL_GET) {
			for (const auto &type : op->types) {
				if (type.id() == LogicalTypeId::VARCHAR) {
					data_sources.push_back(op.get());
					//					    Insert_USSR_Operator(op);
					break;
				}
			}
		}
	}

	switch (op->type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ORDER_BY: {
		for (const auto &type : op->types) {
			if (type.id() == LogicalTypeId::VARCHAR) {
				requires_ussr = true;
			}
		}
		break;
	}
	default:
		break;
	}
	return op;
}

} // namespace duckdb
