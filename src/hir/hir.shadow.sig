# hash: -1613473149242308591
module|hir
fn|hir_env_new|1||0|0|HirEnv||
fn|hir_env_lookup|1||2|0|int|env:HirEnv,name:string|
fn|hir_env_bind|1||2|0|int|env:HirEnv,name:string|
fn|hir_mk_expr|1||1|0|HirExpr|kind:int|
fn|hir_mk_stmt|1||1|0|HirStmt|kind:int|
fn|hir_type_of_expr|1||1|0|Type|e:Expr|
fn|hir_type_of_stmt|1||1|0|Type|s:Stmt|
fn|hir_lower_expr|1||2|0|HirExpr|e:Expr,env:HirEnv|
fn|hir_type_of_hir_expr|1||1|0|Type|h:HirExpr|
fn|hir_lower_stmt|1||2|0|HirStmt|s:Stmt,env:HirEnv|
fn|hir_lower_func|1||1|0|HirFunc|fd:FuncDef|
fn|hir_lower_program|1||1|0|HirFunc[]|prog:Program|
