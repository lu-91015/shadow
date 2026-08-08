# -*- coding: utf-8 -*-
path = 'src/type_checker/type_checker.shadow'
s = open(path, encoding='utf-8').read()

# 1. tc_infer_bindings 改为使用 ctx 字段（去 bind_names/bind_types 参数）
old_fn = """pub kimo tc_infer_bindings(ctx: CompileContext, declared: Type, actual: Type, tparams: array<TypeParam>, bind_names: array<string>, bind_types: array<Type>) -> void {
    let dk = declared.kind;
    if (dk == TYPE_STRUCT || dk == TYPE_ENUM || dk == TYPE_ENUM_PAYLOAD) {
        // 裸类型参数
        if (tc_type_param_index(declared.name, tparams) >= 0) {
            if (tc_name_in_array(bind_names, declared.name) == 0) {
                bind_names = array_push(bind_names, declared.name);
                bind_types = array_push(bind_types, actual);
            }
            return;
        }
        // 命名类型带实参（泛型实例）→ 按位递归
        if (len(declared.param_types) > 0 && len(actual.param_types) > 0) {
            let i = 0;
            while (i < len(declared.param_types) && i < len(actual.param_types)) {
                tc_infer_bindings(ctx, declared.param_types[i], actual.param_types[i], tparams, bind_names, bind_types);
                i = i + 1;
            }
        }
        return;
    }
    if (dk == TYPE_ARRAY && actual.kind == TYPE_ARRAY) {
        tc_infer_bindings(ctx, declared.elem_type, actual.elem_type, tparams, bind_names, bind_types);
        return;
    }
    if (dk == TYPE_DICT && actual.kind == TYPE_DICT) {
        tc_infer_bindings(ctx, declared.key_type, actual.key_type, tparams, bind_names, bind_types);
        tc_infer_bindings(ctx, declared.val_type, actual.val_type, tparams, bind_names, bind_types);
        return;
    }
}"""
new_fn = """pub kimo tc_infer_bindings(ctx: CompileContext, declared: Type, actual: Type, tparams: array<TypeParam>) -> void {
    let dk = declared.kind;
    if (dk == TYPE_STRUCT || dk == TYPE_ENUM || dk == TYPE_ENUM_PAYLOAD) {
        // 裸类型参数
        if (tc_type_param_index(declared.name, tparams) >= 0) {
            if (tc_name_in_array(ctx.bind_names, declared.name) == 0) {
                ctx.bind_names = array_push(ctx.bind_names, declared.name);
                ctx.bind_types = array_push(ctx.bind_types, actual);
            }
            return;
        }
        // 命名类型带实参（泛型实例）→ 按位递归
        if (len(declared.param_types) > 0 && len(actual.param_types) > 0) {
            let i = 0;
            while (i < len(declared.param_types) && i < len(actual.param_types)) {
                tc_infer_bindings(ctx, declared.param_types[i], actual.param_types[i], tparams);
                i = i + 1;
            }
        }
        return;
    }
    if (dk == TYPE_ARRAY && actual.kind == TYPE_ARRAY) {
        tc_infer_bindings(ctx, declared.elem_type, actual.elem_type, tparams);
        return;
    }
    if (dk == TYPE_DICT && actual.kind == TYPE_DICT) {
        tc_infer_bindings(ctx, declared.key_type, actual.key_type, tparams);
        tc_infer_bindings(ctx, declared.val_type, actual.val_type, tparams);
        return;
    }
}"""
assert old_fn in s, "infer_bindings not found"
s = s.replace(old_fn, new_fn)

# 2. tc_check_call_fd 初始化 + 调用
old_call = """    let tparams = fd.type_params;
    let bind_names: array<string> = [];
    let bind_types: array<Type> = [];
    if (len(tparams) > 0) {
        // 从实参位置推断绑定（递归 unify：形参 Pair<A,B> 实参 Pair__T_int_string）
        let pi = 0;
        while (pi < nparams) {
            let p = params[pi] as Param;
            if (pi < nargs) {
                let arg_t = tc_check_expr(ctx, args[pi], env);
                tc_infer_bindings(ctx, p.type_ref, arg_t, tparams, bind_names, bind_types);
            }
            pi = pi + 1;
        }"""
new_call = """    let tparams = fd.type_params;
    ctx.bind_names = [];
    ctx.bind_types = [];
    if (len(tparams) > 0) {
        // 从实参位置推断绑定（递归 unify：形参 Pair<A,B> 实参 Pair__T_int_string）
        let pi = 0;
        while (pi < nparams) {
            let p = params[pi] as Param;
            if (pi < nargs) {
                let arg_t = tc_check_expr(ctx, args[pi], env);
                tc_infer_bindings(ctx, p.type_ref, arg_t, tparams);
            }
            pi = pi + 1;
        }"""
assert old_call in s, "call fd init not found"
s = s.replace(old_call, new_call)

# 3. 约束检查段
s = s.replace('while (i < len(bind_names)) {\n                let bn = bind_names[i] as string;',
              'while (i < len(ctx.bind_names)) {\n                let bn = ctx.bind_names[i] as string;')
s = s.replace('let bt2 = bind_types[i] as Type;', 'let bt2 = ctx.bind_types[i] as Type;')

# 4. 参数检查与返回类型替换
s = s.replace('pty = tc_mono_type(pty, tparams, bind_names, bind_types);', 'pty = tc_mono_type(pty, tparams, ctx.bind_names, ctx.bind_types);')
s = s.replace('rt = tc_mono_type(rt, tparams, bind_names, bind_types);', 'rt = tc_mono_type(rt, tparams, ctx.bind_names, ctx.bind_types);')
s = s.replace('let mono_name = tc_mono_instantiate(ctx, fd, tparams, bind_names, bind_types);', 'let mono_name = tc_mono_instantiate(ctx, fd, tparams, ctx.bind_names, ctx.bind_types);')

open(path, 'w', encoding='utf-8', newline='').write(s)
print('bind refactor done')
# 残留检查
import re
left = [l for l in s.split('\n') if 'bind_names' in l and 'ctx.bind' not in l and 'mono_' not in l and 'struct_mono' not in l]
for l in left[:8]:
    print('LEFT:', l.strip())
