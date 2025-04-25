#include "duckdb/optimizer/UnifiedStringDictionary_Optimizer.h"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/printer.hpp"

namespace duckdb {


	unique_ptr<LogicalOperator> USSR_optimizer::CheckUnifiedDictionary(unique_ptr<LogicalOperator> op) {
    	op = Rewrite(std::move(op));
	    Insert_USSR_Operator(data_sources[0]);
	    return op;
    }

//    void USSR_optimizer::Insert_USSR_Operator(unique_ptr<LogicalOperator> op) {
//
//
//	    auto bindings_before = op->GetColumnBindings();
//	    vector<unique_ptr<Expression>> projections;
//	    projections.reserve(op->types.size());
//	    op = Get_USSR_Expressions(std::move(op), projections);
//	    const auto table_index = optimizer->binder.GenerateTableIndex();
//	    auto ussr_projection = make_uniq<LogicalProjection>(table_index, std::move(projections));
//	    if(op->has_estimated_cardinality){
//		    ussr_projection->SetEstimatedCardinality(op->estimated_cardinality);
//	    }
//
//
//	    ussr_projection->ResolveOperatorTypes();
//
//	    ussr_projection->children.emplace_back(std::move(op->children[0]));
//	    op->children[0] = std::move(ussr_projection);
//	    //	    op = std::move(ussr_projection);
//
//	    auto bindings_after = op->GetColumnBindings();
//
//	    auto types = op->types;
//
//
//	    auto &replacement_bindings = replacer->replacement_bindings;
//	    for (idx_t col_idx = 0; col_idx < bindings_before.size(); col_idx++) {
//		    const auto &old_binding = bindings_before[col_idx];
//		    const auto &new_binding = bindings_after[col_idx];
//		    const auto &new_type = types[col_idx];
//		    replacement_bindings.emplace_back(old_binding, new_binding, new_type);
//	    }
//
//	    // Make sure we stop at the compress operator when replacing bindings
//
//
//
//    }

    void USSR_optimizer::Insert_USSR_Operator(optional_ptr<LogicalOperator> op) {


	    auto bindings_before = op->children[0]->GetColumnBindings();
		vector<unique_ptr<Expression>> projections;
	    projections.reserve(op->types.size());
	    auto bindings = op->children[0]->GetColumnBindings();

	    vector<unique_ptr<Expression>> old_expressions;
	    old_expressions.reserve(bindings.size());
	    for (idx_t i = 0; i < op->types.size(); ++i) {
		    old_expressions.emplace_back(make_uniq<BoundColumnRefExpression>(op->types[i], bindings[i]));
	    }


	    for (auto& expression: old_expressions) {
		    if (expression->return_type == LogicalType::VARCHAR) {
			    auto &child_colref_expr = expression->Cast<BoundColumnRefExpression>();
			    auto colref_expr =
			        make_uniq<BoundColumnRefExpression>(LogicalType::VARCHAR, child_colref_expr.binding);
			    projections.emplace_back(std::move(colref_expr));
		    } else {
			    projections.emplace_back(std::move(expression));
		    }
	    }
	    const auto table_index = optimizer->binder.GenerateTableIndex();
	    auto ussr_projection = make_uniq<LogicalProjection>(table_index, std::move(projections));
	    if(op->has_estimated_cardinality){
		    ussr_projection->SetEstimatedCardinality(op->estimated_cardinality);
	    }


	    ussr_projection->ResolveOperatorTypes();

	    ussr_projection->children.emplace_back(std::move(op->children[0]));
	    op->children[0] = std::move(ussr_projection);
//	    op = std::move(ussr_projection);

	    auto bindings_after = op->children[0]->GetColumnBindings();

	    auto types = op->types;


	    auto &replacement_bindings = replacer->replacement_bindings;
	    for (idx_t col_idx = 0; col_idx < bindings_before.size(); col_idx++) {
		    const auto &old_binding = bindings_before[col_idx];
		    const auto &new_binding = bindings_after[col_idx];
		    const auto &new_type = types[col_idx];
		    replacement_bindings.emplace_back(old_binding, new_binding);
	    }

	    // Make sure we stop at the compress operator when replacing bindings
	    replacer->stop_operator = op->children[0].get();
	    replacer->VisitOperator(*root);



    }

    unique_ptr<LogicalOperator> USSR_optimizer::Get_USSR_Expressions(unique_ptr<LogicalOperator> op, vector<unique_ptr<Expression>>& ret_expressions) {

	    auto bindings = op->GetColumnBindings();
	    vector<unique_ptr<Expression>> old_expressions;
	    old_expressions.reserve(bindings.size());
	    for (idx_t i = 0; i < op->types.size(); ++i) {
		    old_expressions.emplace_back(make_uniq<BoundColumnRefExpression>(op->types[i], bindings[i]));
	    }


		for (auto& expression: old_expressions) {
			    if (expression->return_type == LogicalType::VARCHAR) {
				    auto &child_colref_expr = expression->Cast<BoundColumnRefExpression>();
				    auto colref_expr =
				        make_uniq<BoundColumnRefExpression>(LogicalType::VARCHAR, child_colref_expr.binding);
				    ret_expressions.emplace_back(std::move(colref_expr));
			    } else {
			        ret_expressions.emplace_back(std::move(expression));
			    }
	    }
	    return op;
    }



    unique_ptr<LogicalOperator> USSR_optimizer::Rewrite(unique_ptr<LogicalOperator> op){
	    op->PrintColumnBindings();
	    op->ResolveOperatorTypes();
	    // Depth-first-search post-order
	    for (idx_t i = 0; i < op->children.size(); ++i) {
		    op->children[i] = Rewrite(std::move(op->children[i]));

	    }

	    for (idx_t i = 0; i < op->children.size(); ++i) {
		    if(op->children[i]->type == LogicalOperatorType::LOGICAL_GET){
			    for (const auto& type : op->types) {
				    if(type.id() == LogicalTypeId::VARCHAR){
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
	    case LogicalOperatorType::LOGICAL_ORDER_BY:{
		    for (const auto& type : op->types) {
			    if(type.id() == LogicalTypeId::VARCHAR){
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

}