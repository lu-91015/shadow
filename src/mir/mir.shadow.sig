# hash: -2729346801441848614
module|mir
fn|mir_has_terminator|1||0|0|int||
fn|mir_new_reg|1||0|0|int||
fn|mir_new_label|1||0|0|string||
fn|mir_mk|1||1|0|MirInst|op:int|
fn|mir_type_of_hir_expr|1||1|0|Type|h:HirExpr|
fn|mir_seal_and_begin|1||1|0|void|new_label:string|
fn|mir_seal_last|1||0|0|void||
fn|mir_emit|1||1|0|void|inst:MirInst|
fn|mir_set_term|1||1|0|void|inst:MirInst|
fn|mir_emit_br|1||1|0|void|target:string|
fn|mir_push_loop|1||2|0|void|brk:string,cont:string|
fn|mir_pop_loop|1||0|0|void||
fn|mir_lower_expr|1||1|0|int|h:HirExpr|
fn|mir_lower_stmt|1||1|0|void|h:HirStmt|
fn|mir_lower_func|1||1|0|MirFunc|hf:HirFunc|
fn|mir_lower_program|1||1|0|MirFunc[]|hfuncs:HirFunc[]|
