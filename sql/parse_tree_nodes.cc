/* Copyright (c) 2013, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/parse_tree_nodes.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "field_types.h"
#include "m_ctype.h"
#include "m_string.h"
#include "mem_root_deque.h"
#include "my_alloc.h"
#include "my_dbug.h"
#include "mysql/mysql_lex_string.h"
#include "mysql/udf_registration_types.h"
#include "mysql_com.h"
#include "scope_guard.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/create_field.h"
#include "sql/dd/info_schema/show.h"      // build_show_...
#include "sql/dd/types/abstract_table.h"  // dd::enum_table_type::BASE_TABLE
#include "sql/dd/types/column.h"
#include "sql/derror.h"  // ER_THD
#include "sql/field.h"
#include "sql/gis/srid.h"
#include "sql/intrusive_list_iterator.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_timefunc.h"
#include "sql/key_spec.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"                   // global_system_variables
#include "sql/opt_explain_json.h"         // Explain_format_JSON
#include "sql/opt_explain_traditional.h"  // Explain_format_traditional
#include "sql/parse_location.h"
#include "sql/parse_tree_column_attrs.h"  // PT_field_def_base
#include "sql/parse_tree_hints.h"
#include "sql/parse_tree_partitions.h"  // PT_partition
#include "sql/parse_tree_window.h"      // PT_window
#include "sql/parser_yystype.h"
#include "sql/query_options.h"
#include "sql/query_result.h"
#include "sql/sp.h"  // sp_add_used_routine
#include "sql/sp_head.h"
#include "sql/sp_instr.h"  // sp_instr_set
#include "sql/sp_pcontext.h"
#include "sql/sql_base.h"  // find_temporary_table
#include "sql/sql_call.h"  // Sql_cmd_call...
#include "sql/sql_class.h"
#include "sql/sql_cmd.h"
#include "sql/sql_cmd_ddl_table.h"
#include "sql/sql_const.h"
#include "sql/sql_data_change.h"
#include "sql/sql_delete.h"  // Sql_cmd_delete...
#include "sql/sql_do.h"      // Sql_cmd_do...
#include "sql/sql_error.h"
#include "sql/sql_insert.h"  // Sql_cmd_insert...
#include "sql/sql_parse.h"
#include "sql/sql_select.h"  // Sql_cmd_select...
#include "sql/sql_show.h"    // Sql_cmd_show...
#include "sql/sql_show_processlist.h"
#include "sql/sql_show_status.h"  // build_show_session_status, ...
#include "sql/sql_update.h"       // Sql_cmd_update...
#include "sql/system_variables.h"
#include "sql/table_function.h"
#include "sql/thr_malloc.h"
#include "sql/trigger_def.h"
#include "sql/window.h"  // Window
#include "sql_string.h"
#include "template_utils.h"

extern bool pfs_processlist_enabled;

namespace {

template <typename Context, typename Node>
bool contextualize_safe(Context *pc, Node node) {
  if (node == nullptr) return false;
  return node->contextualize(pc);
}

template <typename Context>
bool contextualize_safe(Context *pc, mem_root_deque<Item *> *list) {
  if (list == nullptr) return false;
  for (Item *&item : *list) {
    if (item->itemize(pc, &item)) return true;
  }
  return false;
}

/**
  Convenience function that calls Parse_tree_node::contextualize() on each of
  the nodes that are non-NULL, stopping when a call returns true.
*/
template <typename Context, typename Node, typename... Nodes>
bool contextualize_safe(Context *pc, Node node, Nodes... nodes) {
  return contextualize_safe(pc, node) || contextualize_safe(pc, nodes...);
}

/**
  Convenience function that calls Item::itemize() on the item if it's
  non-NULL.
*/
bool itemize_safe(Parse_context *pc, Item **item) {
  if (*item == nullptr) return false;
  return (*item)->itemize(pc, item);
}

}  // namespace

Table_ddl_parse_context::Table_ddl_parse_context(THD *thd_arg,
                                                 Query_block *select_arg,
                                                 Alter_info *alter_info)
    : Parse_context(thd_arg, select_arg),
      create_info(thd_arg->lex->create_info),
      alter_info(alter_info),
      key_create_info(&thd_arg->lex->key_create_info) {}

PT_joined_table *PT_table_reference::add_cross_join(PT_cross_join *cj) {
  cj->add_rhs(this);
  return cj;
}

bool PT_joined_table::contextualize_tabs(Parse_context *pc) {
  if (tr1 != nullptr) return false;  // already done

  bool was_right = m_type & JTT_RIGHT;
  if (was_right)  // rewrite to LEFT
  {
    m_type =
        static_cast<PT_joined_table_type>((m_type & ~JTT_RIGHT) | JTT_LEFT);
    std::swap(tab1_node, tab2_node);
  }

  if (tab1_node->contextualize(pc) || tab2_node->contextualize(pc)) return true;

  tr1 = tab1_node->value;
  tr2 = tab2_node->value;

  if (tr1 == nullptr || tr2 == nullptr) {
    error(pc, join_pos);
    return true;
  }

  if (m_type & JTT_LEFT) {
    tr2->outer_join = true;
    if (was_right) {
      tr2->join_order_swapped = true;
      tr2->query_block->set_right_joins();
    }
  }

  return false;
}

bool PT_option_value_no_option_type_charset::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  int flags = opt_charset ? 0 : set_var_collation_client::SET_CS_DEFAULT;
  const CHARSET_INFO *cs2;
  cs2 =
      opt_charset ? opt_charset : global_system_variables.character_set_client;
  set_var_collation_client *var;
  var = new (thd->mem_root) set_var_collation_client(
      flags, cs2, thd->variables.collation_database, cs2);
  if (var == nullptr) return true;
  lex->var_list.push_back(var);
  return false;
}

bool PT_option_value_no_option_type_names::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();
  LEX_CSTRING names = {STRING_WITH_LEN("names")};

  if (pctx && pctx->find_variable(names.str, names.length, false))
    my_error(ER_SP_BAD_VAR_SHADOW, MYF(0), names.str);
  else
    error(pc, pos);

  return true;  // alwais fails with an error
}

bool PT_set_names::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  const CHARSET_INFO *cs2;
  const CHARSET_INFO *cs3;
  int flags = set_var_collation_client::SET_CS_NAMES |
              (opt_charset ? 0 : set_var_collation_client::SET_CS_DEFAULT) |
              (opt_collation ? set_var_collation_client::SET_CS_COLLATE : 0);
  cs2 =
      opt_charset ? opt_charset : global_system_variables.character_set_client;
  if (opt_collation != nullptr) {
    if (!my_charset_same(cs2, opt_collation)) {
      my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0), opt_collation->name,
               cs2->csname);
      return true;
    }
    cs3 = opt_collation;
  } else {
    if (cs2 == &my_charset_utf8mb4_0900_ai_ci &&
        cs2 != thd->variables.default_collation_for_utf8mb4)
      cs3 = thd->variables.default_collation_for_utf8mb4;
    else
      cs3 = cs2;
  }
  set_var_collation_client *var;
  var = new (thd->mem_root) set_var_collation_client(flags, cs3, cs3, cs3);
  if (var == nullptr) return true;
  lex->var_list.push_back(var);
  return false;
}

bool PT_group::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  Query_block *select = pc->select;
  select->parsing_place = CTX_GROUP_BY;

  if (group_list->contextualize(pc)) return true;
  assert(select == pc->select);

  select->group_list = group_list->value;

  // group by does not have to provide ordering
  ORDER *group = select->group_list.first;
  for (; group; group = group->next) group->direction = ORDER_NOT_RELEVANT;

  // Ensure we're resetting parsing place of the right select
  assert(select->parsing_place == CTX_GROUP_BY);
  select->parsing_place = CTX_NONE;

  switch (olap) {
    case UNSPECIFIED_OLAP_TYPE:
      break;
    case ROLLUP_TYPE:
      if (select->linkage == GLOBAL_OPTIONS_TYPE) {
        my_error(ER_WRONG_USAGE, MYF(0), "WITH ROLLUP",
                 "global union parameters");
        return true;
      }
      select->olap = ROLLUP_TYPE;
      break;
    default:
      assert(!"unexpected OLAP type!");
  }
  return false;
}

bool PT_order::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->select->parsing_place = CTX_ORDER_BY;
  pc->thd->where = "global ORDER clause";

  if (order_list->contextualize(pc)) return true;
  pc->select->order_list = order_list->value;

  // Reset parsing place only for ORDER BY
  if (pc->select->parsing_place == CTX_ORDER_BY)
    pc->select->parsing_place = CTX_NONE;

  pc->thd->where = THD::DEFAULT_WHERE;
  return false;
}

bool PT_order_expr::contextualize(Parse_context *pc) {
  return super::contextualize(pc) || item_initial->itemize(pc, &item_initial);
}

bool PT_internal_variable_name_1d::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();
  sp_variable *spv;

  value.var = nullptr;
  value.base_name = ident;

  /* Best effort lookup for system variable. */
  if (!pctx || !(spv = pctx->find_variable(ident.str, ident.length, false))) {
    /* Not an SP local variable */
    if (find_sys_var_null_base(thd, &value)) return true;
  } else {
    /*
      Possibly an SP local variable (or a shadowed sysvar).
      Will depend on the context of the SET statement.
    */
  }
  return false;
}

bool PT_internal_variable_name_2d::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_head *sp = lex->sphead;

  if (check_reserved_words(ident1.str)) {
    error(pc, pos);
    return true;
  }

  if (sp && sp->m_type == enum_sp_type::TRIGGER &&
      (!my_strcasecmp(system_charset_info, ident1.str, "NEW") ||
       !my_strcasecmp(system_charset_info, ident1.str, "OLD"))) {
    if (ident1.str[0] == 'O' || ident1.str[0] == 'o') {
      my_error(ER_TRG_CANT_CHANGE_ROW, MYF(0), "OLD", "");
      return true;
    }
    if (sp->m_trg_chistics.event == TRG_EVENT_DELETE) {
      my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "NEW", "on DELETE");
      return true;
    }
    if (sp->m_trg_chistics.action_time == TRG_ACTION_AFTER) {
      my_error(ER_TRG_CANT_CHANGE_ROW, MYF(0), "NEW", "after ");
      return true;
    }
    /* This special combination will denote field of NEW row */
    value.var = trg_new_row_fake_var;
    value.base_name = ident2;
  } else {
    LEX_CSTRING *domain;
    LEX_CSTRING *variable;
    bool is_key_cache_variable = false;
    sys_var *tmp;
    if (ident2.str && is_key_cache_variable_suffix(ident2.str)) {
      is_key_cache_variable = true;
      domain = &ident2;
      variable = &ident1;
      tmp = find_sys_var(thd, domain->str, domain->length);
    } else {
      domain = &ident1;
      variable = &ident2;
      /*
        We are getting the component name as domain->str and variable name
        as variable->str, and we are adding the "." as a separator to find
        the variable from systam_variable_hash.
        We are doing this, because we use the structured variable syntax for
        component variables.
      */
      String tmp_name;
      if (tmp_name.reserve(domain->length + 1 + variable->length + 1) ||
          tmp_name.append(domain->str) || tmp_name.append(".") ||
          tmp_name.append(variable->str))
        return true;  // OOM
      tmp = find_sys_var(thd, tmp_name.c_ptr(), tmp_name.length());
    }
    if (!tmp) return true;

    if (is_key_cache_variable && !tmp->is_struct())
      my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), domain->str);

    value.var = tmp;
    if (is_key_cache_variable)
      value.base_name = *variable;
    else
      value.base_name = NULL_CSTR;
  }
  return false;
}

bool PT_option_value_no_option_type_internal::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || name->contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_head *sp = lex->sphead;

  if (opt_expr != nullptr && opt_expr->itemize(pc, &opt_expr)) return true;

  const char *expr_start_ptr = nullptr;

  if (sp) expr_start_ptr = expr_pos.raw.start;

  if (name->value.var == trg_new_row_fake_var) {
    assert(sp);
    assert(expr_start_ptr);

    /* We are parsing trigger and this is a trigger NEW-field. */

    LEX_CSTRING expr_query = EMPTY_CSTR;

    if (!opt_expr) {
      // This is: SET NEW.x = DEFAULT
      // DEFAULT clause is not supported in triggers.

      error(pc, expr_pos);
      return true;
    } else if (lex->is_metadata_used()) {
      expr_query = make_string(thd, expr_start_ptr, expr_pos.raw.end);

      if (!expr_query.str) return true;
    }

    if (set_trigger_new_row(pc, name->value.base_name, opt_expr, expr_query))
      return true;
  } else if (name->value.var) {
    /* We're not parsing SP and this is a system variable. */

    if (set_system_variable(thd, &name->value, lex->option_type, opt_expr))
      return true;
  } else {
    assert(sp);
    assert(expr_start_ptr);

    /* We're parsing SP and this is an SP-variable. */

    sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();
    sp_variable *spv = pctx->find_variable(name->value.base_name.str,
                                           name->value.base_name.length, false);

    LEX_CSTRING expr_query = EMPTY_CSTR;

    if (!opt_expr) {
      /*
        This is: SET x = DEFAULT, where x is a SP-variable.
        This is not supported.
      */

      error(pc, expr_pos);
      return true;
    } else if (lex->is_metadata_used()) {
      expr_query = make_string(thd, expr_start_ptr, expr_pos.raw.end);

      if (!expr_query.str) return true;
    }

    /*
      NOTE: every SET-expression has its own LEX-object, even if it is
      a multiple SET-statement, like:

        SET spv1 = expr1, spv2 = expr2, ...

      Every SET-expression has its own sp_instr_set. Thus, the
      instruction owns the LEX-object, i.e. the instruction is
      responsible for destruction of the LEX-object.
    */

    sp_instr_set *i = new (thd->mem_root)
        sp_instr_set(sp->instructions(), lex, spv->offset, opt_expr, expr_query,
                     true);  // The instruction owns its lex.

    if (!i || sp->add_instr(thd, i)) return true;
  }
  return false;
}

bool PT_option_value_no_option_type_password_for::contextualize(
    Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  set_var_password *var;
  lex->contains_plaintext_password = true;

  /*
    In case of anonymous user, user->user is set to empty string with
    length 0. But there might be case when user->user.str could be NULL.
    For Ex: "set password for current_user() = password('xyz');".
    In this case, set user information as of the current user.
  */
  if (!user->user.str) {
    LEX_CSTRING sctx_priv_user = thd->security_context()->priv_user();
    assert(sctx_priv_user.str);
    user->user.str = sctx_priv_user.str;
    user->user.length = sctx_priv_user.length;
  }
  if (!user->host.str) {
    LEX_CSTRING sctx_priv_host = thd->security_context()->priv_host();
    assert(sctx_priv_host.str);
    user->host.str = sctx_priv_host.str;
    user->host.length = sctx_priv_host.length;
  }

  // Current password is specified through the REPLACE clause hence set the flag
  if (current_password != nullptr) user->uses_replace_clause = true;

  if (random_password_generator) password = nullptr;

  var = new (thd->mem_root) set_var_password(
      user, const_cast<char *>(password), const_cast<char *>(current_password),
      retain_current_password, random_password_generator);

  if (var == nullptr || lex->var_list.push_back(var)) {
    return true;  // Out of memory
  }
  lex->sql_command = SQLCOM_SET_PASSWORD;
  if (lex->sphead) lex->sphead->m_flags |= sp_head::HAS_SET_AUTOCOMMIT_STMT;
  if (sp_create_assignment_instr(pc->thd, expr_pos.raw.end)) return true;
  return false;
}

bool PT_option_value_no_option_type_password::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  sp_head *sp = lex->sphead;
  sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();
  LEX_CSTRING pw = {STRING_WITH_LEN("password")};
  lex->contains_plaintext_password = true;

  if (pctx && pctx->find_variable(pw.str, pw.length, false)) {
    my_error(ER_SP_BAD_VAR_SHADOW, MYF(0), pw.str);
    return true;
  }

  LEX_CSTRING sctx_user = thd->security_context()->user();
  LEX_CSTRING sctx_priv_host = thd->security_context()->priv_host();
  assert(sctx_priv_host.str);

  LEX_USER *user = LEX_USER::alloc(thd, (LEX_STRING *)&sctx_user,
                                   (LEX_STRING *)&sctx_priv_host);
  if (!user) return true;

  if (random_password_generator) password = nullptr;

  set_var_password *var = new (thd->mem_root) set_var_password(
      user, const_cast<char *>(password), const_cast<char *>(current_password),
      retain_current_password, random_password_generator);

  if (var == nullptr || lex->var_list.push_back(var)) {
    return true;  // Out of Memory
  }
  lex->sql_command = SQLCOM_SET_PASSWORD;

  if (sp) sp->m_flags |= sp_head::HAS_SET_AUTOCOMMIT_STMT;

  if (sp_create_assignment_instr(pc->thd, expr_pos.raw.end)) return true;

  return false;
}

PT_key_part_specification::PT_key_part_specification(Item *expression,
                                                     enum_order order)
    : m_expression(expression), m_order(order) {}

PT_key_part_specification::PT_key_part_specification(
    const LEX_CSTRING &column_name, enum_order order, int prefix_length)
    : m_expression(nullptr),
      m_order(order),
      m_column_name(column_name),
      m_prefix_length(prefix_length) {}

bool PT_key_part_specification::contextualize(Parse_context *pc) {
  return super::contextualize(pc) || itemize_safe(pc, &m_expression);
}

bool PT_select_sp_var::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *lex = pc->thd->lex;
#ifndef NDEBUG
  sp = lex->sphead;
#endif
  sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();
  sp_variable *spv;

  if (!pctx || !(spv = pctx->find_variable(name.str, name.length, false))) {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), name.str);
    return true;
  }

  offset = spv->offset;

  return false;
}

Sql_cmd *PT_select_stmt::make_cmd(THD *thd) {
  Parse_context pc(thd, thd->lex->current_query_block());

  thd->lex->sql_command = m_sql_command;

  if (m_qe->contextualize(&pc)) {
    return nullptr;
  }

  const bool has_into_clause_inside_query_block = thd->lex->result != nullptr;

  if (has_into_clause_inside_query_block && m_into != nullptr) {
    my_error(ER_MULTIPLE_INTO_CLAUSES, MYF(0));
    return nullptr;
  }
  if (contextualize_safe(&pc, m_into)) {
    return nullptr;
  }

  if (m_into != nullptr && m_has_trailing_locking_clauses) {
    // Example: ... INTO ... FOR UPDATE;
    push_warning(thd, ER_WARN_DEPRECATED_INNER_INTO);
  } else if (has_into_clause_inside_query_block && thd->lex->unit->is_union()) {
    // Example: ... UNION ... INTO ...;
    if (!m_qe->has_trailing_into_clause()) {
      // Example: ... UNION SELECT * INTO OUTFILE 'foo' FROM ...;
      push_warning(thd, ER_WARN_DEPRECATED_INNER_INTO);
    } else if (m_has_trailing_locking_clauses) {
      // Example: ... UNION SELECT ... FROM ... INTO OUTFILE 'foo' FOR UPDATE;
      push_warning(thd, ER_WARN_DEPRECATED_INNER_INTO);
    }
  }

  if (thd->lex->sql_command == SQLCOM_SELECT)
    return new (thd->mem_root) Sql_cmd_select(thd->lex->result);
  else  // (thd->lex->sql_command == SQLCOM_DO)
    return new (thd->mem_root) Sql_cmd_do(nullptr);
}

/*
  Given a table in the source list, find a correspondent table in the
  list of table references.

  @param tbl    Source table to match.
  @param tables Table references list.

  @remark The source table list (tables listed before the FROM clause
  or tables listed in the FROM clause before the USING clause) may
  contain table names or aliases that must match unambiguously one,
  and only one, table in the target table list (table references list,
  after FROM/USING clause).

  @return Matching table, NULL if error.
*/

static TABLE_LIST *multi_delete_table_match(TABLE_LIST *tbl,
                                            TABLE_LIST *tables) {
  TABLE_LIST *match = nullptr;
  DBUG_TRACE;

  for (TABLE_LIST *elem = tables; elem; elem = elem->next_local) {
    int cmp;

    if (tbl->is_fqtn && elem->is_alias) continue; /* no match */
    if (tbl->is_fqtn && elem->is_fqtn)
      cmp = my_strcasecmp(table_alias_charset, tbl->table_name,
                          elem->table_name) ||
            strcmp(tbl->db, elem->db);
    else if (elem->is_alias)
      cmp = my_strcasecmp(table_alias_charset, tbl->alias, elem->alias);
    else
      cmp = my_strcasecmp(table_alias_charset, tbl->table_name,
                          elem->table_name) ||
            strcmp(tbl->db, elem->db);

    if (cmp) continue;

    if (match) {
      my_error(ER_NONUNIQ_TABLE, MYF(0), elem->alias);
      return nullptr;
    }

    match = elem;
  }

  if (!match)
    my_error(ER_UNKNOWN_TABLE, MYF(0), tbl->table_name, "MULTI DELETE");

  return match;
}

/**
  Link tables in auxiliary table list of multi-delete with corresponding
  elements in main table list, and set proper locks for them.

  @param pc   Parse context
  @param delete_tables  List of tables to delete from

  @returns false if success, true if error
*/

static bool multi_delete_link_tables(Parse_context *pc,
                                     SQL_I_List<TABLE_LIST> *delete_tables) {
  DBUG_TRACE;

  TABLE_LIST *tables = pc->select->table_list.first;

  for (TABLE_LIST *target_tbl = delete_tables->first; target_tbl;
       target_tbl = target_tbl->next_local) {
    /* All tables in aux_tables must be found in FROM PART */
    TABLE_LIST *walk = multi_delete_table_match(target_tbl, tables);
    if (!walk) return true;
    if (!walk->is_derived()) {
      target_tbl->table_name = walk->table_name;
      target_tbl->table_name_length = walk->table_name_length;
    }
    walk->updating = target_tbl->updating;
    walk->set_lock(target_tbl->lock_descriptor());
    /* We can assume that tables to be deleted from are locked for write. */
    assert(walk->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE);
    walk->mdl_request.set_type(mdl_type_for_dml(walk->lock_descriptor().type));
    target_tbl->correspondent_table = walk;  // Remember corresponding table
  }
  return false;
}

bool PT_delete::add_table(Parse_context *pc, Table_ident *table) {
  const ulong table_opts = is_multitable()
                               ? TL_OPTION_UPDATING | TL_OPTION_ALIAS
                               : TL_OPTION_UPDATING;
  const thr_lock_type lock_type = (opt_delete_options & DELETE_LOW_PRIORITY)
                                      ? TL_WRITE_LOW_PRIORITY
                                      : TL_WRITE_DEFAULT;
  const enum_mdl_type mdl_type = (opt_delete_options & DELETE_LOW_PRIORITY)
                                     ? MDL_SHARED_WRITE_LOW_PRIO
                                     : MDL_SHARED_WRITE;
  return !pc->select->add_table_to_list(
      pc->thd, table, opt_table_alias, table_opts, lock_type, mdl_type, nullptr,
      opt_use_partition, nullptr, pc);
}

Sql_cmd *PT_delete::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  Parse_context pc(thd, select);

  assert(lex->query_block == select);
  lex->sql_command = is_multitable() ? SQLCOM_DELETE_MULTI : SQLCOM_DELETE;
  lex->set_ignore(opt_delete_options & DELETE_IGNORE);
  select->init_order();
  if (opt_delete_options & DELETE_QUICK) select->add_base_options(OPTION_QUICK);

  if (contextualize_safe(&pc, m_with_clause))
    return nullptr; /* purecov: inspected */

  if (is_multitable()) {
    for (Table_ident **i = table_list.begin(); i != table_list.end(); ++i) {
      if (add_table(&pc, *i)) return nullptr;
    }
  } else if (add_table(&pc, table_ident))
    return nullptr;

  if (is_multitable()) {
    select->table_list.save_and_clear(&delete_tables);
    lex->query_tables = nullptr;
    lex->query_tables_last = &lex->query_tables;
  } else {
    select->top_join_list.push_back(select->get_table_list());
  }
  Yacc_state *const yyps = &pc.thd->m_parser_state->m_yacc;
  yyps->m_lock_type = TL_READ_DEFAULT;
  yyps->m_mdl_type = MDL_SHARED_READ;

  if (is_multitable()) {
    if (contextualize_array(&pc, &join_table_list)) return nullptr;
    pc.select->context.table_list =
        pc.select->context.first_name_resolution_table =
            pc.select->table_list.first;
  }

  if (opt_where_clause != nullptr &&
      opt_where_clause->itemize(&pc, &opt_where_clause))
    return nullptr;
  select->set_where_cond(opt_where_clause);

  if (opt_order_clause != nullptr && opt_order_clause->contextualize(&pc))
    return nullptr;

  assert(select->select_limit == nullptr);
  if (opt_delete_limit_clause != nullptr) {
    if (opt_delete_limit_clause->itemize(&pc, &opt_delete_limit_clause))
      return nullptr;
    select->select_limit = opt_delete_limit_clause;
    if (select->select_limit->fixed && select->select_limit->val_int() != 0)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
  }

  if (is_multitable() && multi_delete_link_tables(&pc, &delete_tables))
    return nullptr;

  if (opt_hints != nullptr && opt_hints->contextualize(&pc)) return nullptr;

  return new (thd->mem_root) Sql_cmd_delete(is_multitable(), &delete_tables);
}

Sql_cmd *PT_update::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  Parse_context pc(thd, select);

  lex->duplicates = DUP_ERROR;

  lex->set_ignore(opt_ignore);

  if (contextualize_safe(&pc, m_with_clause))
    return nullptr; /* purecov: inspected */

  if (contextualize_array(&pc, &join_table_list)) return nullptr;
  select->parsing_place = CTX_UPDATE_VALUE;

  if (column_list->contextualize(&pc) || value_list->contextualize(&pc)) {
    return nullptr;
  }
  select->fields = column_list->value;

  // Ensure we're resetting parsing context of the right select
  assert(select->parsing_place == CTX_UPDATE_VALUE);
  select->parsing_place = CTX_NONE;
  const bool is_multitable = select->table_list.elements > 1;
  lex->sql_command = is_multitable ? SQLCOM_UPDATE_MULTI : SQLCOM_UPDATE;

  /*
    In case of multi-update setting write lock for all tables may
    be too pessimistic. We will decrease lock level if possible in
    Sql_cmd_update::prepare_inner().
  */
  select->set_lock_for_tables(opt_low_priority);

  if (opt_where_clause != nullptr &&
      opt_where_clause->itemize(&pc, &opt_where_clause)) {
    return nullptr;
  }
  select->set_where_cond(opt_where_clause);

  if (opt_order_clause != nullptr && opt_order_clause->contextualize(&pc))
    return nullptr;

  assert(select->select_limit == nullptr);
  if (opt_limit_clause != nullptr) {
    if (opt_limit_clause->itemize(&pc, &opt_limit_clause)) return nullptr;
    select->select_limit = opt_limit_clause;
    lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
  }

  if (opt_hints != nullptr && opt_hints->contextualize(&pc)) return nullptr;

  return new (thd->mem_root) Sql_cmd_update(is_multitable, &value_list->value);
}

bool PT_insert_values_list::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;
  for (List_item *item_list : many_values) {
    for (auto it = item_list->begin(); it != item_list->end(); ++it) {
      if ((*it)->itemize(pc, &*it)) return true;
    }
  }

  return false;
}

Sql_cmd *PT_insert::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;

  Parse_context pc(thd, lex->current_query_block());

  // Currently there are two syntaxes (old and new, respectively) for INSERT
  // .. VALUES statements:
  //
  //  - INSERT .. VALUES (), () ..
  //  - INSERT .. VALUES ROW(), ROW() ..
  //
  // The latter is a table value constructor, i.e. it has an subquery
  // expression, while the former is the standard VALUES syntax. When the
  // non-standard VALUES() function (primarily used in ON DUPLICATE KEY UPDATE
  // update expressions) is deprecated in the future, the old syntax can be used
  // as a table value constructor as well.
  //
  // However, until such a change is made, we convert INSERT statements with
  // table value constructors into PT_insert objects that are equal to the old
  // syntax, as to enforce consistency by making sure they both follow the same
  // execution path.
  //
  // Note that this removes the constness of both row_value_list and
  // insert_query_expression, which should both be restored when deprecating
  // VALUES as mentioned above.
  if (has_query_block() &&
      insert_query_expression->is_table_value_constructor()) {
    row_value_list = insert_query_expression->get_row_value_list();
    assert(row_value_list != nullptr);

    insert_query_expression = nullptr;
  }

  if (is_replace) {
    lex->sql_command =
        has_query_block() ? SQLCOM_REPLACE_SELECT : SQLCOM_REPLACE;
    lex->duplicates = DUP_REPLACE;
  } else {
    lex->sql_command = has_query_block() ? SQLCOM_INSERT_SELECT : SQLCOM_INSERT;
    lex->duplicates = DUP_ERROR;
    lex->set_ignore(ignore);
  }

  Yacc_state *yyps = &pc.thd->m_parser_state->m_yacc;
  if (!pc.select->add_table_to_list(
          thd, table_ident, nullptr, TL_OPTION_UPDATING, yyps->m_lock_type,
          yyps->m_mdl_type, nullptr, opt_use_partition)) {
    return nullptr;
  }
  pc.select->set_lock_for_tables(lock_option);

  assert(lex->current_query_block() == lex->query_block);

  if (column_list->contextualize(&pc)) return nullptr;

  if (has_query_block()) {
    /*
      In INSERT/REPLACE INTO t ... SELECT the table_list initially contains
      here a table entry for the destination table `t'.
      Backup it and clean the table list for the processing of
      the query expression and push `t' back to the beginning of the
      table_list finally.

      @todo: Don't save the INSERT/REPLACE destination table in
             Query_block::table_list and remove this backup & restore.

      The following work only with the local list, the global list
      is created correctly in this case
    */
    SQL_I_List<TABLE_LIST> save_list;
    Query_block *const save_query_block = pc.select;
    save_query_block->table_list.save_and_clear(&save_list);

    if (insert_query_expression->contextualize(&pc)) return nullptr;

    /*
      The following work only with the local list, the global list
      is created correctly in this case
    */
    save_query_block->table_list.push_front(&save_list);

    lex->bulk_insert_row_cnt = 0;
  } else {
    pc.select->parsing_place = CTX_INSERT_VALUES;
    if (row_value_list->contextualize(&pc)) return nullptr;
    // Ensure we're resetting parsing context of the right select
    assert(pc.select->parsing_place == CTX_INSERT_VALUES);
    pc.select->parsing_place = CTX_NONE;

    lex->bulk_insert_row_cnt = row_value_list->get_many_values().size();
  }

  // Create a derived table to use as a table reference to the VALUES rows,
  // which can be referred to from ON DUPLICATE KEY UPDATE. Naming the derived
  // table columns is deferred to Sql_cmd_insert_base::prepare_inner, as this
  // requires the insert table to be resolved.
  TABLE_LIST *values_table{nullptr};
  if (opt_values_table_alias != nullptr && opt_values_column_list != nullptr) {
    if (!strcmp(opt_values_table_alias, table_ident->table.str)) {
      my_error(ER_NONUNIQ_TABLE, MYF(0), opt_values_table_alias);
      return nullptr;
    }

    Table_ident *ti = new (pc.thd->mem_root)
        Table_ident(lex->query_block->master_query_expression());
    if (ti == nullptr) return nullptr;

    values_table = pc.select->add_table_to_list(
        pc.thd, ti, opt_values_table_alias, 0, TL_READ, MDL_SHARED_READ);
    if (values_table == nullptr) return nullptr;
  }

  if (opt_on_duplicate_column_list != nullptr) {
    assert(!is_replace);
    assert(opt_on_duplicate_value_list != nullptr &&
           opt_on_duplicate_value_list->elements() ==
               opt_on_duplicate_column_list->elements());

    lex->duplicates = DUP_UPDATE;
    TABLE_LIST *first_table = lex->query_block->table_list.first;
    /* Fix lock for ON DUPLICATE KEY UPDATE */
    if (first_table->lock_descriptor().type == TL_WRITE_CONCURRENT_DEFAULT)
      first_table->set_lock({TL_WRITE_DEFAULT, THR_DEFAULT});

    pc.select->parsing_place = CTX_INSERT_UPDATE;

    if (opt_on_duplicate_column_list->contextualize(&pc) ||
        opt_on_duplicate_value_list->contextualize(&pc))
      return nullptr;

    // Ensure we're resetting parsing context of the right select
    assert(pc.select->parsing_place == CTX_INSERT_UPDATE);
    pc.select->parsing_place = CTX_NONE;
  }

  if (opt_hints != nullptr && opt_hints->contextualize(&pc)) return nullptr;

  Sql_cmd_insert_base *sql_cmd;
  if (has_query_block())
    sql_cmd =
        new (thd->mem_root) Sql_cmd_insert_select(is_replace, lex->duplicates);
  else
    sql_cmd =
        new (thd->mem_root) Sql_cmd_insert_values(is_replace, lex->duplicates);
  if (sql_cmd == nullptr) return nullptr;

  if (!has_query_block()) {
    sql_cmd->insert_many_values = row_value_list->get_many_values();
    sql_cmd->values_table = values_table;
    sql_cmd->values_column_list = opt_values_column_list;
  }

  sql_cmd->insert_field_list = column_list->value;
  if (opt_on_duplicate_column_list != nullptr) {
    assert(!is_replace);
    sql_cmd->update_field_list = opt_on_duplicate_column_list->value;
    sql_cmd->update_value_list = opt_on_duplicate_value_list->value;
  }

  return sql_cmd;
}

Sql_cmd *PT_call::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;

  Parse_context pc(thd, lex->current_query_block());

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  if (opt_expr_list != nullptr && opt_expr_list->contextualize(&pc))
    return nullptr; /* purecov: inspected */

  lex->sql_command = SQLCOM_CALL;

  sp_add_own_used_routine(lex, thd, Sroutine_hash_entry::PROCEDURE, proc_name);

  mem_root_deque<Item *> *proc_args = nullptr;
  if (opt_expr_list != nullptr) proc_args = &opt_expr_list->value;

  return new (thd->mem_root) Sql_cmd_call(proc_name, proc_args);
}

bool PT_query_specification::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->select->parsing_place = CTX_SELECT_LIST;

  if (options.query_spec_options & SELECT_HIGH_PRIORITY) {
    Yacc_state *yyps = &pc->thd->m_parser_state->m_yacc;
    yyps->m_lock_type = TL_READ_HIGH_PRIORITY;
    yyps->m_mdl_type = MDL_SHARED_READ;
  }
  if (options.save_to(pc)) return true;

  if (item_list->contextualize(pc)) return true;

  // Ensure we're resetting parsing place of the right select
  assert(pc->select->parsing_place == CTX_SELECT_LIST);
  pc->select->parsing_place = CTX_NONE;

  if (contextualize_safe(pc, opt_into1)) return true;

  if (!from_clause.empty()) {
    if (contextualize_array(pc, &from_clause)) return true;
    pc->select->context.table_list =
        pc->select->context.first_name_resolution_table =
            pc->select->table_list.first;
  }

  if (itemize_safe(pc, &opt_where_clause) ||
      contextualize_safe(pc, opt_group_clause) ||
      itemize_safe(pc, &opt_having_clause))
    return true;

  pc->select->set_where_cond(opt_where_clause);
  pc->select->set_having_cond(opt_having_clause);

  /*
    Window clause is resolved under CTX_SELECT_LIST and not
    under CTX_WINDOW. Reasons being:
    1. Window functions are part of select list and the
    resolution of window definition happens along with
    window functions.
    2. It is tricky to resolve window definition under CTX_WINDOW
    and window functions under CTX_SELECT_LIST.
    3. Unnamed window definitions are anyways naturally placed in
    select list.
    4. Named window definition are not placed in select list of
    the query. But if this window definition is
    used by any window functions, then we resolve under CTX_SELECT_LIST.
    5. Because of all of the above, unused window definitions are
    resolved under CTX_SELECT_LIST. (These unused window definitions
    are removed after syntactic and semantic checks are done).
  */

  pc->select->parsing_place = CTX_SELECT_LIST;
  if (contextualize_safe(pc, opt_window_clause)) return true;
  pc->select->parsing_place = CTX_NONE;

  return (opt_hints != nullptr ? opt_hints->contextualize(pc) : false);
}

bool PT_table_value_constructor::contextualize(Parse_context *pc) {
  if (row_value_list->contextualize(pc)) return true;

  pc->select->is_table_value_constructor = true;
  pc->select->row_value_list = &row_value_list->get_many_values();

  // Some queries, such as CREATE TABLE with SELECT, require item_list to
  // contain items to call Query_block::prepare.
  for (Item *item : *pc->select->row_value_list->front()) {
    pc->select->fields.push_back(item);
  }

  return false;
}

bool PT_query_expression::contextualize_order_and_limit(Parse_context *pc) {
  /*
    Quick reject test. We don't need to do anything if there are no limit
    or order by clauses.
  */
  if (m_order == nullptr && m_limit == nullptr) return false;

  if (m_body->can_absorb_order_and_limit(m_order != nullptr,
                                         m_limit != nullptr)) {
    if (contextualize_safe(pc, m_order, m_limit)) return true;
  } else {
    auto lex = pc->thd->lex;
    auto unit = pc->select->master_query_expression();
    if (unit->fake_query_block == nullptr) {
      if (unit->add_fake_query_block(lex->thd)) {
        return true;  // OOM
      }
    } else if (unit->fake_query_block->has_limit() ||
               unit->fake_query_block->is_ordered()) {
      /*
        Make sure that we don't silently overwrite intermediate ORDER BY
        and/or LIMIT clauses, but reject unsupported levels of nesting
        instead.

        We are here since we support syntax like this:

          (SELECT ... ORDER BY ... LIMIT) ORDER BY ... LIMIT ...

        where the second pair of ORDER BY and LIMIT goes to "global parameters"
        A.K.A. fake_query_block. I.e. this syntax works like a degenerate case
        of unions: a union of one query block with no trailing clauses.

        Such an implementation is unable to process more than one external
        level of ORDER BY/LIMIT like this:

          ( (SELECT ...
              ORDER BY ... LIMIT)
            ORDER BY ... LIMIT ...)
          ORDER BY ... LIMIT ...

        TODO: Don't use fake_query_block code (that is designed for unions)
              for parenthesized query blocks. Reimplement this syntax with
              e.g. equivalent derived tables to support any level of nesting.
      */
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "parenthesized query expression with more than one external "
               "level of ORDER/LIMIT operations");
      return true;
    }

    auto orig_query_block = pc->select;
    pc->select = unit->fake_query_block;
    lex->push_context(&pc->select->context);
    assert(pc->select->parsing_place == CTX_NONE);

    bool res = contextualize_safe(pc, m_order, m_limit);

    lex->pop_context();
    pc->select = orig_query_block;

    if (res) return true;
  }
  return false;
}

bool PT_table_factor_function::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || m_expr->itemize(pc, &m_expr)) return true;

  if (m_path->itemize(pc, &m_path)) return true;

  auto nested_columns = new (pc->mem_root) List<Json_table_column>;
  if (nested_columns == nullptr) return true;  // OOM

  for (auto col : *m_nested_columns) {
    if (col->contextualize(pc) || nested_columns->push_back(col->get_column()))
      return true;
  }

  auto root_el = new (pc->mem_root) Json_table_column(m_path, nested_columns);
  auto *root_list = new (pc->mem_root) List<Json_table_column>;
  if (root_el == nullptr || root_list == nullptr ||
      root_list->push_front(root_el))
    return true;  // OOM

  auto jtf = new (pc->mem_root)
      Table_function_json(m_table_alias.str, m_expr, root_list);
  if (jtf == nullptr) return true;  // OOM

  LEX_CSTRING alias;
  alias.length = strlen(jtf->func_name());
  alias.str = sql_strmake(jtf->func_name(), alias.length);
  if (alias.str == nullptr) return true;  // OOM

  auto ti = new (pc->mem_root) Table_ident(alias, jtf);
  if (ti == nullptr) return true;

  value = pc->select->add_table_to_list(pc->thd, ti, m_table_alias.str, 0,
                                        TL_READ, MDL_SHARED_READ);
  if (value == nullptr || pc->select->add_joined_table(value)) return true;

  return false;
}

bool PT_table_sequence_function::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || m_expr->itemize(pc, &m_expr)) return true;

  auto stf = new (pc->mem_root)
      Table_function_sequence(m_table_alias.str, m_expr);
  if (stf == nullptr) return true;  // OOM

  LEX_CSTRING alias;
  alias.length = strlen(stf->func_name());
  alias.str = sql_strmake(stf->func_name(), alias.length);
  if (alias.str == nullptr) return true;  // OOM

  auto ti = new (pc->mem_root) Table_ident(alias, stf);
  if (ti == nullptr) return true;

  value = pc->select->add_table_to_list(pc->thd, ti, m_table_alias.str, 0,
                                        TL_READ, MDL_SHARED_READ);
  if (value == nullptr) return true;
  if (pc->select->add_joined_table(value)) return true;

  return false;
}

PT_derived_table::PT_derived_table(bool lateral, PT_subquery *subquery,
                                   const LEX_CSTRING &table_alias,
                                   Create_col_name_list *column_names)
    : m_lateral(lateral),
      m_subquery(subquery),
      m_table_alias(table_alias.str),
      column_names(*column_names) {
  m_subquery->m_is_derived_table = true;
}

bool PT_derived_table::contextualize(Parse_context *pc) {
  Query_block *outer_query_block = pc->select;

  outer_query_block->parsing_place = CTX_DERIVED;
  assert(outer_query_block->linkage != GLOBAL_OPTIONS_TYPE);

  /*
    Determine the first outer context to try for the derived table:
    - if lateral: context of query which owns the FROM i.e. outer_query_block
    - if not lateral: context of query outer to query which owns the FROM.
    This is just a preliminary decision. Name resolution
    {Item_field,Item_ref}::fix_fields() may use or ignore this outer context
    depending on where the derived table is placed in it.
  */
  if (!m_lateral)
    pc->thd->lex->push_context(
        outer_query_block->master_query_expression()->outer_query_block()
            ? &outer_query_block->master_query_expression()
                   ->outer_query_block()
                   ->context
            : nullptr);

  if (m_subquery->contextualize(pc)) return true;

  if (!m_lateral) pc->thd->lex->pop_context();

  outer_query_block->parsing_place = CTX_NONE;

  assert(pc->select->next_query_block() == nullptr);

  Query_expression *unit = pc->select->first_inner_query_expression();
  pc->select = outer_query_block;
  Table_ident *ti = new (pc->thd->mem_root) Table_ident(unit);
  if (ti == nullptr) return true;

  value = pc->select->add_table_to_list(pc->thd, ti, m_table_alias, 0, TL_READ,
                                        MDL_SHARED_READ);
  if (value == nullptr) return true;
  if (column_names.size()) value->set_derived_column_names(&column_names);
  if (m_lateral) {
    // Mark the unit as LATERAL, by turning on one bit in the map:
    value->derived_query_expression()->m_lateral_deps = OUTER_REF_TABLE_BIT;
  }
  if (pc->select->add_joined_table(value)) return true;

  return false;
}

bool PT_table_factor_joined_table::contextualize(Parse_context *pc) {
  if (Parse_tree_node::contextualize(pc)) return true;

  Query_block *outer_query_block = pc->select;
  if (outer_query_block->init_nested_join(pc->thd)) return true;

  if (m_joined_table->contextualize(pc)) return true;
  value = m_joined_table->value;

  if (outer_query_block->end_nested_join() == nullptr) return true;

  return false;
}

bool PT_union::contextualize(Parse_context *pc) {
  if (PT_query_expression_body::contextualize(pc)) return true;

  if (m_lhs->contextualize(pc)) return true;

  pc->select = pc->thd->lex->new_union_query(pc->select, m_is_distinct);

  if (pc->select == nullptr || m_rhs->contextualize(pc)) return true;

  if (m_rhs->is_union()) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "nesting of unions at the right-hand side");
    return true;
  }

  pc->thd->lex->pop_context();
  return false;
}

static bool setup_index(keytype key_type, const LEX_STRING name,
                        PT_base_index_option *type,
                        List<PT_key_part_specification> *columns,
                        Index_options options, Table_ddl_parse_context *pc) {
  *pc->key_create_info = default_key_create_info;

  if (type != nullptr && type->contextualize(pc)) return true;

  if (contextualize_nodes(options, pc)) return true;

  List_iterator<PT_key_part_specification> li(*columns);

  if ((key_type == KEYTYPE_FULLTEXT || key_type == KEYTYPE_SPATIAL ||
       pc->key_create_info->algorithm == HA_KEY_ALG_HASH)) {
    PT_key_part_specification *kp;
    while ((kp = li++)) {
      if (kp->is_explicit()) {
        my_error(ER_WRONG_USAGE, MYF(0), "spatial/fulltext/hash index",
                 "explicit index order");
        return true;
      }
    }
  }

  List<Key_part_spec> cols;
  for (PT_key_part_specification &kp : *columns) {
    if (kp.contextualize(pc)) return true;

    Key_part_spec *spec;
    if (kp.has_expression()) {
      spec =
          new (pc->mem_root) Key_part_spec(kp.get_expression(), kp.get_order());
    } else {
      spec = new (pc->mem_root) Key_part_spec(
          kp.get_column_name(), kp.get_prefix_length(), kp.get_order());
    }
    if (spec == nullptr || cols.push_back(spec)) {
      return true; /* purecov: deadcode */
    }
  }

  Key_spec *key =
      new (pc->mem_root) Key_spec(pc->mem_root, key_type, to_lex_cstring(name),
                                  pc->key_create_info, false, true, cols);
  if (key == nullptr || pc->alter_info->key_list.push_back(key)) return true;

  return false;
}

Sql_cmd *PT_create_index_stmt::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  Query_block *query_block = lex->current_query_block();

  thd->lex->sql_command = SQLCOM_CREATE_INDEX;

  if (query_block->add_table_to_list(thd, m_table_ident, nullptr,
                                     TL_OPTION_UPDATING, TL_READ_NO_INSERT,
                                     MDL_SHARED_UPGRADABLE) == nullptr)
    return nullptr;

  Table_ddl_parse_context pc(thd, query_block, &m_alter_info);

  m_alter_info.flags = Alter_info::ALTER_ADD_INDEX;

  if (setup_index(m_keytype, m_name, m_type, m_columns, m_options, &pc))
    return nullptr;

  m_alter_info.requested_algorithm = m_algo;
  m_alter_info.requested_lock = m_lock;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_create_index(&m_alter_info);
}

bool PT_inline_index_definition::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  if (setup_index(m_keytype, m_name, m_type, m_columns, m_options, pc))
    return true;

  if (m_keytype == KEYTYPE_PRIMARY && !pc->key_create_info->is_visible)
    my_error(ER_PK_INDEX_CANT_BE_INVISIBLE, MYF(0));

  return false;
}

bool PT_foreign_key_definition::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *const thd = pc->thd;
  LEX *const lex = thd->lex;

  LEX_CSTRING db;
  LEX_CSTRING orig_db;

  if (m_referenced_table->db.str) {
    orig_db = m_referenced_table->db;

    if (check_db_name(orig_db.str, orig_db.length) != Ident_name_check::OK)
      return true;

    if (lower_case_table_names) {
      char *db_str = thd->strmake(orig_db.str, orig_db.length);
      if (db_str == nullptr) return true;  // OOM
      db.length = my_casedn_str(files_charset_info, db_str);
      db.str = db_str;
    } else
      db = orig_db;
  } else {
    /*
      Before 8.0 foreign key metadata was handled by SEs and they
      assumed that parent table belongs to the same database as
      child table unless FQTN was used (and connection's current
      database was ignored). We keep behavior compatible even
      though this is inconsistent with interpretation of non-FQTN
      table names in other contexts.

      If this is ALTER TABLE with RENAME TO <db_name.table_name>
      clause we need to use name of the target database.
    */
    if (pc->alter_info->new_db_name.str) {
      db = orig_db = pc->alter_info->new_db_name;
    } else {
      TABLE_LIST *child_table = lex->query_block->get_table_list();
      db = orig_db = LEX_CSTRING{child_table->db, child_table->db_length};
    }
  }

  Ident_name_check ident_check_status = check_table_name(
      m_referenced_table->table.str, m_referenced_table->table.length);
  if (ident_check_status != Ident_name_check::OK) {
    my_error(ER_WRONG_TABLE_NAME, MYF(0), m_referenced_table->table.str);
    return true;
  }

  LEX_CSTRING table_name;

  if (lower_case_table_names) {
    char *table_name_str = thd->strmake(m_referenced_table->table.str,
                                        m_referenced_table->table.length);
    if (table_name_str == nullptr) return true;  // OOM
    table_name.length = my_casedn_str(files_charset_info, table_name_str);
    table_name.str = table_name_str;
  } else
    table_name = m_referenced_table->table;

  lex->key_create_info = default_key_create_info;

  /*
    If present name from the CONSTRAINT clause is used as name of generated
    supporting index (which is created in cases when there is no explicitly
    created supporting index). Otherwise, the FOREIGN KEY index_name value
    is used. If both are missing name of generated supporting index is
    automatically produced.
  */
  const LEX_CSTRING key_name = to_lex_cstring(
      m_constraint_name.str ? m_constraint_name
                            : m_key_name.str ? m_key_name : NULL_STR);

  if (key_name.str && check_string_char_length(key_name, "", NAME_CHAR_LEN,
                                               system_charset_info, true)) {
    my_error(ER_TOO_LONG_IDENT, MYF(0), key_name.str);
    return true;
  }

  List<Key_part_spec> cols;
  for (PT_key_part_specification &kp : *m_columns) {
    if (kp.contextualize(pc)) return true;

    Key_part_spec *spec = new (pc->mem_root) Key_part_spec(
        kp.get_column_name(), kp.get_prefix_length(), kp.get_order());
    if (spec == nullptr || cols.push_back(spec)) {
      return true; /* purecov: deadcode */
    }
  }

  /*
    We always use value from CONSTRAINT clause as a foreign key name.
    If it is not present we use generated name as a foreign key name
    (i.e. we ignore value from FOREIGN KEY index_name part).

    Validity of m_constraint_name has been already checked by the code
    above that handles supporting index name.
  */
  Key_spec *foreign_key = new (pc->mem_root) Foreign_key_spec(
      pc->mem_root, to_lex_cstring(m_constraint_name), cols, db, orig_db,
      table_name, m_referenced_table->table, m_ref_list, m_fk_delete_opt,
      m_fk_update_opt, m_fk_match_option);
  if (foreign_key == nullptr || pc->alter_info->key_list.push_back(foreign_key))
    return true;
  /* Only used for ALTER TABLE. Ignored otherwise. */
  pc->alter_info->flags |= Alter_info::ADD_FOREIGN_KEY;

  Key_spec *key =
      new (pc->mem_root) Key_spec(thd->mem_root, KEYTYPE_MULTIPLE, key_name,
                                  &default_key_create_info, true, true, cols);
  if (key == nullptr || pc->alter_info->key_list.push_back(key)) return true;

  return false;
}

bool PT_with_list::push_back(PT_common_table_expr *el) {
  const LEX_STRING &n = el->name();
  for (auto previous : m_elements) {
    const LEX_STRING &pn = previous->name();
    if (pn.length == n.length && !memcmp(pn.str, n.str, n.length)) {
      my_error(ER_NONUNIQ_TABLE, MYF(0), n.str);
      return true;
    }
  }
  return m_elements.push_back(el);
}

PT_common_table_expr::PT_common_table_expr(
    const LEX_STRING &name, const LEX_STRING &subq_text, uint subq_text_offs,
    PT_subquery *subq_node, const Create_col_name_list *column_names,
    MEM_ROOT *mem_root)
    : m_name(name),
      m_subq_text(subq_text),
      m_subq_text_offset(subq_text_offs),
      m_subq_node(subq_node),
      m_column_names(*column_names),
      m_postparse(mem_root) {
  if (lower_case_table_names && m_name.length) {
    // Lowercase name, as in Query_block::add_table_to_list()
    m_name.length = my_casedn_str(files_charset_info, m_name.str);
  }
  m_postparse.name = m_name;
}

bool PT_with_clause::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true; /* purecov: inspected */
  // WITH complements a query expression (a unit).
  pc->select->master_query_expression()->m_with_clause = this;
  return false;
}

void PT_with_clause::print(const THD *thd, String *str,
                           enum_query_type query_type) {
  size_t len1 = str->length();
  str->append("with ");
  if (m_recursive) str->append("recursive ");
  size_t len2 = str->length(), len3 = len2;
  for (auto el : m_list->elements()) {
    if (str->length() != len3) {
      str->append(", ");
      len3 = str->length();
    }
    el->print(thd, str, query_type);
  }
  if (str->length() == len2)
    str->length(len1);  // don't print an empty WITH clause
  else
    str->append(" ");
}

void PT_common_table_expr::print(const THD *thd, String *str,
                                 enum_query_type query_type) {
  size_t len = str->length();
  append_identifier(thd, str, m_name.str, m_name.length);
  if (m_column_names.size())
    print_derived_column_names(thd, str, &m_column_names);
  str->append(" as ");

  /*
    Printing the raw text (this->m_subq_text) would lack:
    - expansion of '||' (which can mean CONCAT or OR, depending on
    sql_mode's PIPES_AS_CONCAT (the effect would be that a view containing
    a CTE containing '||' would change behaviour if sql_mode was
    changed between its creation and its usage).
    - quoting of table identifiers
    - expansion of the default db.
    So, we rather locate one resolved query expression for this CTE; for
    it to be intact this query expression must be non-merged. And we print
    it.
    If query expression has been merged everywhere, its Query_expression is
    gone and printing this CTE can be skipped. Note that when we print the
    view's body to the data dictionary, no merging is done.
  */
  bool found = false;
  for (auto *tl : m_postparse.references) {
    if (!tl->is_merged() &&
        // If 2+ references exist, show the one which is shown in EXPLAIN
        tl->query_block_id_for_explain() == tl->query_block_id()) {
      str->append('(');
      tl->derived_query_expression()->print(thd, str, query_type);
      str->append(')');
      found = true;
      break;
    }
  }
  if (!found) str->length(len);  // don't print a useless CTE definition
}

bool PT_create_table_engine_option::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->create_info->used_fields |= HA_CREATE_USED_ENGINE;
  const bool is_temp_table = pc->create_info->options & HA_LEX_CREATE_TMP_TABLE;
  return resolve_engine(pc->thd, engine, is_temp_table, false,
                        &pc->create_info->db_type);
}

bool PT_create_table_secondary_engine_option::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->create_info->used_fields |= HA_CREATE_USED_SECONDARY_ENGINE;
  pc->create_info->secondary_engine = m_secondary_engine;
  return false;
}

bool PT_create_stats_auto_recalc_option::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  switch (value) {
    case Ternary_option::ON:
      pc->create_info->stats_auto_recalc = HA_STATS_AUTO_RECALC_ON;
      break;
    case Ternary_option::OFF:
      pc->create_info->stats_auto_recalc = HA_STATS_AUTO_RECALC_OFF;
      break;
    case Ternary_option::DEFAULT:
      pc->create_info->stats_auto_recalc = HA_STATS_AUTO_RECALC_DEFAULT;
      break;
    default:
      assert(false);
  }
  pc->create_info->used_fields |= HA_CREATE_USED_STATS_AUTO_RECALC;
  return false;
}

bool PT_create_stats_stable_pages::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->create_info->stats_sample_pages = value;
  pc->create_info->used_fields |= HA_CREATE_USED_STATS_SAMPLE_PAGES;
  return false;
}

bool PT_create_union_option::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *const thd = pc->thd;
  LEX *const lex = thd->lex;
  const Yacc_state *yyps = &thd->m_parser_state->m_yacc;

  TABLE_LIST **exclude_merge_engine_tables = lex->query_tables_last;
  SQL_I_List<TABLE_LIST> save_list;
  lex->query_block->table_list.save_and_clear(&save_list);
  if (pc->select->add_tables(thd, tables, TL_OPTION_UPDATING, yyps->m_lock_type,
                             yyps->m_mdl_type))
    return true;
  /*
    Move the union list to the merge_list and exclude its tables
    from the global list.
  */
  pc->create_info->merge_list = lex->query_block->table_list;
  lex->query_block->table_list = save_list;
  /*
    When excluding union list from the global list we assume that
    elements of the former immediately follow elements which represent
    table being created/altered and parent tables.
  */
  assert(*exclude_merge_engine_tables == pc->create_info->merge_list.first);
  *exclude_merge_engine_tables = nullptr;
  lex->query_tables_last = exclude_merge_engine_tables;

  pc->create_info->used_fields |= HA_CREATE_USED_UNION;
  return false;
}

bool set_default_charset(HA_CREATE_INFO *create_info,
                         const CHARSET_INFO *value) {
  assert(value != nullptr);

  if ((create_info->used_fields & HA_CREATE_USED_DEFAULT_CHARSET) &&
      create_info->default_table_charset &&
      !my_charset_same(create_info->default_table_charset, value)) {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0), "CHARACTER SET ",
             create_info->default_table_charset->csname, "CHARACTER SET ",
             value->csname);
    return true;
  }
  create_info->default_table_charset = value;
  create_info->used_fields |= HA_CREATE_USED_DEFAULT_CHARSET;
  return false;
}

bool PT_create_table_default_charset::contextualize(
    Table_ddl_parse_context *pc) {
  return (super::contextualize(pc) ||
          set_default_charset(pc->create_info, value));
}

bool set_default_collation(HA_CREATE_INFO *create_info,
                           const CHARSET_INFO *collation) {
  assert(collation != nullptr);
  assert((create_info->default_table_charset == nullptr) ==
         ((create_info->used_fields & HA_CREATE_USED_DEFAULT_CHARSET) == 0));

  if (merge_charset_and_collation(create_info->default_table_charset, collation,
                                  &create_info->default_table_charset)) {
    return true;
  }
  create_info->used_fields |= HA_CREATE_USED_DEFAULT_CHARSET;
  create_info->used_fields |= HA_CREATE_USED_DEFAULT_COLLATE;
  return false;
}

bool PT_create_table_default_collation::contextualize(
    Table_ddl_parse_context *pc) {
  return (super::contextualize(pc) ||
          set_default_collation(pc->create_info, value));
}

bool PT_locking_clause::contextualize(Parse_context *pc) {
  LEX *lex = pc->thd->lex;

  if (lex->is_explain()) return false;

  if (m_locked_row_action == Locked_row_action::SKIP)
    lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SKIP_LOCKED);

  if (m_locked_row_action == Locked_row_action::NOWAIT)
    lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_NOWAIT);

  lex->safe_to_cache_query = false;

  return set_lock_for_tables(pc);
}

using Local_tables_iterator =
    IntrusiveListIterator<TABLE_LIST, &TABLE_LIST::next_local>;

/// A list interface over the TABLE_LIST::next_local pointer.
using Local_tables_list = IteratorContainer<Local_tables_iterator>;

bool PT_query_block_locking_clause::set_lock_for_tables(Parse_context *pc) {
  Local_tables_list local_tables(pc->select->table_list.first);
  for (TABLE_LIST *table_list : local_tables)
    if (!table_list->is_derived()) {
      if (table_list->lock_descriptor().type != TL_READ_DEFAULT) {
        my_error(ER_DUPLICATE_TABLE_LOCK, MYF(0), table_list->alias);
        return true;
      }

      pc->select->set_lock_for_table(get_lock_descriptor(), table_list);
    }
  return false;
}

bool PT_column_def::contextualize(Table_ddl_parse_context *pc) {
  // Since Alter_info objects are allocated on a mem_root and never
  // destroyed we (move)-assign an empty vector to cf_appliers to
  // ensure any dynamic memory is released. This must be done whenever
  // leaving this scope since appliers may be added in
  // field_def->contextualize(pc).
  auto clr_appliers = create_scope_guard([&]() {
    pc->alter_info->cf_appliers = decltype(pc->alter_info->cf_appliers)();
  });

  if (super::contextualize(pc) || field_def->contextualize(pc) ||
      contextualize_safe(pc, opt_column_constraint))
    return true;

  pc->alter_info->flags |= field_def->alter_info_flags;
  dd::Column::enum_hidden_type field_hidden_type =
      (field_def->type_flags & FIELD_IS_INVISIBLE)
          ? dd::Column::enum_hidden_type::HT_HIDDEN_USER
          : dd::Column::enum_hidden_type::HT_VISIBLE;

  return pc->alter_info->add_field(
      pc->thd, &field_ident, field_def->type, field_def->length, field_def->dec,
      field_def->type_flags, field_def->default_value,
      field_def->on_update_value, &field_def->comment, nullptr,
      field_def->interval_list, field_def->charset,
      field_def->has_explicit_collation, field_def->uint_geom_type,
      &field_def->m_zip_dict, field_def->gcol_info, field_def->default_val_info,
      opt_place, field_def->m_srid, field_def->check_const_spec_list,
      field_hidden_type);
}

Sql_cmd *PT_create_table_stmt::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;

  lex->sql_command = SQLCOM_CREATE_TABLE;

  Parse_context pc(thd, lex->current_query_block());

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  TABLE_LIST *table = pc.select->add_table_to_list(
      thd, table_name, nullptr, TL_OPTION_UPDATING, TL_WRITE, MDL_SHARED);
  if (table == nullptr) return nullptr;

  table->open_strategy = TABLE_LIST::OPEN_FOR_CREATE;

  lex->create_info = &m_create_info;
  Table_ddl_parse_context pc2(thd, pc.select, &m_alter_info);

  pc2.create_info->options = 0;
  if (is_temporary) pc2.create_info->options |= HA_LEX_CREATE_TMP_TABLE;
  if (only_if_not_exists)
    pc2.create_info->options |= HA_LEX_CREATE_IF_NOT_EXISTS;

  pc2.create_info->default_table_charset = nullptr;

  lex->name.str = nullptr;
  lex->name.length = 0;

  TABLE_LIST *qe_tables = nullptr;

  if (opt_like_clause != nullptr) {
    pc2.create_info->options |= HA_LEX_CREATE_TABLE_LIKE;
    TABLE_LIST **like_clause_table = &lex->query_tables->next_global;
    TABLE_LIST *src_table = pc.select->add_table_to_list(
        thd, opt_like_clause, nullptr, 0, TL_READ, MDL_SHARED_READ);
    if (!src_table) return nullptr;
    /* CREATE TABLE ... LIKE is not allowed for views. */
    src_table->required_type = dd::enum_table_type::BASE_TABLE;
    qe_tables = *like_clause_table;
  } else {
    if (opt_table_element_list) {
      for (auto element : *opt_table_element_list) {
        if (element->contextualize(&pc2)) return nullptr;
      }
    }

    if (opt_create_table_options) {
      for (auto option : *opt_create_table_options)
        if (option->contextualize(&pc2)) return nullptr;
    }

    if (opt_partitioning) {
      TABLE_LIST **exclude_part_tables = lex->query_tables_last;
      if (opt_partitioning->contextualize(&pc)) return nullptr;
      /*
        Remove all tables used in PARTITION clause from the global table
        list. Partitioning with subqueries is not allowed anyway.
      */
      *exclude_part_tables = nullptr;
      lex->query_tables_last = exclude_part_tables;

      lex->part_info = &opt_partitioning->part_info;
    }

    switch (on_duplicate) {
      case On_duplicate::IGNORE_DUP:
        lex->set_ignore(true);
        break;
      case On_duplicate::REPLACE_DUP:
        lex->duplicates = DUP_REPLACE;
        break;
      case On_duplicate::ERROR:
        lex->duplicates = DUP_ERROR;
        break;
    }

    if (opt_query_expression) {
      TABLE_LIST **query_expression_tables = &lex->query_tables->next_global;
      /*
        In CREATE TABLE t ... SELECT the table_list initially contains
        here a table entry for the destination table `t'.
        Backup it and clean the table list for the processing of
        the query expression and push `t' back to the beginning of the
        table_list finally.

        @todo: Don't save the CREATE destination table in
               Query_block::table_list and remove this backup & restore.

        The following work only with the local list, the global list
        is created correctly in this case
      */
      SQL_I_List<TABLE_LIST> save_list;
      Query_block *const save_query_block = pc.select;
      save_query_block->table_list.save_and_clear(&save_list);

      if (opt_query_expression->contextualize(&pc)) return nullptr;

      /*
        The following work only with the local list, the global list
        is created correctly in this case
      */
      save_query_block->table_list.push_front(&save_list);
      qe_tables = *query_expression_tables;
    }
  }

  lex->set_current_query_block(pc.select);
  if ((pc2.create_info->used_fields & HA_CREATE_USED_ENGINE) &&
      !pc2.create_info->db_type) {
    pc2.create_info->db_type =
        pc2.create_info->options & HA_LEX_CREATE_TMP_TABLE
            ? ha_default_temp_handlerton(thd)
            : ha_default_handlerton(thd);
    push_warning_printf(
        thd, Sql_condition::SL_WARNING, ER_WARN_USING_OTHER_HANDLER,
        ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
        ha_resolve_storage_engine_name(pc2.create_info->db_type),
        table_name->table.str);
  }
  create_table_set_open_action_and_adjust_tables(lex);

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_create_table(&m_alter_info, qe_tables);
}

bool PT_table_locking_clause::set_lock_for_tables(Parse_context *pc) {
  assert(!m_tables.empty());
  for (Table_ident *table_ident : m_tables) {
    Query_block *select = pc->select;

    TABLE_LIST *table_list = select->find_table_by_name(table_ident);

    THD *thd = pc->thd;

    if (table_list == nullptr)
      return raise_error(thd, table_ident, ER_UNRESOLVED_TABLE_LOCK);

    if (table_list->lock_descriptor().type != TL_READ_DEFAULT)
      return raise_error(thd, table_ident, ER_DUPLICATE_TABLE_LOCK);

    select->set_lock_for_table(get_lock_descriptor(), table_list);
  }

  return false;
}

bool PT_show_table_base::make_table_base_cmd(THD *thd, bool *temporary) {
  LEX *const lex = thd->lex;
  Parse_context pc(thd, lex->current_query_block());

  lex->sql_command = m_sql_command;

  // Create empty query block and add user specfied table.
  TABLE_LIST **query_tables_last = lex->query_tables_last;
  Query_block *schema_query_block = lex->new_empty_query_block();
  if (schema_query_block == nullptr) return true;
  TABLE_LIST *tbl = schema_query_block->add_table_to_list(
      thd, m_table_ident, nullptr, 0, TL_READ, MDL_SHARED_READ);
  if (tbl == nullptr) return true;
  lex->query_tables_last = query_tables_last;

  if (m_wild.str && lex->set_wild(m_wild)) return true;  // OOM

  TABLE *show_table = find_temporary_table(thd, tbl);
  *temporary = show_table != nullptr;

  // If its a temporary table then use schema_table implementation,
  // otherwise read I_S system view:
  if (*temporary) {
    Query_block *query_block = lex->current_query_block();

    if (m_where != nullptr) {
      if (m_where->itemize(&pc, &m_where)) return true;
      query_block->set_where_cond(m_where);
    }

    enum enum_schema_tables schema_table = m_sql_command == SQLCOM_SHOW_FIELDS
                                               ? SCH_TMP_TABLE_COLUMNS
                                               : SCH_TMP_TABLE_KEYS;
    if (make_schema_query_block(thd, query_block, schema_table)) return true;

    TABLE_LIST *table_list = query_block->table_list.first;
    table_list->schema_query_block = schema_query_block;
    table_list->schema_table_reformed = true;
  } else {
    Query_block *sel = nullptr;
    switch (m_sql_command) {
      case SQLCOM_SHOW_FIELDS:
        sel = dd::info_schema::build_show_columns_query(
            m_pos, thd, m_table_ident, lex->wild, m_where);
        break;
      case SQLCOM_SHOW_KEYS:
        sel = dd::info_schema::build_show_keys_query(m_pos, thd, m_table_ident,
                                                     m_where);
        break;
      default:
        assert(false);
        sel = nullptr;
        break;
    }

    if (sel == nullptr) return true;

    TABLE_LIST *table_list = sel->table_list.first;
    table_list->schema_query_block = schema_query_block;
  }

  return false;
}

static void setup_lex_show_cmd_type(THD *thd, Show_cmd_type show_cmd_type) {
  thd->lex->verbose = false;
  thd->lex->m_extended_show = false;

  switch (show_cmd_type) {
    case Show_cmd_type::STANDARD:
      break;
    case Show_cmd_type::FULL_SHOW:
      thd->lex->verbose = true;
      break;
    case Show_cmd_type::EXTENDED_SHOW:
      thd->lex->m_extended_show = true;
      break;
    case Show_cmd_type::EXTENDED_FULL_SHOW:
      thd->lex->verbose = true;
      thd->lex->m_extended_show = true;
      break;
  }
}

Sql_cmd *PT_show_binlog_events::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->mi.log_file_name = m_opt_log_file_name.str;

  Parse_context pc(thd, thd->lex->current_query_block());
  if (contextualize_safe(&pc, m_opt_limit_clause)) return nullptr;  // OOM

  return &m_sql_cmd;
}

Sql_cmd *PT_show_binlogs::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_charsets::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_character_set_query(m_pos, thd, lex->wild,
                                                      m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_collations::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_collation_query(m_pos, thd, lex->wild,
                                                  m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_count_base::make_cmd_generic(
    THD *thd, LEX_CSTRING diagnostic_variable_name) {
  LEX *const lex = thd->lex;
  lex->sql_command = SQLCOM_SELECT;

  // SHOW COUNT(*) { ERRORS | WARNINGS } doesn't clear them.
  lex->keep_diagnostics = DA_KEEP_DIAGNOSTICS;

  Parse_context pc(thd, lex->current_query_block());
  Item *var = get_system_var(
      &pc, OPT_SESSION,
      to_lex_string(diagnostic_variable_name),  // TODO: use LEX_CSTRING
      {}, false);
  if (var == nullptr) {
    assert(false);
    return nullptr;  // should never happen
  }

  constexpr const char session_prefix[] = "@@session.";
  assert(diagnostic_variable_name.length <= MAX_SYS_VAR_LENGTH);
  char buff[sizeof(session_prefix) + MAX_SYS_VAR_LENGTH + 1];
  /*
    We set the name of Item to @@session.var_name because that then is used
    as the column name in the output.
  */
  char *end =
      strxmov(buff, session_prefix, diagnostic_variable_name.str, nullptr);
  var->item_name.copy(buff, end - buff);

  add_item_to_list(thd, var);

  return new (thd->mem_root) Sql_cmd_select(nullptr);
}

Sql_cmd *PT_show_create_database::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  assert(lex->create_info == nullptr);
  lex->create_info = thd->alloc_typed<HA_CREATE_INFO>();
  if (lex->create_info == nullptr) return nullptr;  // OOM
  lex->create_info->options = m_if_not_exists ? HA_LEX_CREATE_IF_NOT_EXISTS : 0;
  lex->name = m_name;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_event::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->spname = m_spname;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_function::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->spname = m_spname;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_procedure::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->spname = m_spname;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_table::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  assert(lex->create_info == nullptr);
  lex->create_info = thd->alloc_typed<HA_CREATE_INFO>();
  if (lex->create_info == nullptr) return nullptr;  // OOM
  lex->create_info->storage_media = HA_SM_DEFAULT;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_view::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_trigger::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->spname = m_spname;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_create_user::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->grant_user = m_user;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_databases::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_databases_query(m_pos, thd, lex->wild,
                                                  m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_client_stats::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_CLIENT_STATS)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_index_stats::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_INDEX_STATS)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_table_stats::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_TABLE_STATS)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_thread_stats::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_THREAD_STATS)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_user_stats::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_USER_STATS)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_engine_logs::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  assert(lex->create_info == nullptr);
  lex->create_info = thd->alloc_typed<HA_CREATE_INFO>();
  if (lex->create_info == nullptr) return nullptr;  // OOM
  if (!m_all && resolve_engine(thd, to_lex_cstring(m_engine), false, true,
                               &lex->create_info->db_type))
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_engine_mutex::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  assert(lex->create_info == nullptr);
  lex->create_info = thd->alloc_typed<HA_CREATE_INFO>();
  if (lex->create_info == nullptr) return nullptr;  // OOM
  if (!m_all && resolve_engine(thd, to_lex_cstring(m_engine), false, true,
                               &lex->create_info->db_type))
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_engine_status::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  assert(lex->create_info == nullptr);
  lex->create_info = thd->alloc_typed<HA_CREATE_INFO>();
  if (lex->create_info == nullptr) return nullptr;  // OOM
  if (!m_all && resolve_engine(thd, to_lex_cstring(m_engine), false, true,
                               &lex->create_info->db_type))
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_engines::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_ENGINES)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_errors::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;
  // SHOW ERRORS will not clear diagnostics
  lex->keep_diagnostics = DA_KEEP_DIAGNOSTICS;

  Parse_context pc(thd, thd->lex->current_query_block());
  if (contextualize_safe(&pc, m_opt_limit_clause)) return nullptr;  // OOM

  return &m_sql_cmd;
}

Sql_cmd *PT_show_fields::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;
  assert(lex->query_block->db == nullptr);

  setup_lex_show_cmd_type(thd, m_show_cmd_type);
  lex->current_query_block()->parsing_place = CTX_SELECT_LIST;
  if (make_table_base_cmd(thd, &m_sql_cmd.m_temporary)) return nullptr;
  // WL#6599 opt_describe_column is handled during prepare stage in
  // prepare_schema_dd_view instead of execution stage
  lex->current_query_block()->parsing_place = CTX_NONE;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_keys::make_cmd(THD *thd) {
  thd->lex->m_extended_show = m_extended_show;

  if (make_table_base_cmd(thd, &m_sql_cmd.m_temporary)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_events::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;
  lex->query_block->db = m_opt_db;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_events_query(m_pos, thd, lex->wild,
                                               m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_master_status::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_open_tables::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  Parse_context pc(thd, lex->query_block);

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM
  if (m_where != nullptr) {
    if (m_where->itemize(&pc, &m_where)) return nullptr;
    lex->query_block->set_where_cond(m_where);
  }
  lex->query_block->db = m_opt_db;

  if (prepare_schema_table(thd, lex, 0, SCH_OPEN_TABLES)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_plugins::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (prepare_schema_table(thd, lex, 0, SCH_PLUGINS)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_privileges::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_processlist::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  m_sql_cmd.set_use_pfs(pfs_processlist_enabled);
  if (pfs_processlist_enabled) {
    if (build_processlist_query(m_pos, thd, m_sql_cmd.verbose()))
      return nullptr;
  }

  return &m_sql_cmd;
}

Sql_cmd *PT_show_routine_code::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_profile::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->profile_options = m_opt_profile_options;
  lex->show_profile_query_id = m_opt_query_id;

  Parse_context pc(thd, thd->lex->current_query_block());
  if (contextualize_safe(&pc, m_opt_limit_clause)) return nullptr;  // OOM

  if (prepare_schema_table(thd, lex, 0, SCH_PROFILES)) return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_profiles::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_relaylog_events::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->mi.log_file_name = m_opt_log_file_name.str;
  if (lex->set_channel_name(m_opt_channel_name)) return nullptr;  // OOM

  Parse_context pc(thd, thd->lex->current_query_block());
  if (contextualize_safe(&pc, m_opt_limit_clause)) return nullptr;  // OOM

  return &m_sql_cmd;
}

Sql_cmd *PT_show_replicas::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_replica_status::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (lex->set_channel_name(m_opt_channel_name)) return nullptr;  // OOM

  return &m_sql_cmd;
}

Sql_cmd *PT_show_status::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (m_var_type == OPT_SESSION) {
    if (build_show_session_status(m_pos, thd, lex->wild, m_where) == nullptr)
      return nullptr;
  } else if (m_var_type == OPT_GLOBAL) {
    if (build_show_global_status(m_pos, thd, lex->wild, m_where) == nullptr)
      return nullptr;
  }
  return &m_sql_cmd;
}

Sql_cmd *PT_show_status_func::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_procedures_query(m_pos, thd, lex->wild,
                                                   m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_status_proc::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_procedures_query(m_pos, thd, lex->wild,
                                                   m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_table_status::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  lex->query_block->db = m_opt_db;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_tables_query(m_pos, thd, lex->wild, m_where,
                                               true) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_tables::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;
  setup_lex_show_cmd_type(thd, m_show_cmd_type);

  lex->query_block->db = m_opt_db;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_tables_query(m_pos, thd, lex->wild, m_where,
                                               false) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_triggers::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;
  lex->verbose = m_full;
  lex->query_block->db = m_opt_db;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (dd::info_schema::build_show_triggers_query(m_pos, thd, lex->wild,
                                                 m_where) == nullptr)
    return nullptr;

  return &m_sql_cmd;
}

Sql_cmd *PT_show_variables::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;

  if (m_wild.str && lex->set_wild(m_wild)) return nullptr;  // OOM

  if (m_var_type == OPT_SESSION) {
    if (build_show_session_variables(m_pos, thd, lex->wild, m_where) == nullptr)
      return nullptr;
  } else if (m_var_type == OPT_GLOBAL) {
    if (build_show_global_variables(m_pos, thd, lex->wild, m_where) == nullptr)
      return nullptr;
  }
  return &m_sql_cmd;
}

Sql_cmd *PT_show_warnings::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;
  // SHOW WARNINGS will not clear diagnostics
  lex->keep_diagnostics = DA_KEEP_DIAGNOSTICS;

  Parse_context pc(thd, thd->lex->current_query_block());
  if (contextualize_safe(&pc, m_opt_limit_clause)) return nullptr;  // OOM

  return &m_sql_cmd;
}

bool PT_alter_table_change_column::contextualize(Table_ddl_parse_context *pc) {
  // Since Alter_info objects are allocated on a mem_root and never
  // destroyed we (move)-assign an empty vector to cf_appliers to
  // ensure any dynamic memory is released. This must be done whenever
  // leaving this scope since appliers may be added in
  // m_field_def->contextualize(pc).
  auto clr_appliers = create_scope_guard([&]() {
    pc->alter_info->cf_appliers = decltype(pc->alter_info->cf_appliers)();
  });

  if (super::contextualize(pc) || m_field_def->contextualize(pc)) return true;
  pc->alter_info->flags |= m_field_def->alter_info_flags;
  dd::Column::enum_hidden_type field_hidden_type =
      (m_field_def->type_flags & FIELD_IS_INVISIBLE)
          ? dd::Column::enum_hidden_type::HT_HIDDEN_USER
          : dd::Column::enum_hidden_type::HT_VISIBLE;

  return pc->alter_info->add_field(
      pc->thd, &m_new_name, m_field_def->type, m_field_def->length,
      m_field_def->dec, m_field_def->type_flags, m_field_def->default_value,
      m_field_def->on_update_value, &m_field_def->comment, m_old_name.str,
      m_field_def->interval_list, m_field_def->charset,
      m_field_def->has_explicit_collation, m_field_def->uint_geom_type,
      &m_field_def->m_zip_dict, m_field_def->gcol_info,
      m_field_def->default_val_info, m_opt_place, m_field_def->m_srid, nullptr,
      field_hidden_type);
}

bool PT_alter_table_rename::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;  // OOM

  if (m_ident->db.str) {
    LEX_STRING db_str = to_lex_string(m_ident->db);
    if (check_and_convert_db_name(&db_str, false) != Ident_name_check::OK)
      return true;
    pc->alter_info->new_db_name = to_lex_cstring(db_str);
  } else if (pc->thd->lex->copy_db_to(&pc->alter_info->new_db_name.str,
                                      &pc->alter_info->new_db_name.length)) {
    return true;
  }
  switch (check_table_name(m_ident->table.str, m_ident->table.length)) {
    case Ident_name_check::WRONG:
      my_error(ER_WRONG_TABLE_NAME, MYF(0), m_ident->table.str);
      return true;
    case Ident_name_check::TOO_LONG:
      my_error(ER_TOO_LONG_IDENT, MYF(0), m_ident->table.str);
      return true;
    case Ident_name_check::OK:
      break;
  }
  pc->alter_info->new_table_name = m_ident->table;
  return false;
}

bool PT_alter_table_convert_to_charset::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;  // OOM

  const CHARSET_INFO *const cs =
      m_charset ? m_charset : pc->thd->variables.collation_database;
  const CHARSET_INFO *const collation = m_collation ? m_collation : cs;

  if (!my_charset_same(cs, collation)) {
    my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0), collation->name,
             cs->csname);
    return true;
  }

  if ((pc->create_info->used_fields & HA_CREATE_USED_DEFAULT_CHARSET) &&
      pc->create_info->default_table_charset && collation &&
      !my_charset_same(pc->create_info->default_table_charset, collation)) {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0), "CHARACTER SET ",
             pc->create_info->default_table_charset->csname, "CHARACTER SET ",
             collation->csname);
    return true;
  }

  pc->create_info->table_charset = pc->create_info->default_table_charset =
      collation;
  pc->create_info->used_fields |=
      HA_CREATE_USED_CHARSET | HA_CREATE_USED_DEFAULT_CHARSET;
  if (m_collation != nullptr)
    pc->create_info->used_fields |= HA_CREATE_USED_DEFAULT_COLLATE;
  return false;
}

bool PT_alter_table_add_partition_def_list::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  Partition_parse_context part_pc(pc->thd, &m_part_info,
                                  is_add_or_reorganize_partition());
  for (auto *part_def : *m_def_list) {
    if (part_def->contextualize(&part_pc)) return true;
  }
  m_part_info.num_parts = m_part_info.partitions.elements;

  return false;
}

bool PT_alter_table_reorganize_partition_into::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *const lex = pc->thd->lex;
  lex->no_write_to_binlog = m_no_write_to_binlog;

  assert(pc->alter_info->partition_names.is_empty());
  pc->alter_info->partition_names = m_partition_names;

  Partition_parse_context ppc(pc->thd, &m_partition_info,
                              is_add_or_reorganize_partition());

  for (auto *part_def : *m_into)
    if (part_def->contextualize(&ppc)) return true;

  m_partition_info.num_parts = m_partition_info.partitions.elements;
  lex->part_info = &m_partition_info;
  return false;
}

bool PT_alter_table_exchange_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->alter_info->with_validation = m_validation;

  String *s = new (pc->mem_root) String(
      m_partition_name.str, m_partition_name.length, system_charset_info);
  if (s == nullptr || pc->alter_info->partition_names.push_back(s) ||
      !pc->select->add_table_to_list(pc->thd, m_table_name, nullptr,
                                     TL_OPTION_UPDATING, TL_READ_NO_INSERT,
                                     MDL_SHARED_NO_WRITE)) {
    return true;
  }

  return false;
}

/**
  A common initialization part of ALTER TABLE statement variants.

  @param pc             The parse context.
  @param table_name     The name of a table to alter.
  @param algo           The ALGORITHM clause: inplace, copy etc.
  @param lock           The LOCK clause: none, shared, exclusive etc.
  @param validation     The WITH or WITHOUT VALIDATION clause.

  @returns false on success, true on error.

*/
static bool init_alter_table_stmt(Table_ddl_parse_context *pc,
                                  Table_ident *table_name,
                                  Alter_info::enum_alter_table_algorithm algo,
                                  Alter_info::enum_alter_table_lock lock,
                                  Alter_info::enum_with_validation validation) {
  LEX *lex = pc->thd->lex;
  if (!lex->query_block->add_table_to_list(
          pc->thd, table_name, nullptr, TL_OPTION_UPDATING, TL_READ_NO_INSERT,
          MDL_SHARED_UPGRADABLE))
    return true;
  lex->query_block->init_order();
  pc->create_info->db_type = nullptr;
  pc->create_info->default_table_charset = nullptr;
  pc->create_info->row_type = ROW_TYPE_NOT_USED;

  pc->alter_info->new_db_name =
      LEX_CSTRING{lex->query_block->table_list.first->db,
                  lex->query_block->table_list.first->db_length};
  lex->no_write_to_binlog = false;
  pc->create_info->storage_media = HA_SM_DEFAULT;

  pc->alter_info->requested_algorithm = algo;
  pc->alter_info->requested_lock = lock;
  pc->alter_info->with_validation = validation;
  return false;
}

Sql_cmd *PT_alter_table_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_ALTER_TABLE;

  thd->lex->create_info = &m_create_info;
  Table_ddl_parse_context pc(thd, thd->lex->current_query_block(),
                             &m_alter_info);

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  if (init_alter_table_stmt(&pc, m_table_name, m_algo, m_lock, m_validation))
    return nullptr;

  if (m_opt_actions) {
    /*
      Move RENAME TO <table_name> clauses to the head of array, so they are
      processed before ADD FOREIGN KEY clauses. The latter need to know target
      database name for proper contextualization.

      Use stable sort to preserve order of other clauses which might be
      sensitive to it.
    */
    std::stable_sort(
        m_opt_actions->begin(), m_opt_actions->end(),
        [](const PT_ddl_table_option *lhs, const PT_ddl_table_option *rhs) {
          return lhs->is_rename_table() && !rhs->is_rename_table();
        });

    for (auto *action : *m_opt_actions)
      if (action->contextualize(&pc)) return nullptr;
  }

  if ((pc.create_info->used_fields & HA_CREATE_USED_ENGINE) &&
      !pc.create_info->db_type) {
    pc.create_info->used_fields &= ~HA_CREATE_USED_ENGINE;
  }

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_alter_table(&m_alter_info);
}

Sql_cmd *PT_alter_table_standalone_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_ALTER_TABLE;

  thd->lex->create_info = &m_create_info;

  Table_ddl_parse_context pc(thd, thd->lex->current_query_block(),
                             &m_alter_info);

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  if (init_alter_table_stmt(&pc, m_table_name, m_algo, m_lock, m_validation) ||
      m_action->contextualize(&pc))
    return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return m_action->make_cmd(&pc);
}

Sql_cmd *PT_repair_table_stmt::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;

  lex->sql_command = SQLCOM_REPAIR;

  Query_block *const select = lex->current_query_block();

  lex->no_write_to_binlog = m_no_write_to_binlog;
  lex->check_opt.flags |= m_flags;
  lex->check_opt.sql_flags |= m_sql_flags;
  if (select->add_tables(thd, m_table_list, TL_OPTION_UPDATING, TL_UNLOCK,
                         MDL_SHARED_READ))
    return nullptr;

  lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_repair_table(&m_alter_info);
}

Sql_cmd *PT_analyze_table_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_ANALYZE;

  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  Parse_context pc(thd, select);
  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  lex->no_write_to_binlog = m_no_write_to_binlog;
  if (select->add_tables(thd, m_table_list, TL_OPTION_UPDATING, TL_UNLOCK,
                         MDL_SHARED_READ))
    return nullptr;

  thd->lex->alter_info = &m_alter_info;
  auto cmd = new (thd->mem_root)
      Sql_cmd_analyze_table(thd, &m_alter_info, m_command, m_num_buckets);
  if (cmd == nullptr) return nullptr;
  if (m_command != Sql_cmd_analyze_table::Histogram_command::NONE) {
    if (cmd->set_histogram_fields(m_columns)) return nullptr;
  }
  return cmd;
}

Sql_cmd *PT_check_table_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_CHECK;

  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  if (lex->sphead) {
    my_error(ER_SP_BADSTATEMENT, MYF(0), "CHECK");
    return nullptr;
  }

  Parse_context pc(thd, select);

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  lex->check_opt.flags |= m_flags;
  lex->check_opt.sql_flags |= m_sql_flags;
  if (select->add_tables(thd, m_table_list, TL_OPTION_UPDATING, TL_UNLOCK,
                         MDL_SHARED_READ))
    return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_check_table(&m_alter_info);
}

Sql_cmd *PT_optimize_table_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_OPTIMIZE;

  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  lex->no_write_to_binlog = m_no_write_to_binlog;

  Parse_context pc(thd, select);

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  if (select->add_tables(thd, m_table_list, TL_OPTION_UPDATING, TL_UNLOCK,
                         MDL_SHARED_READ))
    return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_optimize_table(&m_alter_info);
}

Sql_cmd *PT_drop_index_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_DROP_INDEX;

  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  m_alter_info.flags = Alter_info::ALTER_DROP_INDEX;
  m_alter_info.drop_list.push_back(&m_alter_drop);
  if (!select->add_table_to_list(thd, m_table, nullptr, TL_OPTION_UPDATING,
                                 TL_READ_NO_INSERT, MDL_SHARED_UPGRADABLE))
    return nullptr;

  m_alter_info.requested_algorithm = m_algo;
  m_alter_info.requested_lock = m_lock;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_drop_index(&m_alter_info);
}

Sql_cmd *PT_truncate_table_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_TRUNCATE;

  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  if (!select->add_table_to_list(thd, m_table, nullptr, TL_OPTION_UPDATING,
                                 TL_WRITE, MDL_EXCLUSIVE))
    return nullptr;
  return &m_cmd_truncate_table;
}

bool PT_assign_to_keycache::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  if (!pc->select->add_table_to_list(pc->thd, m_table, nullptr, 0, TL_READ,
                                     MDL_SHARED_READ, m_index_hints))
    return true;
  return false;
}

bool PT_adm_partition::contextualize(Table_ddl_parse_context *pc) {
  pc->alter_info->flags |= Alter_info::ALTER_ADMIN_PARTITION;

  assert(pc->alter_info->partition_names.is_empty());
  if (m_opt_partitions == nullptr)
    pc->alter_info->flags |= Alter_info::ALTER_ALL_PARTITION;
  else
    pc->alter_info->partition_names = *m_opt_partitions;
  return false;
}

Sql_cmd *PT_cache_index_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_ASSIGN_TO_KEYCACHE;

  Table_ddl_parse_context pc(thd, thd->lex->current_query_block(),
                             &m_alter_info);

  for (auto *tbl_index_list : *m_tbl_index_lists)
    if (tbl_index_list->contextualize(&pc)) return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root)
      Sql_cmd_cache_index(&m_alter_info, m_key_cache_name);
}

Sql_cmd *PT_cache_index_partitions_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_ASSIGN_TO_KEYCACHE;

  Query_block *const select = thd->lex->current_query_block();

  Table_ddl_parse_context pc(thd, select, &m_alter_info);

  if (m_partitions->contextualize(&pc)) return nullptr;

  if (!select->add_table_to_list(thd, m_table, nullptr, 0, TL_READ,
                                 MDL_SHARED_READ, m_opt_key_usage_list))
    return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root)
      Sql_cmd_cache_index(&m_alter_info, m_key_cache_name);
}

Sql_cmd *PT_load_index_partitions_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_PRELOAD_KEYS;

  Query_block *const select = thd->lex->current_query_block();

  Table_ddl_parse_context pc(thd, select, &m_alter_info);

  if (m_partitions->contextualize(&pc)) return nullptr;

  if (!select->add_table_to_list(
          thd, m_table, nullptr, m_ignore_leaves ? TL_OPTION_IGNORE_LEAVES : 0,
          TL_READ, MDL_SHARED_READ, m_opt_cache_key_list))
    return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_load_index(&m_alter_info);
}

Sql_cmd *PT_load_index_stmt::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_PRELOAD_KEYS;

  Table_ddl_parse_context pc(thd, thd->lex->current_query_block(),
                             &m_alter_info);

  if (m_opt_hints != nullptr && m_opt_hints->contextualize(&pc)) return nullptr;

  for (auto *preload_keys : *m_preload_list)
    if (preload_keys->contextualize(&pc)) return nullptr;

  thd->lex->alter_info = &m_alter_info;
  return new (thd->mem_root) Sql_cmd_load_index(&m_alter_info);
}

/* Window functions */

Item *PT_border::build_addop(Item_cache *order_expr, bool prec, bool asc,
                             const Window *window) {
  /*
    Check according to SQL 2014 7.15 <window clause> SR 13.a.iii:
    ORDER BY expression is temporal iff bound is temporal.
  */
  if (order_expr->result_type() == STRING_RESULT && order_expr->is_temporal()) {
    if (!m_date_time) {
      my_error(ER_WINDOW_RANGE_FRAME_TEMPORAL_TYPE, MYF(0),
               window->printable_name());
      return nullptr;
    }
  } else {
    if (m_date_time) {
      my_error(ER_WINDOW_RANGE_FRAME_NUMERIC_TYPE, MYF(0),
               window->printable_name());
      return nullptr;
    }
  }

  Item *addop;
  const bool substract = prec ? asc : !asc;
  if (m_date_time) {
    addop =
        new Item_date_add_interval(order_expr, m_value, m_int_type, substract);
  } else {
    if (substract)
      addop = new Item_func_minus(order_expr, m_value);
    else
      addop = new Item_func_plus(order_expr, m_value);
  }
  return addop;
}

PT_json_table_column_for_ordinality::PT_json_table_column_for_ordinality(
    LEX_STRING name)
    : m_name(name.str) {}

PT_json_table_column_for_ordinality::~PT_json_table_column_for_ordinality() =
    default;

bool PT_json_table_column_for_ordinality::contextualize(Parse_context *pc) {
  assert(m_column == nullptr);
  m_column = make_unique_destroy_only<Json_table_column>(
      pc->mem_root, enum_jt_column::JTC_ORDINALITY);
  if (m_column == nullptr) return true;
  m_column->init_for_tmp_table(MYSQL_TYPE_LONGLONG, 10, 0, true, true, 8,
                               m_name);
  return super::contextualize(pc);
}

PT_json_table_column_with_path::PT_json_table_column_with_path(
    unique_ptr_destroy_only<Json_table_column> column, LEX_STRING name,
    PT_type *type, const CHARSET_INFO *collation)
    : m_column(std::move(column)),
      m_name(name.str),
      m_type(type),
      m_collation(collation) {}

PT_json_table_column_with_path::~PT_json_table_column_with_path() = default;

static bool check_unsupported_json_table_default(const Item *item) {
  if (item == nullptr) return false;

  // JSON_TABLE currently only supports string literals on JSON format in
  // DEFAULT clauses. Other literals used to be rejected by the grammar, but the
  // grammar was extended for JSON_VALUE and now accepts all types of literals.
  // Until JSON_TABLE gets support for non-string defaults, reject them here.
  if (item->data_type() != MYSQL_TYPE_VARCHAR) {
    my_error(
        ER_NOT_SUPPORTED_YET, MYF(0),
        "non-string DEFAULT value for a column in a JSON_TABLE expression");
    return true;
  }

  return false;
}

bool PT_json_table_column_with_path::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || m_type->contextualize(pc)) return true;

  if (m_column->m_path_string->itemize(pc, &m_column->m_path_string))
    return true;

  if (check_unsupported_json_table_default(m_column->m_default_empty_string) ||
      check_unsupported_json_table_default(m_column->m_default_error_string))
    return true;

  if (itemize_safe(pc, &m_column->m_default_empty_string)) return true;
  if (itemize_safe(pc, &m_column->m_default_error_string)) return true;

  const CHARSET_INFO *cs;
  if (merge_charset_and_collation(m_type->get_charset(), m_collation, &cs))
    return true;
  if (cs == nullptr) {
    cs = pc->thd->variables.collation_connection;
  }

  m_column->init(pc->thd,
                 m_name,                        // Alias
                 m_type->type,                  // Type
                 m_type->get_length(),          // Length
                 m_type->get_dec(),             // Decimals
                 m_type->get_type_flags(),      // Type modifier
                 nullptr,                       // Default value
                 nullptr,                       // On update value
                 &EMPTY_CSTR,                   // Comment
                 nullptr,                       // Change
                 m_type->get_interval_list(),   // Interval list
                 cs,                            // Charset & collation
                 m_collation != nullptr,        // Has "COLLATE" clause
                 m_type->get_uint_geom_type(),  // Geom type
                 nullptr,                       // Compression dictionary name
                 nullptr,                       // Gcol_info
                 nullptr,                       // Default gen expression
                 {},                            // SRID
                 dd::Column::enum_hidden_type::HT_VISIBLE);  // Hidden
  return false;
}

bool PT_json_table_column_with_nested_path::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;  // OOM

  if (m_path->itemize(pc, &m_path)) return true;

  auto nested_columns = new (pc->mem_root) List<Json_table_column>;
  if (nested_columns == nullptr) return true;  // OOM

  for (auto col : *m_nested_columns) {
    if (col->contextualize(pc) || nested_columns->push_back(col->get_column()))
      return true;
  }

  m_column = new (pc->mem_root) Json_table_column(m_path, nested_columns);
  if (m_column == nullptr) return true;  // OOM

  return false;
}

Sql_cmd *PT_explain_for_connection::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_EXPLAIN_OTHER;

  if (thd->lex->sphead) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "non-standalone EXPLAIN FOR CONNECTION");
    return nullptr;
  }
  if (thd->lex->is_explain_analyze) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "EXPLAIN ANALYZE FOR CONNECTION");
    return nullptr;
  }
  return &m_cmd;
}

Sql_cmd *PT_explain::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;
  switch (m_format) {
    case Explain_format_type::TRADITIONAL:
      lex->explain_format = new (thd->mem_root) Explain_format_traditional;
      break;
    case Explain_format_type::JSON:
      lex->explain_format = new (thd->mem_root) Explain_format_JSON;
      break;
    case Explain_format_type::TREE:
      lex->explain_format = new (thd->mem_root) Explain_format_tree;
      break;
    case Explain_format_type::TREE_WITH_EXECUTE:
      lex->explain_format = new (thd->mem_root) Explain_format_tree;
      lex->is_explain_analyze = true;
      break;
    default:
      assert(false);
      lex->explain_format = new (thd->mem_root) Explain_format_traditional;
  }
  if (lex->explain_format == nullptr) return nullptr;  // OOM

  Sql_cmd *ret = m_explainable_stmt->make_cmd(thd);
  if (ret == nullptr) return nullptr;  // OOM

  auto code = ret->sql_command_code();
  if (!is_explainable_query(code) && code != SQLCOM_EXPLAIN_OTHER) {
    assert(!"Should not happen!");
    my_error(ER_WRONG_USAGE, MYF(0), "EXPLAIN", "non-explainable query");
    return nullptr;
  }

  return ret;
}

Sql_cmd *PT_load_table::make_cmd(THD *thd) {
  LEX *const lex = thd->lex;
  Query_block *const select = lex->current_query_block();

  if (lex->sphead) {
    my_error(
        ER_SP_BADSTATEMENT, MYF(0),
        m_cmd.m_exchange.filetype == FILETYPE_CSV ? "LOAD DATA" : "LOAD XML");
    return nullptr;
  }

  lex->sql_command = SQLCOM_LOAD;

  switch (m_cmd.m_on_duplicate) {
    case On_duplicate::ERROR:
      lex->duplicates = DUP_ERROR;
      break;
    case On_duplicate::IGNORE_DUP:
      lex->set_ignore(true);
      break;
    case On_duplicate::REPLACE_DUP:
      lex->duplicates = DUP_REPLACE;
      break;
  }

  /* Fix lock for LOAD DATA CONCURRENT REPLACE */
  thr_lock_type lock_type = m_lock_type;
  if (lex->duplicates == DUP_REPLACE && lock_type == TL_WRITE_CONCURRENT_INSERT)
    lock_type = TL_WRITE_DEFAULT;

  if (!select->add_table_to_list(
          thd, m_cmd.m_table, nullptr, TL_OPTION_UPDATING, lock_type,
          lock_type == TL_WRITE_LOW_PRIORITY ? MDL_SHARED_WRITE_LOW_PRIO
                                             : MDL_SHARED_WRITE,
          nullptr, m_cmd.m_opt_partitions))
    return nullptr;

  /* We can't give an error in the middle when using LOCAL files */
  if (m_cmd.m_is_local_file && lex->duplicates == DUP_ERROR)
    lex->set_ignore(true);

  Parse_context pc(thd, select);
  if (contextualize_safe(&pc, &m_cmd.m_opt_fields_or_vars) ||
      contextualize_safe(&pc, &m_cmd.m_opt_set_fields) ||
      contextualize_safe(&pc, &m_cmd.m_opt_set_exprs))
    return nullptr;

  return &m_cmd;
}

bool PT_select_item_list::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;
  pc->select->fields = value;
  return false;
}

bool PT_limit_clause::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  if (limit_options.is_offset_first && limit_options.opt_offset != nullptr &&
      limit_options.opt_offset->itemize(pc, &limit_options.opt_offset))
    return true;

  if (limit_options.limit->itemize(pc, &limit_options.limit)) return true;

  if (!limit_options.is_offset_first && limit_options.opt_offset != nullptr &&
      limit_options.opt_offset->itemize(pc, &limit_options.opt_offset))
    return true;

  pc->select->select_limit = limit_options.limit;
  pc->select->offset_limit = limit_options.opt_offset;

  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
  return false;
}

bool PT_table_factor_table_ident::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  Yacc_state *yyps = &thd->m_parser_state->m_yacc;

  value = pc->select->add_table_to_list(
      thd, table_ident, opt_table_alias, 0, yyps->m_lock_type, yyps->m_mdl_type,
      opt_key_definition, opt_use_partition, nullptr, pc);
  if (value == nullptr) return true;
  if (pc->select->add_joined_table(value)) return true;
  return false;
}

bool PT_table_reference_list_parens::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || contextualize_array(pc, &table_list))
    return true;

  assert(table_list.size() >= 2);
  value = pc->select->nest_last_join(pc->thd, table_list.size());
  return value == nullptr;
}

bool PT_joined_table::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || contextualize_tabs(pc)) return true;

  if (m_type & JTT_NATURAL) tr1->add_join_natural(tr2);

  if (m_type & JTT_STRAIGHT) tr2->straight = true;

  return false;
}

bool PT_cross_join::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;
  value = pc->select->nest_last_join(pc->thd);
  return value == nullptr;
}

bool PT_joined_table_on::contextualize(Parse_context *pc) {
  if (this->contextualize_tabs(pc)) return true;

  if (push_new_name_resolution_context(pc, this->tr1, this->tr2)) {
    this->error(pc, this->join_pos);
    return true;
  }

  Query_block *sel = pc->select;
  sel->parsing_place = CTX_ON;

  if (super::contextualize(pc) || on->itemize(pc, &on)) return true;
  if (!on->is_bool_func()) {
    on = make_condition(pc, on);
    if (on == nullptr) return true;
  }
  assert(sel == pc->select);

  add_join_on(this->tr2, on);
  pc->thd->lex->pop_context();
  assert(sel->parsing_place == CTX_ON);
  sel->parsing_place = CTX_NONE;
  value = pc->select->nest_last_join(pc->thd);
  return value == nullptr;
}

bool PT_joined_table_using::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  tr1->add_join_natural(tr2);
  value = pc->select->nest_last_join(pc->thd);
  if (value == nullptr) return true;
  value->join_using_fields = using_fields;

  return false;
}

void PT_table_locking_clause::print_table_ident(const THD *thd,
                                                const Table_ident *ident,
                                                String *s) {
  if (ident->db.length > 0) {
    append_identifier(thd, s, ident->db.str, ident->db.length);
    s->append('.');
  }
  append_identifier(thd, s, ident->table.str, ident->table.length);
}

bool PT_table_locking_clause::raise_error(THD *thd, const Table_ident *name,
                                          int error) {
  String s;
  print_table_ident(thd, name, &s);
  my_error(error, MYF(0), s.ptr());
  return true;
}

bool PT_table_locking_clause::raise_error(int error) {
  my_error(error, MYF(0));
  return true;
}

bool PT_internal_variable_name_default::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  sys_var *tmp = find_sys_var(pc->thd, ident.str, ident.length);
  if (!tmp) return true;
  if (!tmp->is_struct()) {
    my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), ident.str);
    return true;
  }
  value.var = tmp;
  value.base_name.str = "default";
  value.base_name.length = 7;
  return false;
}

bool PT_option_value_following_option_type::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || name->contextualize(pc) ||
      (opt_expr != nullptr && opt_expr->itemize(pc, &opt_expr)))
    return true;

  if (name->value.var && name->value.var != trg_new_row_fake_var) {
    /* It is a system variable. */
    if (set_system_variable(pc->thd, &name->value, pc->thd->lex->option_type,
                            opt_expr))
      return true;
  } else {
    /*
      Not in trigger assigning value to new row,
      and option_type preceding local variable is illegal.
    */
    error(pc, pos);
    return true;
  }
  return false;
}

bool PT_option_value_no_option_type_user_var::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || expr->itemize(pc, &expr)) return true;

  THD *thd = pc->thd;
  Item_func_set_user_var *item =
      new (pc->mem_root) Item_func_set_user_var(name, expr);
  if (item == nullptr) return true;
  set_var_user *var = new (thd->mem_root) set_var_user(item);
  if (var == nullptr) return true;
  return thd->lex->var_list.push_back(var);
}

bool PT_option_value_no_option_type_sys_var::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || name->contextualize(pc) ||
      (opt_expr != nullptr && opt_expr->itemize(pc, &opt_expr)))
    return true;

  THD *thd = pc->thd;
  struct sys_var_with_base tmp = name->value;
  if (tmp.var == trg_new_row_fake_var) {
    error(pc, down_cast<PT_internal_variable_name_2d *>(name)->pos);
    return true;
  }
  /* Lookup if necessary: must be a system variable. */
  if (tmp.var == nullptr) {
    if (find_sys_var_null_base(thd, &tmp)) return true;
  }
  if (set_system_variable(thd, &tmp, type, opt_expr)) return true;
  return false;
}

bool PT_option_value_type::contextualize(Parse_context *pc) {
  pc->thd->lex->option_type = type;
  return super::contextualize(pc) || value->contextualize(pc);
}

bool PT_option_value_list_head::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
#ifndef NDEBUG
  LEX *old_lex = thd->lex;
#endif  // NDEBUG

  sp_create_assignment_lex(thd, delimiter_pos.raw.end);
  assert(thd->lex->query_block == thd->lex->current_query_block());
  Parse_context inner_pc(pc->thd, thd->lex->query_block);

  if (value->contextualize(&inner_pc)) return true;

  if (sp_create_assignment_instr(pc->thd, value_pos.raw.end)) return true;
  assert(thd->lex == old_lex && thd->lex->current_query_block() == pc->select);

  return false;
}

bool PT_start_option_value_list_no_type::contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || head->contextualize(pc)) return true;

  if (sp_create_assignment_instr(pc->thd, head_pos.raw.end)) return true;
  assert(pc->thd->lex->query_block == pc->thd->lex->current_query_block());
  pc->select = pc->thd->lex->query_block;

  if (tail != nullptr && tail->contextualize(pc)) return true;

  return false;
}

bool PT_transaction_characteristic::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  Item *item = new (pc->mem_root) Item_int(value);
  if (item == nullptr) return true;
  set_var *var = new (thd->mem_root)
      set_var(lex->option_type, find_sys_var(thd, name), NULL_CSTR, item);
  if (var == nullptr) return true;
  return lex->var_list.push_back(var);
}

bool PT_start_option_value_list_transaction::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  thd->lex->option_type = OPT_DEFAULT;
  if (characteristics->contextualize(pc)) return true;

  if (sp_create_assignment_instr(thd, end_pos.raw.end)) return true;
  assert(pc->thd->lex->query_block == pc->thd->lex->current_query_block());
  pc->select = pc->thd->lex->query_block;

  return false;
}

bool PT_start_option_value_list_following_option_type_eq::contextualize(
    Parse_context *pc) {
  if (super::contextualize(pc) || head->contextualize(pc)) return true;

  if (sp_create_assignment_instr(pc->thd, head_pos.raw.end)) return true;
  assert(pc->thd->lex->query_block == pc->thd->lex->current_query_block());
  pc->select = pc->thd->lex->query_block;

  if (opt_tail != nullptr && opt_tail->contextualize(pc)) return true;

  return false;
}

bool PT_start_option_value_list_following_option_type_transaction::
    contextualize(Parse_context *pc) {
  if (super::contextualize(pc) || characteristics->contextualize(pc))
    return true;

  if (sp_create_assignment_instr(pc->thd, characteristics_pos.raw.end))
    return true;
  assert(pc->thd->lex->query_block == pc->thd->lex->current_query_block());
  pc->select = pc->thd->lex->query_block;

  return false;
}

bool PT_start_option_value_list_type::contextualize(Parse_context *pc) {
  pc->thd->lex->option_type = type;
  return super::contextualize(pc) || list->contextualize(pc);
}

bool PT_set::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  lex->sql_command = SQLCOM_SET_OPTION;
  lex->option_type = OPT_SESSION;
  lex->var_list.clear();
  lex->autocommit = false;

  sp_create_assignment_lex(thd, set_pos.raw.end);
  assert(pc->thd->lex->query_block == pc->thd->lex->current_query_block());
  pc->select = pc->thd->lex->query_block;

  return list->contextualize(pc);
}

bool PT_into_destination::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *lex = pc->thd->lex;
  if (!pc->thd->lex->parsing_options.allows_select_into) {
    if (lex->sql_command == SQLCOM_SHOW_CREATE ||
        lex->sql_command == SQLCOM_CREATE_VIEW)
      my_error(ER_VIEW_SELECT_CLAUSE, MYF(0), "INTO");
    else
      error(pc, m_pos);
    return true;
  }
  return false;
}

bool PT_into_destination_outfile::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *lex = pc->thd->lex;
  lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  lex->result = new (pc->thd->mem_root) Query_result_export(&m_exchange);
  return lex->result == nullptr;
}

bool PT_into_destination_dumpfile::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *lex = pc->thd->lex;
  if (!lex->is_explain()) {
    lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
    lex->result = new (pc->thd->mem_root) Query_result_dump(&m_exchange);
    if (lex->result == nullptr) return true;
  }
  return false;
}

bool PT_select_var_list::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  List_iterator<PT_select_var> it(value);
  PT_select_var *var;
  while ((var = it++)) {
    if (var->contextualize(pc)) return true;
  }

  LEX *const lex = pc->thd->lex;
  if (lex->is_explain()) return false;

  Query_dumpvar *dumpvar = new (pc->mem_root) Query_dumpvar();
  if (dumpvar == nullptr) return true;

  dumpvar->var_list = value;
  lex->result = dumpvar;
  lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);

  return false;
}

bool PT_query_expression::contextualize(Parse_context *pc) {
  if (contextualize_safe(pc, m_with_clause))
    return true; /* purecov: inspected */

  if (Parse_tree_node::contextualize(pc) || m_body->contextualize(pc))
    return true;

  if (contextualize_order_and_limit(pc)) return true;

  return false;
}

bool PT_subquery::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *lex = pc->thd->lex;
  if (!lex->expr_allows_subselect || lex->sql_command == SQLCOM_PURGE) {
    error(pc, pos);
    return true;
  }

  // Create a Query_expression and Query_block for the subquery's query
  // expression.
  Query_block *child = lex->new_query(pc->select);
  if (child == nullptr) return true;

  Parse_context inner_pc(pc->thd, child);

  if (m_is_derived_table) child->linkage = DERIVED_TABLE_TYPE;

  if (qe->contextualize(&inner_pc)) return true;

  if (qe->has_into_clause()) {
    my_error(ER_MISPLACED_INTO, MYF(0));
    return true;
  }

  query_block = inner_pc.select->master_query_expression()->first_query_block();

  lex->pop_context();
  pc->select->n_child_sum_items += child->n_sum_items;

  /*
    A subquery (and all the subsequent query blocks in a UNION) can add
    columns to an outer query block. Reserve space for them.
  */
  for (Query_block *temp = child; temp != nullptr;
       temp = temp->next_query_block()) {
    pc->select->select_n_where_fields += temp->select_n_where_fields;
    pc->select->select_n_having_items += temp->select_n_having_items;
  }

  return false;
}

Sql_cmd *PT_create_srs::make_cmd(THD *thd) {
  // Note: This function hard-codes the maximum length of various
  // strings. These lengths must match those in
  // sql/dd/impl/tables/spatial_reference_systems.cc.

  thd->lex->sql_command = SQLCOM_CREATE_SRS;

  if (m_srid > std::numeric_limits<gis::srid_t>::max()) {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "SRID",
             m_or_replace ? "CREATE OR REPLACE SPATIAL REFERENCE SYSTEM"
                          : "CREATE SPATIAL REFERENCE SYSTEM");
    return nullptr;
  }
  if (m_srid == 0) {
    my_error(ER_CANT_MODIFY_SRID_0, MYF(0));
    return nullptr;
  }

  if (m_attributes.srs_name.str == nullptr) {
    my_error(ER_SRS_MISSING_MANDATORY_ATTRIBUTE, MYF(0), "NAME");
    return nullptr;
  }
  MYSQL_LEX_STRING srs_name_utf8 = {nullptr, 0};
  if (thd->convert_string(&srs_name_utf8, &my_charset_utf8_bin,
                          m_attributes.srs_name.str,
                          m_attributes.srs_name.length, thd->charset())) {
    /* purecov: begin inspected */
    my_error(ER_DA_OOM, MYF(0));
    return nullptr;
    /* purecov: end */
  }
  if (srs_name_utf8.length == 0 || std::isspace(srs_name_utf8.str[0]) ||
      std::isspace(srs_name_utf8.str[srs_name_utf8.length - 1])) {
    my_error(ER_SRS_NAME_CANT_BE_EMPTY_OR_WHITESPACE, MYF(0));
    return nullptr;
  }
  if (contains_control_char(srs_name_utf8.str, srs_name_utf8.length)) {
    my_error(ER_SRS_INVALID_CHARACTER_IN_ATTRIBUTE, MYF(0), "NAME");
    return nullptr;
  }
  String srs_name_str(srs_name_utf8.str, srs_name_utf8.length,
                      &my_charset_utf8_bin);
  if (srs_name_str.numchars() > 80) {
    my_error(ER_SRS_ATTRIBUTE_STRING_TOO_LONG, MYF(0), "NAME", 80);
    return nullptr;
  }

  if (m_attributes.definition.str == nullptr) {
    my_error(ER_SRS_MISSING_MANDATORY_ATTRIBUTE, MYF(0), "DEFINITION");
    return nullptr;
  }
  MYSQL_LEX_STRING definition_utf8 = {nullptr, 0};
  if (thd->convert_string(&definition_utf8, &my_charset_utf8_bin,
                          m_attributes.definition.str,
                          m_attributes.definition.length, thd->charset())) {
    /* purecov: begin inspected */
    my_error(ER_DA_OOM, MYF(0));
    return nullptr;
    /* purecov: end */
  }
  String definition_str(definition_utf8.str, definition_utf8.length,
                        &my_charset_utf8_bin);
  if (contains_control_char(definition_utf8.str, definition_utf8.length)) {
    my_error(ER_SRS_INVALID_CHARACTER_IN_ATTRIBUTE, MYF(0), "DEFINITION");
    return nullptr;
  }
  if (definition_str.numchars() > 4096) {
    my_error(ER_SRS_ATTRIBUTE_STRING_TOO_LONG, MYF(0), "DEFINITION", 4096);
    return nullptr;
  }

  MYSQL_LEX_STRING organization_utf8 = {nullptr, 0};
  if (m_attributes.organization.str != nullptr) {
    if (thd->convert_string(&organization_utf8, &my_charset_utf8_bin,
                            m_attributes.organization.str,
                            m_attributes.organization.length, thd->charset())) {
      /* purecov: begin inspected */
      my_error(ER_DA_OOM, MYF(0));
      return nullptr;
      /* purecov: end */
    }
    if (organization_utf8.length == 0 ||
        std::isspace(organization_utf8.str[0]) ||
        std::isspace(organization_utf8.str[organization_utf8.length - 1])) {
      my_error(ER_SRS_ORGANIZATION_CANT_BE_EMPTY_OR_WHITESPACE, MYF(0));
      return nullptr;
    }
    String organization_str(organization_utf8.str, organization_utf8.length,
                            &my_charset_utf8_bin);
    if (contains_control_char(organization_utf8.str,
                              organization_utf8.length)) {
      my_error(ER_SRS_INVALID_CHARACTER_IN_ATTRIBUTE, MYF(0), "ORGANIZATION");
      return nullptr;
    }
    if (organization_str.numchars() > 256) {
      my_error(ER_SRS_ATTRIBUTE_STRING_TOO_LONG, MYF(0), "ORGANIZATION", 256);
      return nullptr;
    }

    if (m_attributes.organization_coordsys_id >
        std::numeric_limits<gis::srid_t>::max()) {
      my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "IDENTIFIED BY",
               m_or_replace ? "CREATE OR REPLACE SPATIAL REFERENCE SYSTEM"
                            : "CREATE SPATIAL REFERENCE SYSTEM");
      return nullptr;
    }
  }

  MYSQL_LEX_STRING description_utf8 = {nullptr, 0};
  if (m_attributes.description.str != nullptr) {
    if (thd->convert_string(&description_utf8, &my_charset_utf8_bin,
                            m_attributes.description.str,
                            m_attributes.description.length, thd->charset())) {
      /* purecov: begin inspected */
      my_error(ER_DA_OOM, MYF(0));
      return nullptr;
      /* purecov: end */
    }
    String description_str(description_utf8.str, description_utf8.length,
                           &my_charset_utf8_bin);
    if (contains_control_char(description_utf8.str, description_utf8.length)) {
      my_error(ER_SRS_INVALID_CHARACTER_IN_ATTRIBUTE, MYF(0), "DESCRIPTION");
      return nullptr;
    }
    if (description_str.numchars() > 2048) {
      my_error(ER_SRS_ATTRIBUTE_STRING_TOO_LONG, MYF(0), "DESCRIPTION", 2048);
      return nullptr;
    }
  }

  sql_cmd.init(m_or_replace, m_if_not_exists, m_srid, srs_name_utf8,
               definition_utf8, organization_utf8,
               m_attributes.organization_coordsys_id, description_utf8);
  return &sql_cmd;
}

Sql_cmd *PT_drop_srs::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_DROP_SRS;

  if (m_srid > std::numeric_limits<gis::srid_t>::max()) {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "SRID",
             "DROP SPATIAL REFERENCE SYSTEM");
    return nullptr;
  }
  if (m_srid == 0) {
    my_error(ER_CANT_MODIFY_SRID_0, MYF(0));
    return nullptr;
  }

  return &sql_cmd;
}

Sql_cmd *PT_alter_instance::make_cmd(THD *thd) {
  thd->lex->no_write_to_binlog = false;
  return &sql_cmd;
}

bool PT_check_constraint::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc) ||
      cc_spec.check_expr->itemize(pc, &cc_spec.check_expr))
    return true;

  if (pc->alter_info->check_constraint_spec_list.push_back(&cc_spec))
    return true;

  pc->alter_info->flags |= Alter_info::ADD_CHECK_CONSTRAINT;
  return false;
}

Sql_cmd *PT_create_role::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_CREATE_ROLE;
  return &sql_cmd;
}

Sql_cmd *PT_drop_role::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_DROP_ROLE;
  return &sql_cmd;
}

Sql_cmd *PT_set_role::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_SET_ROLE;
  return &sql_cmd;
}

LEX_USER *PT_role_or_privilege::get_user(THD *thd) {
  thd->syntax_error_at(pos, "Illegal authorization identifier");
  return nullptr;
}

Privilege *PT_role_or_privilege::get_privilege(THD *thd) {
  thd->syntax_error_at(pos, "Illegal privilege identifier");
  return nullptr;
}

LEX_USER *PT_role_at_host::get_user(THD *thd) {
  return LEX_USER::alloc(thd, &role, &host);
}

LEX_USER *PT_role_or_dynamic_privilege::get_user(THD *thd) {
  return LEX_USER::alloc(thd, &ident, nullptr);
}

Privilege *PT_role_or_dynamic_privilege::get_privilege(THD *thd) {
  return new (thd->mem_root) Dynamic_privilege(ident, nullptr);
}

Privilege *PT_static_privilege::get_privilege(THD *thd) {
  return new (thd->mem_root) Static_privilege(grant, columns);
}

Privilege *PT_dynamic_privilege::get_privilege(THD *thd) {
  return new (thd->mem_root) Dynamic_privilege(ident, nullptr);
}

Sql_cmd *PT_grant_roles::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_GRANT_ROLE;

  List<LEX_USER> *role_objects = new (thd->mem_root) List<LEX_USER>;
  if (role_objects == nullptr) return nullptr;  // OOM
  for (PT_role_or_privilege *r : *roles) {
    LEX_USER *user = r->get_user(thd);
    if (r == nullptr || role_objects->push_back(user)) return nullptr;
  }

  return new (thd->mem_root)
      Sql_cmd_grant_roles(role_objects, users, with_admin_option);
}

Sql_cmd *PT_revoke_roles::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_REVOKE_ROLE;

  List<LEX_USER> *role_objects = new (thd->mem_root) List<LEX_USER>;
  if (role_objects == nullptr) return nullptr;  // OOM
  for (PT_role_or_privilege *r : *roles) {
    LEX_USER *user = r->get_user(thd);
    if (r == nullptr || role_objects->push_back(user)) return nullptr;
  }
  return new (thd->mem_root) Sql_cmd_revoke_roles(role_objects, users);
}

Sql_cmd *PT_alter_user_default_role::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_ALTER_USER_DEFAULT_ROLE;
  return &sql_cmd;
}

Sql_cmd *PT_show_grants::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_SHOW_GRANTS;
  return &sql_cmd;
}

bool PT_alter_table_action::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;
  pc->alter_info->flags |= flag;
  return false;
}

bool PT_alter_table_set_default::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc) || itemize_safe(pc, &m_expr)) return true;
  Alter_column *alter_column;
  if (m_expr == nullptr || m_expr->basic_const_item()) {
    Item *actual_expr = m_expr;
    if (m_expr && m_expr->type() == Item::FUNC_ITEM) {
      /*
        Default value should be literal => basic constants =>
        no need fix_fields()
       */
      Item_func *func = down_cast<Item_func *>(m_expr);
      if (func->result_type() != INT_RESULT) {
        my_error(ER_INVALID_DEFAULT, MYF(0), m_name);
        return true;
      }
      assert(dynamic_cast<Item_func_true *>(func) ||
             dynamic_cast<Item_func_false *>(func));
      actual_expr = new Item_int(func->val_int());
    }
    alter_column = new (pc->mem_root) Alter_column(m_name, actual_expr);
  } else {
    auto vg = new (pc->mem_root) Value_generator;
    if (vg == nullptr) return true;  // OOM
    vg->expr_item = m_expr;
    vg->set_field_stored(true);
    alter_column = new (pc->mem_root) Alter_column(m_name, vg);
  }
  if (alter_column == nullptr ||
      pc->alter_info->alter_list.push_back(alter_column)) {
    return true;  // OOM
  }
  return false;
}

bool PT_alter_table_order::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc) || m_order->contextualize(pc)) return true;
  pc->select->order_list = m_order->value;
  return false;
}

bool PT_alter_table_partition_by::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc) || m_partition->contextualize(pc)) return true;
  pc->thd->lex->part_info = &m_partition->part_info;
  return false;
}

bool PT_alter_table_add_partition::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *const lex = pc->thd->lex;
  lex->no_write_to_binlog = m_no_write_to_binlog;
  assert(lex->part_info == nullptr);
  lex->part_info = &m_part_info;
  return false;
}

bool PT_alter_table_drop_partition::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  assert(pc->alter_info->partition_names.is_empty());
  pc->alter_info->partition_names = m_partitions;
  return false;
}

bool PT_alter_table_rebuild_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;
  pc->thd->lex->no_write_to_binlog = m_no_write_to_binlog;
  return false;
}

bool PT_alter_table_optimize_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;
  pc->thd->lex->no_write_to_binlog = m_no_write_to_binlog;
  return false;
}

bool PT_alter_table_analyze_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;
  pc->thd->lex->no_write_to_binlog = m_no_write_to_binlog;
  return false;
}

bool PT_alter_table_check_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *const lex = pc->thd->lex;
  lex->check_opt.flags |= m_flags;
  lex->check_opt.sql_flags |= m_sql_flags;
  return false;
}

bool PT_alter_table_repair_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  LEX *const lex = pc->thd->lex;
  lex->no_write_to_binlog = m_no_write_to_binlog;

  lex->check_opt.flags |= m_flags;
  lex->check_opt.sql_flags |= m_sql_flags;

  return false;
}

bool PT_alter_table_coalesce_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;

  pc->thd->lex->no_write_to_binlog = m_no_write_to_binlog;
  pc->alter_info->num_parts = m_num_parts;
  return false;
}

bool PT_alter_table_truncate_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;
  return false;
}

bool PT_alter_table_reorganize_partition::contextualize(
    Table_ddl_parse_context *pc) {
  if (super::contextualize(pc)) return true;
  pc->thd->lex->part_info = &m_partition_info;
  pc->thd->lex->no_write_to_binlog = m_no_write_to_binlog;
  return false;
}

bool PT_preload_keys::contextualize(Table_ddl_parse_context *pc) {
  if (super::contextualize(pc) ||
      !pc->select->add_table_to_list(
          pc->thd, m_table, nullptr,
          m_ignore_leaves ? TL_OPTION_IGNORE_LEAVES : 0, TL_READ,
          MDL_SHARED_READ, m_opt_cache_key_list))
    return true;
  return false;
}

Alter_tablespace_parse_context::Alter_tablespace_parse_context(THD *thd)
    : thd(thd), mem_root(thd->mem_root) {}

bool PT_alter_tablespace_option_nodegroup::contextualize(
    Alter_tablespace_parse_context *pc) {
  if (super::contextualize(pc)) return true; /* purecov: inspected */  // OOM

  if (pc->nodegroup_id != UNDEF_NODEGROUP) {
    my_error(ER_FILEGROUP_OPTION_ONLY_ONCE, MYF(0), "NODEGROUP");
    return true;
  }
  pc->nodegroup_id = m_nodegroup_id;
  return false;
}

Sql_cmd *PT_create_resource_group::make_cmd(THD *thd) {
  if (check_resource_group_support()) return nullptr;

  if (check_resource_group_name_len(sql_cmd.m_name, Sql_condition::SL_ERROR))
    return nullptr;

  if (has_priority &&
      validate_resource_group_priority(thd, &sql_cmd.m_priority, sql_cmd.m_name,
                                       sql_cmd.m_type))
    return nullptr;

  for (auto &range : *sql_cmd.m_cpu_list) {
    if (validate_vcpu_range(range)) return nullptr;
  }

  thd->lex->sql_command = SQLCOM_CREATE_RESOURCE_GROUP;
  return &sql_cmd;
}

Sql_cmd *PT_alter_resource_group::make_cmd(THD *thd) {
  if (check_resource_group_support()) return nullptr;

  if (check_resource_group_name_len(sql_cmd.m_name, Sql_condition::SL_ERROR))
    return nullptr;

  for (auto &range : *sql_cmd.m_cpu_list) {
    if (validate_vcpu_range(range)) return nullptr;
  }

  thd->lex->sql_command = SQLCOM_ALTER_RESOURCE_GROUP;
  return &sql_cmd;
}

Sql_cmd *PT_drop_resource_group::make_cmd(THD *thd) {
  if (check_resource_group_support()) return nullptr;

  if (check_resource_group_name_len(sql_cmd.m_name, Sql_condition::SL_ERROR))
    return nullptr;

  thd->lex->sql_command = SQLCOM_DROP_RESOURCE_GROUP;
  return &sql_cmd;
}

Sql_cmd *PT_set_resource_group::make_cmd(THD *thd) {
  if (check_resource_group_support()) return nullptr;

  if (check_resource_group_name_len(sql_cmd.m_name, Sql_condition::SL_ERROR))
    return nullptr;

  thd->lex->sql_command = SQLCOM_SET_RESOURCE_GROUP;
  return &sql_cmd;
}

Sql_cmd *PT_restart_server::make_cmd(THD *thd) {
  thd->lex->sql_command = SQLCOM_RESTART_SERVER;
  return &sql_cmd;
}

/**
   Generic attribute node that can be used with different base types
   and corrsponding parse contexts. CFP (Contextualizer Function
   Pointer) argument implements a suitable contextualize action in the
   given context. Value is typically a decayed captureless lambda.
 */
template <class ATTRIBUTE, class BASE>
class PT_attribute : public BASE {
  ATTRIBUTE m_attr;
  using CFP = bool (*)(ATTRIBUTE, typename BASE::context_t *);
  CFP m_cfp;

 public:
  PT_attribute(ATTRIBUTE a, CFP cfp) : m_attr{a}, m_cfp{cfp} {}
  bool contextualize(typename BASE::context_t *pc) override {
    return BASE::contextualize(pc) || m_cfp(m_attr, pc);
  }
};

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_alter_tablespace_option_base* to PT_attribute object.
 */
PT_alter_tablespace_option_base *make_tablespace_engine_attribute(
    MEM_ROOT *mem_root, LEX_CSTRING attr) {
  return new (mem_root)
      PT_attribute<LEX_CSTRING, PT_alter_tablespace_option_base>(
          attr, +[](LEX_CSTRING a, Alter_tablespace_parse_context *pc) {
            pc->engine_attribute = a;
            return false;
          });
}

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_alter_tablespace_option_base* to PT_attribute object.

 */
PT_create_table_option *make_table_engine_attribute(MEM_ROOT *mem_root,
                                                    LEX_CSTRING attr) {
  return new (mem_root) PT_attribute<LEX_CSTRING, PT_create_table_option>(
      attr, +[](LEX_CSTRING a, Table_ddl_parse_context *pc) {
        pc->create_info->engine_attribute = a;
        pc->create_info->used_fields |= HA_CREATE_USED_ENGINE_ATTRIBUTE;
        pc->alter_info->flags |=
            (Alter_info::ALTER_OPTIONS | Alter_info::ANY_ENGINE_ATTRIBUTE);
        return false;
      });
}

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_create_table_option* to PT_attribute object.

 */
PT_create_table_option *make_table_secondary_engine_attribute(
    MEM_ROOT *mem_root, LEX_CSTRING attr) {
  return new (mem_root) PT_attribute<LEX_CSTRING, PT_create_table_option>(
      attr, +[](LEX_CSTRING a, Table_ddl_parse_context *pc) {
        pc->create_info->secondary_engine_attribute = a;
        pc->create_info->used_fields |=
            HA_CREATE_USED_SECONDARY_ENGINE_ATTRIBUTE;
        pc->alter_info->flags |= Alter_info::ALTER_OPTIONS;
        return false;
      });
}

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_create_table_option* to PT_attribute object.

 */
PT_column_attr_base *make_column_engine_attribute(MEM_ROOT *mem_root,
                                                  LEX_CSTRING attr) {
  return new (mem_root) PT_attribute<LEX_CSTRING, PT_column_attr_base>(
      attr, +[](LEX_CSTRING a, Column_parse_context *pc) {
        // Note that a std::function is created from the lambda and constructed
        // directly in the vector.
        // This means it is necessary to ensure that the elements of the vector
        // are destroyed. This will not happen automatically when the vector is
        // moved to the Alter_info struct which is allocated on the mem_root
        // and not destroyed.
        pc->cf_appliers.emplace_back([=](Create_field *cf, Alter_info *ai) {
          cf->m_engine_attribute = a;
          ai->flags |= Alter_info::ANY_ENGINE_ATTRIBUTE;
          return false;
        });
        return false;
      });
}

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_column_attr_base* to PT_attribute object.

 */
PT_column_attr_base *make_column_secondary_engine_attribute(MEM_ROOT *mem_root,
                                                            LEX_CSTRING attr) {
  return new (mem_root) PT_attribute<LEX_CSTRING, PT_column_attr_base>(
      attr, +[](LEX_CSTRING a, Column_parse_context *pc) {
        // Note that a std::function is created from the lambda and constructed
        // directly in the vector.
        // This means it is necessary to ensure that the elements of the vector
        // are destroyed. This will not happen automatically when the vector is
        // moved to the Alter_info struct which is allocated on the mem_root
        // and not destroyed.
        pc->cf_appliers.emplace_back([=](Create_field *cf, Alter_info *) {
          cf->m_secondary_engine_attribute = a;
          return false;
        });
        return false;
      });
}

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_base_index_option* to PT_attribute object.

 */
PT_base_index_option *make_index_engine_attribute(MEM_ROOT *mem_root,
                                                  LEX_CSTRING attr) {
  return new (mem_root) PT_attribute<LEX_CSTRING, PT_base_index_option>(
      attr, +[](LEX_CSTRING a, Table_ddl_parse_context *pc) {
        pc->key_create_info->m_engine_attribute = a;
        pc->alter_info->flags |= Alter_info::ANY_ENGINE_ATTRIBUTE;
        return false;
      });
}

/**
   Factory function which instantiates PT_attribute with suitable
   parameters, allocates on the provided mem_root, and returns the
   appropriate base pointer.

   @param mem_root Memory arena.
   @param attr     Attribute value from parser.

   @return PT_base_index_option* to PT_attribute object.
 */
PT_base_index_option *make_index_secondary_engine_attribute(MEM_ROOT *mem_root,
                                                            LEX_CSTRING attr) {
  return new (mem_root) PT_attribute<LEX_CSTRING, PT_base_index_option>(
      attr, +[](LEX_CSTRING a, Table_ddl_parse_context *pc) {
        pc->key_create_info->m_secondary_engine_attribute = a;
        return false;
      });
}