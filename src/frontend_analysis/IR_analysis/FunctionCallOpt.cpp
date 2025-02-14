/*
 *
 *                   _/_/_/    _/_/   _/    _/ _/_/_/    _/_/
 *                  _/   _/ _/    _/ _/_/  _/ _/   _/ _/    _/
 *                 _/_/_/  _/_/_/_/ _/  _/_/ _/   _/ _/_/_/_/
 *                _/      _/    _/ _/    _/ _/   _/ _/    _/
 *               _/      _/    _/ _/    _/ _/_/_/  _/    _/
 *
 *             ***********************************************
 *                              PandA Project
 *                     URL: http://panda.dei.polimi.it
 *                       Politecnico di Milano - DEIB
 *                        System Architectures Group
 *             ***********************************************
 *              Copyright (C) 2021-2022 Politecnico di Milano
 *
 *   This file is part of the PandA framework.
 *
 *   The PandA framework is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
/**
 * @file FunctionCallOpt.cpp
 *
 * @author Michele Fiorito <michele.fiorito@polimi.it>
 * $Revision$
 * $Date$
 * Last modified by $Author$
 *
 */

#include "FunctionCallOpt.hpp"

#include "application_manager.hpp"
#include "basic_block.hpp"
#include "basic_blocks_graph_constructor.hpp"
#include "call_graph.hpp"
#include "call_graph_manager.hpp"
#include "design_flow_graph.hpp"
#include "design_flow_manager.hpp"
#include "ext_tree_node.hpp"
#include "function_behavior.hpp"
#include "tree_basic_block.hpp"
#include "tree_helper.hpp"
#include "tree_manager.hpp"
#include "tree_manipulation.hpp"
#include "tree_node.hpp"
#include "tree_reindex.hpp"

#include "Dominance.hpp"
#include "Parameter.hpp"
#include "dbgPrintHelper.hpp"      // for DEBUG_LEVEL_
#include "string_manipulation.hpp" // for GET_CLASS

#define PARAMETER_INLINE_MAX_COST "inline-max-cost"
#define DEAFULT_MAX_INLINE_CONST 65

CustomSet<unsigned int> FunctionCallOpt::always_inline;
CustomSet<unsigned int> FunctionCallOpt::never_inline;
CustomMap<unsigned int, CustomSet<std::tuple<unsigned int, FunctionOptType>>> FunctionCallOpt::opt_call;
size_t FunctionCallOpt::max_inline_cost = DEAFULT_MAX_INLINE_CONST;
unsigned FunctionCallOpt::version_uid = 1U;

static CustomUnorderedMap<kind, size_t> op_costs = {{mult_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {widen_mult_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {widen_mult_hi_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {widen_mult_lo_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {trunc_div_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {exact_div_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {round_div_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {ceil_div_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {floor_div_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {trunc_mod_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {round_mod_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {ceil_mod_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {floor_mod_expr_K, DEAFULT_MAX_INLINE_CONST},
                                                    {view_convert_expr_K, 0},
                                                    {nop_expr_K, 0},
                                                    {call_expr_K, 8}};

FunctionCallOpt::FunctionCallOpt(const ParameterConstRef Param, const application_managerRef _AppM,
                                 unsigned int _function_id, const DesignFlowManagerConstRef _design_flow_manager)
    : FunctionFrontendFlowStep(_AppM, _function_id, FUNCTION_CALL_OPT, _design_flow_manager, Param), caller_bb()
{
   debug_level = Param->get_class_debug_level(GET_CLASS(*this), DEBUG_LEVEL_NONE);
   if(Param->IsParameter(PARAMETER_INLINE_MAX_COST))
   {
      max_inline_cost = Param->GetParameter<size_t>(PARAMETER_INLINE_MAX_COST);
   }
   if(Param->isOption(OPT_inline_functions))
   {
      const auto TM = AppM->get_tree_manager();
      const auto fnames = SplitString(Param->getOption<std::string>(OPT_inline_functions), ",");
      for(const auto& fname_cond : fnames)
      {
         const auto toks = SplitString(fname_cond, "=");
         const auto& fname = toks.at(0);
         const auto fnode = TM->GetFunction(fname);
         if(fnode)
         {
            auto& inline_set = (!(toks.size() > 1) || toks.at(1) == "1") ? always_inline : never_inline;
            inline_set.insert(fnode->index);
            INDENT_OUT_MEX(OUTPUT_LEVEL_MINIMUM, output_level,
                           "Required " + STR((!(toks.size() > 1) || toks.at(1) == "1") ? "always" : "never") +
                               " inline for function " + fname);
         }
         else
         {
            INDENT_OUT_MEX(OUTPUT_LEVEL_MINIMUM, output_level, "Required inline function not found: " + fname);
         }
      }
   }
}

FunctionCallOpt::~FunctionCallOpt() = default;

const CustomUnorderedSet<std::pair<FrontendFlowStepType, FrontendFlowStep::FunctionRelationship>>
FunctionCallOpt::ComputeFrontendRelationships(const DesignFlowStep::RelationshipType relationship_type) const
{
   CustomUnorderedSet<std::pair<FrontendFlowStepType, FrontendFlowStep::FunctionRelationship>> relationships;
   switch(relationship_type)
   {
      case(DEPENDENCE_RELATIONSHIP):
      {
         relationships.insert(std::make_pair(COMPLETE_BB_GRAPH, SAME_FUNCTION));
         relationships.insert(std::make_pair(COMPLETE_CALL_GRAPH, WHOLE_APPLICATION));
         relationships.insert(std::make_pair(FUNCTION_CALL_OPT, CALLED_FUNCTIONS));
         relationships.insert(std::make_pair(FUNCTION_CALL_TYPE_CLEANUP, SAME_FUNCTION));
         break;
      }
      case(PRECEDENCE_RELATIONSHIP):
      {
         break;
      }
      case(INVALIDATION_RELATIONSHIP):
      {
         if(GetStatus() == DesignFlowStep_Status::SUCCESS)
         {
            if(!parameters->getOption<int>(OPT_gcc_openmp_simd))
            {
               relationships.insert(std::make_pair(BIT_VALUE, SAME_FUNCTION));
            }
            relationships.insert(std::make_pair(COMPLETE_BB_GRAPH, SAME_FUNCTION));
            relationships.insert(std::make_pair(DETERMINE_MEMORY_ACCESSES, SAME_FUNCTION));
         }
         break;
      }
      default:
      {
         THROW_UNREACHABLE("");
      }
   }
   return relationships;
}

bool FunctionCallOpt::HasToBeExecuted() const
{
   for(const auto& id_bb : caller_bb)
   {
      if(AppM->CGetFunctionBehavior(id_bb.first)->GetBBVersion() != id_bb.second)
      {
         return true;
      }
   }
   return FunctionFrontendFlowStep::HasToBeExecuted();
}

void FunctionCallOpt::Initialize()
{
   caller_bb.clear();
}

DesignFlowStep_Status FunctionCallOpt::InternalExec()
{
   THROW_ASSERT(!parameters->IsParameter("function-opt") || parameters->GetParameter<bool>("function-opt"),
                "Function call optimization should not be executed");
   INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "-->");
   const auto TM = AppM->get_tree_manager();
   const auto fd = GetPointerS<function_decl>(TM->GetTreeNode(function_id));
   THROW_ASSERT(fd->body, "");
   const auto sl = GetPointerS<statement_list>(GET_NODE(fd->body));
   bool modified = false;
   const auto opt_stmts = opt_call.find(function_id);
   if(opt_stmts != opt_call.end())
   {
      INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "Performing call optimization:");
      INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "-->");
      tree_manipulationRef tree_man(new tree_manipulation(TM, parameters, AppM));
      for(const auto& stmt_opt : opt_stmts->second)
      {
         const auto& stmt_id = std::get<0>(stmt_opt);
         const auto& opt_type = std::get<1>(stmt_opt);
         const auto stmt = TM->CGetTreeReindex(stmt_id);
         if(stmt)
         {
            const auto gn = GetPointerS<const gimple_node>(GET_CONST_NODE(stmt));
            const auto bb_it = sl->list_of_bloc.find(gn->bb_index);
            if(gn->bb_index != 0 && bb_it != sl->list_of_bloc.end())
            {
               const auto& bb = bb_it->second;
               if(std::find_if(bb->CGetStmtList().cbegin(), bb->CGetStmtList().cend(), [&](const tree_nodeRef& tn) {
                     return GET_INDEX_CONST_NODE(tn) == stmt_id;
                  }) != bb->CGetStmtList().cend())
               {
                  if(!AppM->ApplyNewTransformation())
                  {
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                    "---Max transformations reached, skipping function inlining...");
                     break;
                  }
                  if(opt_type == FunctionOptType::INLINE)
                  {
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                    "Inlining required for call statement " + GET_CONST_NODE(stmt)->ToString());
                     tree_man->InlineFunctionCall(stmt, bb, fd);
                     AppM->RegisterTransformation(GetName(), nullptr);
                  }
                  else if(opt_type == FunctionOptType::VERSION)
                  {
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                    "Versioning required for call statement " + GET_CONST_NODE(stmt)->ToString());
                     const auto call_node = GET_CONST_NODE(stmt);
                     tree_nodeRef called_fn;
                     tree_nodeRef ret_val = nullptr;
                     std::vector<tree_nodeRef> const* args;
                     if(call_node->get_kind() == gimple_call_K)
                     {
                        const auto gc = GetPointerS<const gimple_call>(call_node);
                        called_fn = gc->fn;
                        args = &gc->args;
                     }
                     else if(call_node->get_kind() == gimple_assign_K)
                     {
                        const auto ga = GetPointerS<const gimple_assign>(call_node);
                        const auto ce = GetPointer<const call_expr>(GET_CONST_NODE(ga->op1));
                        THROW_ASSERT(ce, "Assign statement does not contain a function call: " + ga->ToString());
                        called_fn = ce->fn;
                        args = &ce->args;
                        ret_val = ga->op0;
                     }
                     else
                     {
                        THROW_UNREACHABLE("Unsupported call statement: " + call_node->ToString());
                     }
                     if(GET_CONST_NODE(called_fn)->get_kind() == addr_expr_K)
                     {
                        called_fn = GetPointerS<const unary_expr>(GET_CONST_NODE(called_fn))->op;
                     }
                     THROW_ASSERT(GET_CONST_NODE(called_fn)->get_kind() == function_decl_K,
                                  "Call statement should address a function declaration");

                     const auto version_fn = tree_man->CloneFunction(called_fn, "_const" + STR(version_uid++));
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "---Call before versioning : " + STR(stmt));
                     if(ret_val)
                     {
                        const auto ce = tree_man->CreateCallExpr(version_fn, *args, BUILTIN_SRCP);
                        const auto ga = GetPointerS<const gimple_assign>(call_node);
                        AppM->GetCallGraphManager()->RemoveCallPoint(function_id, called_fn->index, stmt->index);
                        TM->ReplaceTreeNode(stmt, ga->op1, ce);
                        CallGraphManager::addCallPointAndExpand(already_visited, AppM, function_id, version_fn->index,
                                                                stmt->index, FunctionEdgeInfo::CallType::direct_call,
                                                                DEBUG_LEVEL_NONE);
                        AppM->RegisterTransformation(GetName(), stmt);
                        INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                       "---Call after versioning  : " + STR(stmt));
                     }
                     else
                     {
                        const auto version_call =
                            tree_man->create_gimple_call(version_fn, *args, function_id, BUILTIN_SRCP, bb->number);
                        bb->Replace(stmt, version_call, true, AppM);
                        AppM->RegisterTransformation(GetName(), version_call);
                        INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                       "---Call after versioning  : " + STR(version_call));
                     }
                  }
                  else
                  {
                     THROW_UNREACHABLE("Unknown function call optimization type: " + STR(opt_type));
                  }
                  modified = true;
               }
               else
               {
                  INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                 "Statement " + STR(stmt_id) + " was not present in BB" + STR(bb->number));
               }
            }
            else
            {
               INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                              "BB" + STR(gn->bb_index) + " was not found in current function");
            }
         }
         else
         {
            INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                           "Statement " + STR(stmt_id) + " was not in tree manager");
         }
      }
      INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "<--");
      INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "Performed call inlining");
      opt_call.erase(function_id);
   }

   if(parameters->isOption(OPT_top_design_name) &&
      TM->GetFunction(parameters->getOption<std::string>(OPT_top_design_name))->index == function_id)
   {
      INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                     "Current function is marked as top RTL design, skipping...");
   }
   else
   {
      const auto CGM = AppM->CGetCallGraphManager();
      const auto CG = CGM->CGetCallGraph();
      const auto function_v = CGM->GetVertex(function_id);
      const auto call_count = [&]() -> size_t {
         size_t call_points = 0u;
         InEdgeIterator it, it_end;
         for(boost::tie(it, it_end) = boost::in_edges(function_v, *CG); it != it_end; ++it)
         {
            const auto caller_info = CG->CGetFunctionEdgeInfo(*it);
            call_points += static_cast<size_t>(caller_info->direct_call_points.size());
            call_points += static_cast<size_t>(caller_info->indirect_call_points.size());
            call_points += static_cast<size_t>(caller_info->function_addresses.size());
         }
         return call_points;
      }();
      if(call_count != 0)
      {
         const auto loop_count = detect_loops(sl);
         bool has_simd = false;
         bool has_memory = false;
         const auto body_cost = compute_cost(sl, has_simd, has_memory);
         const auto omp_simd_enabled = parameters->getOption<int>(OPT_gcc_openmp_simd);
         has_simd &= omp_simd_enabled;
         const bool force_inline =
             always_inline.count(function_id) || ((body_cost * call_count) <= max_inline_cost) || call_count == 1;
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "Current function information:");
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "---Internal loops : " + STR(loop_count));
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "---Call points    : " + STR(call_count));
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "---Body cost      : " + STR(body_cost));
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                        "---Memory access  : " + STR(has_memory ? "yes" : "no"));
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "---OpenMP SIMD    : " + STR(has_simd ? "yes" : "no"));
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                        "---Force inline   : " + STR(force_inline ? "yes" : "no"));
         if(!has_simd && !has_memory)
         {
            InEdgeIterator ie, ie_end;
            for(boost::tie(ie, ie_end) = boost::in_edges(function_v, *CG); ie != ie_end; ++ie)
            {
               const auto caller_id = CGM->get_function(ie->m_source);
               caller_bb.insert(std::make_pair(caller_id, AppM->CGetFunctionBehavior(caller_id)->GetBBVersion()));
               INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                              "Analysing call points from " +
                                  tree_helper::print_function_name(
                                      TM, GetPointerS<const function_decl>(TM->CGetTreeNode(caller_id))));
               INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "-->");
               const auto caller_node = TM->CGetTreeNode(caller_id);
               THROW_ASSERT(caller_node->get_kind() == function_decl_K, "");
               const auto caller_fd = GetPointerS<const function_decl>(caller_node);
               THROW_ASSERT(caller_fd->body, "");
               const auto caller_sl = GetPointerS<const statement_list>(GET_CONST_NODE(caller_fd->body));
               if(omp_simd_enabled)
               {
                  const auto caller_has_simd = tree_helper::has_omp_simd(caller_sl);
                  if(caller_has_simd)
                  {
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                    "<--Caller has OpenMP SIMD constructs, analysis postponed");
                     continue;
                  }
               }
               const auto caller_info = CG->CGetFunctionEdgeInfo(*ie);
               bool all_inlined = true;
               for(const auto& call_id : caller_info->direct_call_points)
               {
                  const auto call_stmt = TM->CGetTreeNode(call_id);
                  INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                 "Analysing direct call point " + STR(call_stmt));
                  const auto call_gn = GetPointerS<const gimple_node>(call_stmt);
                  if(call_gn->vdef || call_gn->vuses.size() || call_gn->vovers.size())
                  {
                     // TODO: alias analysis is not able to handle inlining yet
                     all_inlined = false;
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                    "---Call statement carries alias dependencies, skipping...");
                     continue;
                  }
                  if(force_inline)
                  {
                     if(!omp_simd_enabled || loop_count == 0)
                     {
                        const auto is_unique_bb_call = [&]() -> bool {
                           THROW_ASSERT(caller_sl->list_of_bloc.count(call_gn->bb_index),
                                        "BB" + STR(call_gn->bb_index) + " not found in function " +
                                            tree_helper::print_function_name(TM, caller_fd) + " (" + STR(call_id) +
                                            ")");
                           const auto bb = caller_sl->list_of_bloc.at(call_gn->bb_index);
                           for(const auto& tn : bb->CGetStmtList())
                           {
                              if(tn->index != call_gn->index &&
                                 (GET_CONST_NODE(tn)->get_kind() == gimple_call_K ||
                                  (GET_CONST_NODE(tn)->get_kind() == gimple_assign_K &&
                                   GET_CONST_NODE(GetPointerS<const gimple_assign>(GET_CONST_NODE(tn))->op1)
                                           ->get_kind() == call_expr_K)))
                              {
                                 return false;
                              }
                           }
                           return true;
                        }();
                        if(is_unique_bb_call || loop_count == 0)
                        {
                           opt_call[caller_id].insert(std::make_pair(call_id, FunctionOptType::INLINE));
                           INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                          "---" +
                                              STR(always_inline.count(function_id) ? "Always inline function" :
                                                                                     "Low body cost function") +
                                              ", inlining required");
                           continue;
                        }
                        else
                        {
                           INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                          "---BB" + STR(call_gn->bb_index) + " from function " +
                                              tree_helper::print_function_name(TM, caller_fd) +
                                              " has multiple call points, skipping...");
                        }
                     }
                     else
                     {
                        INDENT_DBG_MEX(
                            DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                            "---Inlining of functions with internal loops disabled with OpenMP, skipping...");
                     }
                  }
                  const auto all_const_args = HasConstantArgs(TM->CGetTreeReindex(call_id));
                  if(all_const_args && loop_count == 0)
                  {
                     opt_call[caller_id].insert(std::make_pair(call_id, FunctionOptType::VERSION));
                     INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                                    "---Current call has all constant arguments, versioning required");
                     continue;
                  }
                  all_inlined = false;
               }
               if(all_inlined)
               {
                  caller_bb.erase(caller_id);
               }
               INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "<--");
               INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                              "Analysed call points from " +
                                  tree_helper::print_function_name(
                                      TM, GetPointerS<const function_decl>(TM->CGetTreeNode(caller_id))));
            }
         }
         else
         {
            INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                           "Current function has " +
                               STR((has_simd && has_memory) ? "OpenMP SIMD and memory access" :
                                                              (has_simd ? "OpenMP SIMD" : "memory access")) +
                               " constructs, inlining postponed");
         }
      }
      else
      {
         INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level,
                        "Current function has zero call points, skipping analysis...");
      }
   }
   INDENT_DBG_MEX(DEBUG_LEVEL_VERY_PEDANTIC, debug_level, "<--");
   if(modified)
   {
      function_behavior->UpdateBBVersion();
      function_behavior->UpdateBitValueVersion();
      return DesignFlowStep_Status::SUCCESS;
   }
   return DesignFlowStep_Status::UNCHANGED;
}

void FunctionCallOpt::RequestCallOpt(const tree_nodeConstRef& call_stmt, unsigned int caller_id, FunctionOptType opt)
{
#if HAVE_ASSERTS
   const auto stmt_kind = GET_CONST_NODE(call_stmt)->get_kind();
   bool is_call = stmt_kind == gimple_call_K;
   if(stmt_kind == gimple_assign_K)
   {
      const auto rhs = GetPointerS<const gimple_assign>(GET_CONST_NODE(call_stmt))->op1;
      const auto rhs_kind = GET_CONST_NODE(rhs)->get_kind();
      is_call |= rhs_kind == call_expr_K || rhs_kind == aggr_init_expr_K;
   }
#endif
   THROW_ASSERT(is_call, "Statement does not contain a call");
   opt_call[caller_id].insert(std::make_pair(GET_INDEX_CONST_NODE(call_stmt), opt));
}

bool FunctionCallOpt::HasConstantArgs(const tree_nodeConstRef& call_stmt)
{
   const auto all_constant = [](const std::vector<tree_nodeRef>& tns) {
      for(const auto& tn : tns)
      {
         if(!tree_helper::IsConstant(tn))
         {
            return false;
         }
      }
      return true;
   };
   const auto call_kind = GET_CONST_NODE(call_stmt)->get_kind();
   if(call_kind == gimple_call_K)
   {
      const auto gc = GetPointerS<const gimple_call>(GET_CONST_NODE(call_stmt));
      return all_constant(gc->args);
   }
   else if(call_kind == gimple_assign_K)
   {
      const auto ga = GetPointerS<const gimple_assign>(GET_CONST_NODE(call_stmt));
      THROW_ASSERT(GET_CONST_NODE(ga->op1)->get_kind() == call_expr_K, "");
      const auto ce = GetPointerS<const call_expr>(GET_CONST_NODE(ga->op1));
      return all_constant(ce->args);
   }
   THROW_UNREACHABLE("Expected call statement: " + tree_node::GetString(call_kind) + " - " + STR(call_stmt));
   return false;
}

size_t FunctionCallOpt::compute_cost(const statement_list* body, bool& has_simd, bool& has_memory)
{
   size_t total_cost = 0;
   has_memory = false;
   has_simd = false;
   for(const auto& bb : body->list_of_bloc)
   {
      for(const auto& stmt_rdx : bb.second->CGetStmtList())
      {
         const auto stmt = GET_CONST_NODE(stmt_rdx);
         if(stmt->get_kind() == gimple_assign_K)
         {
            // if(tree_helper::IsLoad(stmt, function_behavior->get_function_mem()) || tree_helper::IsStore(stmt,
            // function_behavior->get_function_mem()))
            // {
            //    has_memory = true;
            // }
            const auto ga = GetPointerS<const gimple_assign>(stmt);
            const auto op_kind = GET_CONST_NODE(ga->op1)->get_kind();
            const auto op_cost = op_costs.find(op_kind);
            if(op_cost != op_costs.end())
            {
               total_cost += op_cost->second;
            }
            else
            {
               total_cost += 1;
            }
         }
         else if(stmt->get_kind() == gimple_cond_K || stmt->get_kind() == gimple_call_K)
         {
            total_cost += 8;
         }
         else if(stmt->get_kind() == gimple_multi_way_if_K)
         {
            const auto mw = GetPointerS<const gimple_multi_way_if>(stmt);
            total_cost += mw->list_of_cond.size() * 8ULL;
         }
         else if(stmt->get_kind() == gimple_asm_K)
         {
            const auto ga = GetPointerS<const gimple_asm>(stmt);
            total_cost += ga->str.size() / 5;
         }
         else if(stmt->get_kind() == gimple_pragma_K)
         {
            const auto gp = GetPointerS<const gimple_pragma>(stmt);
            if(gp->scope && GetPointer<const omp_pragma>(GET_CONST_NODE(gp->scope)))
            {
               const auto sp = GetPointer<const omp_simd_pragma>(GET_CONST_NODE(gp->directive));
               if(sp)
               {
                  has_simd = true;
               }
            }
         }
      }
   }
   return total_cost;
}

size_t FunctionCallOpt::detect_loops(const statement_list* body) const
{
   /// store the IR BB graph ala boost::graph
   BBGraphsCollectionRef bb_graphs_collection(
       new BBGraphsCollection(BBGraphInfoRef(new BBGraphInfo(AppM, function_id)), parameters));
   BBGraph cfg(bb_graphs_collection, CFG_SELECTOR);
   CustomUnorderedMap<unsigned int, vertex> inverse_vertex_map;
   /// add vertices
   for(const auto& block : body->list_of_bloc)
   {
      inverse_vertex_map.insert(
          std::make_pair(block.first, bb_graphs_collection->AddVertex(BBNodeInfoRef(new BBNodeInfo(block.second)))));
   }

   /// add edges
   for(const auto& curr_bb_pair : body->list_of_bloc)
   {
      const auto& curr_bbi = curr_bb_pair.first;
      const auto& curr_bb = curr_bb_pair.second;
      for(const auto& lop : curr_bb->list_of_pred)
      {
         THROW_ASSERT(static_cast<bool>(inverse_vertex_map.count(lop)),
                      "BB" + STR(lop) + " (successor of BB" + STR(curr_bbi) + ") does not exist");
         bb_graphs_collection->AddEdge(inverse_vertex_map.at(lop), inverse_vertex_map.at(curr_bbi), CFG_SELECTOR);
      }

      for(const auto& los : curr_bb->list_of_succ)
      {
         if(los == bloc::EXIT_BLOCK_ID)
         {
            bb_graphs_collection->AddEdge(inverse_vertex_map.at(curr_bbi), inverse_vertex_map.at(los), CFG_SELECTOR);
         }
      }

      if(curr_bb->list_of_succ.empty())
      {
         bb_graphs_collection->AddEdge(inverse_vertex_map.at(curr_bbi), inverse_vertex_map.at(bloc::EXIT_BLOCK_ID),
                                       CFG_SELECTOR);
      }
   }
   bb_graphs_collection->AddEdge(inverse_vertex_map.at(bloc::ENTRY_BLOCK_ID),
                                 inverse_vertex_map.at(bloc::EXIT_BLOCK_ID), CFG_SELECTOR);
   std::map<size_t, UnorderedSetStdStable<vertex>> sccs;
   cfg.GetStronglyConnectedComponents(sccs);
   size_t loop_count = 0;
   for(const auto& scc : sccs)
   {
      if(scc.second.size() > 1)
      {
         loop_count++;
         continue;
      }
      THROW_ASSERT(scc.second.size() == 1, "");
      const auto bb_v = *scc.second.begin();
      const auto& bb = cfg.CGetBBNodeInfo(bb_v)->block;
      if(std::find(bb->list_of_succ.begin(), bb->list_of_succ.end(), bb->number) != bb->list_of_succ.end())
      {
         loop_count++;
      }
   }
   return loop_count;
}