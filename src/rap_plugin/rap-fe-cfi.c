/* Write by David fuqiang Fan <feqin1023@gmail.com>, member of HardenedLinux.
   The code of this file try to make some optimizationsfor  PaX RAP.
   Supply the API for RAP.

   And we also call function wich compute function type hash from PaX RAP.
   Code architecture inspired by RAP of PaX Team <pageexec@freemail.hu>.

   Licensed under the GPL v2. */

#include <stdio.h>
#include <string.h>
#include "rap.h"
#include "tree-pass.h"
//#include "../include/pointer-set.h"
 
/* There are many optimization methrod can do for RAP.
   From simple to complex and aside with the gcc internal work stage.

   Maybe the most efficent methrod is add gcc intenal library and require to merge.
   Because of the lack of analysis for function pointer.

   Wrok with gcc plugin we have more isolate and also indepened with gcc version
   methrod, but need more replicate data 

   1, If a function pointer variable has not external linkage and live at code 
      generation, it's legal function set must be only just the current file 
      scope avail function and the proper type.
   
   2, Global alias anylysis for the avail function set. */

/* This data structure defined in gcc source. */
//extern struct simple_ipa_opt_pass pass_ipa_pta;

/* Contains the beed called optimization level of GCC */
int gcc_optimize_level = 0;
/* Count how many function we have optimized */
int rap_opt_statistics_data = 0;
/* Contain the statistics data, maybe gcc will called many times, we need output
   data in the same file, for the conventices of scripts process. */
const char *dump_rap_opt_statistics_filename = "rap_opt_statistics_dump";
FILE *dump_rap_opt_statistics_fd;
// Hold all the ROP target functions.
static bitmap sensi_funcs;
/* Contains the type database which are pointer analysis can not sloved */
static struct pointer_set_t *pointer_types;
//
static bool will_call_ipa_pta;
/* For compatiable with the original RAP */
typedef rap_hash_t.hash rap_hash_value_type;

/* Test GCC will call some passes which is benefit. */
void 
rap_check_will_call_passes (void* gcc_data, void* user_data) 
{
  //gcc_assert (current_pass);
  if (current_pass 
      && 
      (! strcmp (((struct opt_pass *)current_pass)->name, "inline")))
    {
      if (*(bool*)gcc_data)
	fprintf(dump_rap_opt_statistics_fd, "[+] NOT call pass 'inline'\n");
    }

  return;
}

/* Try make GCC call ipa-pta pass if optimization level is NOT 0 */
void 
rap_try_call_ipa_pta (void* gcc_data, void* user_data) 
{
  /* Make sure we have reach */
  bool will_call_ipa_pta = false;

  //gcc_assert (current_pass);
  if (current_pass 
      && 
      (! strcmp (((struct opt_pass *)current_pass)->name, "pta")))
    {
      *(bool*)gcc_data = true;
      /* The variable optimize is defined int GCC */
      *(int*)user_data = optimize;
    }
  //gcc_assert (init);

  return;
}

/* Tools for type database operates */
static void
insert_type_db (tree t)
{
  tree ty = TREE_TYPE (t);

  if (! pointer_types)
    pointer_types = pointer_set_create ();

  gcc_assert (FUNCTION_POINTER_TYPE_P (t));
  pointer_set_insert (pointer_types, (const void *)ty);

  return;
}


/* If after alias analysis some function pointer may be point anything, we have
   to make the conservation solution, contain and cache the the point to type,
   when emit RAP guard code, make sure all the functions of the compatible type
   NOT igonred and optimized 
   Return value is trivial */
static bool
rap_base_type_db_fliter (const void *db_type, void *fn)
{
  tree f = (tree) fn;
  gcc_assert (TREE_CODE (f) == FUNCTION_DECL);
  if (types_compatible_p ((tree)db_type, TREE_TYPE(f)))
    if (bitmap_clear_bit (sensi_funcs, DECL_UID(f)))
      return true;

  return true;
}

/* The real worker */
static void
rap_gather_function_targets_1 (tree t)
{
  //types_compatible_p()
  //TREE_TYPE()
  //FUNCTION_POINTER_TYPE_P()
  struct ptr_info_def *pi;
  bitmap set;
  pi = SSA_NAME_PTR_INFO (t);
  if (pi)
    {
      if (pi->pt.anything)
        /* we are in trouble, pointer analysis can not give any answer about 
           this pointer point to */
        insert_type_db (t);
    }
  else
    {
      set = pi->pt.vars;
      if (! bitmap_empty_p (set))
        bitmap_ior_into (sensi_funcs, set);
    }

  return;
}

/* Entry point of build the oracle, gather what functions never may be 
   indirected called */
void 
rap_gather_function_targets ()
{
  struct cgraph_node *node;
  struct varpool_node *var;
  tree t;
  struct function *func;
  unsigned int i;

  /* This optimization depend on GCC optimizations */
  if (0 == gcc_optimize_level)
    return;
  // 
  gcc_assert (will_call_ipa_pta);
  bitmap sensi_funcs = BITMAP_ALLOC (NULL);

  /* Gather function pointer infos from global may available variable */
  FOR_EACH_VARIABLE (var)
    {
      if (var->alias)
	continue;
      gcc_assert (t = var->symbol.decl);
      /* We only care about function pointer variables */
      if (! FUNCTION_POINTER_TYPE_P (t))
	continue;
      rap_gather_function_targets_1 (t);
    }
  /* Gather function pointer infos from every function */
  FOR_EACH_DEFINED_FUNCTION (node)
    {
      t = NULL;

      /* Nodes without a body are not interesting.  */
      if (!cgraph_function_with_gimple_body_p (node))
	continue;
      func = DECL_STRUCT_FUNCTION (node->symbol.decl);
      //push_cfun (func);
      /* Function pointers will be SSA_NAME contained in current function, 
        When gcc after pointer analysis we gather all the functions may be 
        pointed by some function pointer and we ignore which function pointer
        can access it. All this gathered function are the sensitive data, need
        RAP add guard instruction. */
      FOR_EACH_VEC_ELT (*func->gimple_df->ssa_names, i, t)
	{
	  if (! (t || FUNCTION_POINTER_TYPE_P (t)))
	    continue;
	  rap_gather_function_targets_1 (t);
        }
    } // FOR_EACH_DEFINED_FUNCTION (node)
  
  /* We have see all of the data about alias and build the type database, It's 
     time for the final fliter */
  if (! pointer_types)
    return;
  /* fial fliter */
  FOR_EACH_DEFINED_FUNCTION (node)
    {
      t = NULL;

      /* Nodes without a body are not interesting.  */
      if (!cgraph_function_with_gimple_body_p (node))
	continue;
      func = DECL_STRUCT_FUNCTION (node->symbol.decl);
      pointer_set_traverse (pointer_types, 
		            rap_base_type_db_fliter,
		            func);
    }

  return;
} // end of rap_gather_function_targets


/* Basic test of function nature */
static inline bool 
is_rap_function_may_be_aliased (tree f)
{
  return (TREE_CODE (f) != CONST_DECL
	  && !((TREE_STATIC (f) || TREE_PUBLIC (f) || DECL_EXTERNAL (f))
	       && TREE_READONLY (f)
	       && !TYPE_NEEDS_CONSTRUCTING (TREE_TYPE (f)))
	  && (TREE_PUBLIC (f)
	      || DECL_EXTERNAL (f)
	      /*|| TREE_ADDRESSABLE (f)*/ ));
}

/* Entry point of the oracle, look up current function weather or not beed
   gathered into our target function set. If YES return 1 otherwise return 0 */
int 
is_rap_function_maybe_roped (tree f)
{
  if (! is_rap_function_may_be_aliased (f))
    return 0;
  /* Ask the oracle for help */
  if (0 == gcc_optimize_level)
    /* Function is_rap_function_may_be_aliased() must be failed we arive here, 
       but our oracle dependent the GCC optimizations. */
    return 1;
  else
    return bitmap_bit_p (sensi_funcs, DECL_UID (f));
}

/* Write some statistics for our algorithm */
void
dump_rap_opt_statistics ()
{
  dump_rap_opt_statistics_fd = fopen (dump_rap_opt_statistics_filename, "a");
  fprintf (dump_rap_opt_statistics_fd, "%d\n", rap_opt_statistics_data);

  return;
}

/* Clean and cloese function for optimizations */
void
rap_optimization_clean ()
{
  /* Now we have finish our job, close file and destroy the GCC allocated data*/
  fclose (dump_rap_opt_statistics_fd);
  bitmap_clear (sensi_funcs);
  pointer_set_destroy (pointer_types);

  return;
}

/* Type definition for hash value maps. */
struct cfi_function_hash_maps_t
{
  struct pointer_map_t *map;
  /* Implementate a circle list. */
  unsigned total;
#if 0
#define HL_CFI_MAP_CACHE_SIZE 3
  // FIFO cache.
  struct cfi_function_hash_pair_t
  {
    tree type;
    rap_hash_value_type val;
  } cfi_cache [HL_CFI_MAP_CACHE_SIZE];
#endif
};

/* Constains the fucntion type and hash value maps. */
static struct cfi_function_hash_maps_t cfi_db;

/* Temp type, used for access cfi_db. */
struct cfi_key_value_t
 {
   tree type;
   rap_hash_value_type val;
 };

/* Hook fed to pointer_map_traverse, type compatiablity test. 
   If find return TRUE, otherwise FALSE. */
bool
pointer_map_access (const void *key, void **val, void *type)
{
  struct cfi_key_value_t contain;

  contain = (cfi_key_value_t *)type;
  if (types_compatible_p ((tree)key, contain.type))
  {
    contain.val = (rap_hash_value_type)**val;
    return true;
  }

  return false;
}

/* Search the [function type : hash value] table, if not have compatiable 
   type match, create one and insert into the table. */
static rap_hash_value_type
find_or_create_cfi_hash_val (tree type)
{
  int i;
  rap_hash_value_type val;
  void **val_address;

  struct key_value
   {
     tree type;
     rap_hash_value_type val;
   } contain;

  gcc_assert (TREE_CODE (type) == FUNCTION_TYPE);
  //struct pointer_map_t *cfi_function_hash_maps;
  if (! cfi_db.map)
    cfi_db.map = pointer_map_create ();
  
  /* Search */
  contain = {type, };
  if (0 == cfi_db.total)
    goto create;
  if (pointer_set_traverse(cfi_db.map, pointer_map_access, (void *)&contain))
    return contain.val;

  /* Fall through. update db. */
create:
  val = (rap_hash_value_type)
	((rap_hash_function_type (type, imprecise_rap_hash_flags)).hash);
  val_address = pointer_map_insert (cfi_db.map, (void *)type);
  *val_address = (void *)val;
  ++cfi_db.total;
  return val;
}

#define BUILD_SOURCE_HASH_TREE 1
#define BUILD_TARGET_HASH_TREE 2

/* Build cfi hash tree, target or source depend on the argument.
   ??? should we reuse the tree node. */
// set_dom_info_availability (enum cdi_direction dir, enum dom_state new_state)
//free_dominance_info (enum cdi_direction dir)

static tree
build_cfi_hash_tree (gimple cs, int direct, tree *target_off_type_p)
{
  //tree hash_tree, var;
  rap_hash_value_type val;
  tree decl, func_type;

  gcc_assert(is_gimple_call (cs));
  
  decl = gimple_call_fn (cs);
  func_type = TREE_TYPE (TREE_TYPE (decl));
  // safe guard 
  gcc_assert (TREE_CODE (decl) == SSA_NAME);
  gcc_assert (TREE_CODE (type) == FUNCTION_TYPE);
  
  // create source hash tree.
  if (direct == BUILD_SOURCE_HASH_TREE)
    {
      val = find_or_create_cfi_hash_val (func_type);
      return build_int_cst(integer_type_node, val);
    }
  else
    {
      //tree off_type;
      int target_offset;

      gcc_assert (direct == BUILD_TARGET_HASH_TREE);
      gcc_assert (target_off_type_p);
      
      //target = create_tmp_var (integer_type_node, "hl_target");
      //target = make_ssa_name (var, NULL);

      /* This code fragment of compute target function hash offset comes
	 from Pax RAP. */
      /* We need the forward offset. */
      if (UNITS_PER_WORD == 8)
        {
	  target_offset = - 2 * sizeof(rap_hash_value_type);
	  *target_off_type_p = long_integer_type_node;
	}
      else if (UNITS_PER_WORD == 4)
        {
	  target_offset = - sizeof(rap_hash_value_type);
	  *target_off_type_p = integer_type_node;
        }
      else
	gcc_unreachable();

      /* Build the tree for : ((rap_hash_value_type *)target_function - 1) 
         This code is referenced from gcc source: gimplify_modify_expr_rhs() */

      // integer_ptr_type_node
      // func is the function pointer, ADDR_EXPR, pointer(function)
      gcc_assert (FUNCTION_POINTER_TYPE_P ( TREE_TYPE (decl)));
      // type is the result type of cast.
      return fold_build2 (MEM_REF, *target_off_type_p, decl,
		   // This is a pointer type tree reprensent the offset.
		   build_int_cst_wide (integer_ptr_type_node,
				       TREE_INT_CST_LOW (target_offset),
				       TREE_INT_CST_HIGH (target_offset)));

    }
  gcc_assert (0);
}

// linux kernel function.
extern void panic (const char *fmt, ...);

/* Help function called when the fe-cfi violate catched. */
void hl_fe_cfi_catch_tree ()
{
  tree catch;
  // TODO, change this to a gcc tree structure;
  panic ("[!] HardenedLinux fe-cfi violate catched.");

  return catch;
}


/* Insert branch and create two blcok contain original function call and our
   catch code. And also need complete the control flow graph.
     +-------
     stmt1;
     call fptr;
     +-------
     change to =>
     +-------
     stmt1;
     lhs = t_;
     ne_expr (lhs, s_);
     // true
     goto call_label;
     // FALLTHRU
     cfi_catch();
   call_label:
     call fptr;
     +--------

   The value of argument s_ is a const integer tree, 
   t_ is a MEM-REF, may need insert temp varibale as lhs get the value. */
static void
insert_cond_and_build_ssa_cfg (gimple_stmt_iterator *gp, 
		               tree s_, 
			       tree t_, 
			       tree t_t)
{
  gimple cs, g;
  gimple_stmt_iterator gsi;
  gimple branch;  // test & branch gimple we insert.
  gimple catch;   // catch function we insert.
  gimple call;    // call label gimple we insert.
  tree lhs, label;

  gsi = *gp;
  cs = gsi_stmt (gsi);
  gcc_assert (is_gimple_call (cs));

  /* Insert gimpls. */
  /* lhs = t_ */
  lhs = create_tmp_var (t_t, "hl_cfi_hash");
  //target = make_ssa_name (var, NULL);
  g = gimple_build_assign (lhs, t_);
  gsi_insert_before (&gsi, g, GSI_SAME_STMT);
  // if (lhs != s_) goto cfi_catch else goto call
  branch = gimple_build_cond (NE_EXPR, lhs, s_, NULL, NULL);
  gsi_insert_before (&gsi, branch, GSI_SAME_STMT);
  
  /* catch function */
  //hl_fe_cfi_catch ();
  catch = gimple_build_call (hl_fe_cfi_catch_tree (), 0);
  gsi_insert_before (&gsi, catch, GSI_SAME_STMT);
  
  /* call_label: */
  label = create_artificial_label (gimple_location (cs));
  call = gimple_build_label (label);
  gsi_insert_before (&gsi, call, GSI_SAME_STMT);
  // current statement should be original call.
  gcc_assert (is_gimple_call (gsi_stmt (gsi)));
  
  // guard test.
  GIMPLE_CHECK (branch, GIMPLE_COND);
  GIMPLE_CHECK (catch, GIMPLE_LABEL);
  GIMPLE_CHECK (call, GIMPLE_LABEL);

  /* Make the blocks. */



  /* Build the edges. */


  return;
}


/* Build the check statement: 
   if ((int *)(cs->target_function - sizeof(rap_hash_value_type)) != hash) 
     catch () */
static void
build_fe_cfi (gimple_stmt_iterator *gp)
{
  gimple cs;
  tree th;  // hash get behind the function definitions.
  tree sh;  // hash get before indirect calls.
  tree target_off_type;

  cs = gsi_stmt (gsi);
  gcc_assert (is_gimple_call (cs));
  decl = gimple_call_fn (cs);
  /* We must be indirect call */
  gcc_assert (TREE_CODE (decl) == SSA_NAME);
  gcc_assert (TREE_TYPE (TREE_TYPE (decl)) == cs->gimple_call.u.fntype);
  
  /* build source hash tree */
  sh = build_cfi_hash_tree (cs, BUILD_SOURCE_HASH_TREE, NULL);
  /* build target hash tree */
  th = build_cfi_hash_tree (cs, BUILD_TARGET_HASH_TREE, &target_off_type);

  /* Build the condition expression and insert into the code block, because
     the conditional import new branch, so we also need update the blocks */
  insert_cond_and_build_ssa_cfg (gp, sh, th, target_off_type);

  return;
}

// types_compatible_p
/*

cgraph_local_node_p 

types_compatible_p
flag_ltrans
flag_lto


*/

/* This new pass will be added after the GCC pass ipa "pta". */
static unsigned int
rap_fe_cfi_execute ()
{
  struct cgraph_node *node;
  //if (! flag_ltrans)
    //gcc_assert(0);
  //struct pointer_map_t *indirect_function_maps;
  
  FOR_EACH_DEFINED_FUNCTION (node)
    {
      struct function *func;
      basic_block bb;
      
      /* Without body not our intent. */
      if (!cgraph_function_with_gimple_body_p (node))
	continue;

      func = DECL_STRUCT_FUNCTION (node->symbol.decl);
      push_cfun (func);
      
      FOR_EACH_BB_FN (bb, func)
	{
	  gimple_stmt_iterator gsi;

	  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	    {
	      //tree decl;
	      gimple cs;
	      //tree hash;
	      cs = gsi_stmt (gsi);
	      /* We are in forward cfi only cares about function call */
	      if (! is_gimple_call (cs))
		continue;
	      /* Indirect calls */
	      if (NULL == gimple_call_fndecl (cs))
	        {
		  //decl = gimple_call_fn (cs);
		  //hash = find_cfi_hash_tree (decl);
		  //gcc_assert (hash);
		  build_fe_cfi (&gsi);
	        }
	    }
	}

      // Force the dominate info of current function recompute.
      free_dominance_info (CDI_DOMINATORS);
      free_dominance_info (CDI_POST_DOMINATORS);

      pop_cfun ();
    }
  //indirect_function_maps = pointer_map_create (); 
  //gimple_call_fndecl (const_gimple gs)

  return 0;
}

/* Genetate the pass structure */
#define PASS_NAME rap_fe_cfi
//#define PROPERTIES_REQUIRED PROP_gimple_any
//#define PROPERTIES_PROVIDED PROP_gimple_lcf
#define TODO_FLAGS_FINISH TODO_update_ssa_any | TODO_verify_all | TODO_dump_func | \
	TODO_remove_unused_locals | TODO_cleanup_cfg | TODO_rebuild_cgraph_edges
#include "gcc-generate-simple_ipa-pass.h"

