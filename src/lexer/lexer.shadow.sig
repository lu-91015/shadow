# hash: -477911584966663383
module|lexer
fn|lx_ctab_init|1||0|0|void||
fn|lx_chr|1||2|0|string|b:int,idx:int|
fn|lx_cur|1||0|0|string||
fn|lx_peek|1||1|0|string|n:int|
fn|lx_adv|1||0|0|void||
fn|lx_make_tok|1||2|0|Token|kind:int,text:string|
fn|lx_skip_ws|1||0|0|void||
fn|lx_read_num|1||0|0|Token||
fn|lx_read_id|1||0|0|Token||
fn|lx_read_str|1||0|0|Token||
fn|lx_read_char|1||0|0|Token||
fn|tokenize|1||2|0|Token[]|source:string,file:string|
fn|tokenize_with_offset|1||3|0|Token[]|source:string,file:string,line_offset:int|
