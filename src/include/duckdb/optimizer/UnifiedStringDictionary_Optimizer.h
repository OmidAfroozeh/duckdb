#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class Optimizer;
class BoundColumnRefExpression;

class USSR_optimizer{
public:
	explicit USSR_optimizer(Optimizer* optimizer, optional_ptr<LogicalOperator> r){
		replacer = make_uniq<ColumnBindingReplacer>();
		this->optimizer = optimizer;
		root = r;
		requires_ussr = false;
	}




	unique_ptr<LogicalOperator> CheckUnifiedDictionary(unique_ptr<LogicalOperator> op);

	unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);

	void Insert_USSR_Operator(unique_ptr<LogicalOperator> op);

private:


	unique_ptr<ColumnBindingReplacer> replacer;

	optional_ptr<LogicalOperator> root;

	Optimizer* optimizer;

	bool requires_ussr;

	vector<optional_ptr<LogicalOperator>> data_sources;


	bool OutputStrings(LogicalOperator &op);

	unique_ptr<LogicalOperator> prev_op;

	unique_ptr<LogicalOperator> Get_USSR_Expressions(unique_ptr<LogicalOperator> op, vector<unique_ptr<Expression>>& ret_expressions);

	void Insert_USSR_Operator(optional_ptr<LogicalOperator> op);

};

} // namespace duckdb
