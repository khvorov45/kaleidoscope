#define GB_IMPLEMENTATION
#define GB_STATIC
#include "gb.h"

#include "llvm-c/Core.h"
#include "llvm-c/Analysis.h"


#if defined(GB_COMPILER_MSVC)
#pragma warning(disable:4201) // nonstandard extension used: nameless struct/union
#pragma warning(disable:4204) // nonstandard extension used: non-constant aggregate initializer
#pragma warning(disable:4820) // padding
#pragma warning(disable:4255) // no function prototype given: converting '()' to '(void)'
#pragma warning(disable:5045) // Compiler will insert Spectre mitigation for memory load if /Qspectre switch specified
#pragma warning(disable:4100) // unreferenced formal parameter
#endif


typedef struct String {
	char *ptr;
	isize len; // NOTE(khvorov) Does not include the null terminator
} String;


typedef enum TokenKind {
	TokenType_EOF,

	TokenType_Def,
	TokenType_Extern,

	TokenType_Identifier,
	TokenType_Number,

	TokenType_Ascii,
} TokenKind;

typedef struct Token {
	TokenKind type;
	String identifier;
	f64 number;
	char ascii;
} Token;


typedef struct AstNumber {
	f64 val;
} AstNumber;

typedef struct AstVariable {
	String name;
} AstVariable;

typedef struct AstParameter {
	String name;
} AstParameter;

typedef struct AstArgument {
	struct AstNode *val;
	struct AstArgument *next;
} AstArgument;

typedef struct AstBinary {
	char op;
	struct AstNode *lhs;
	struct AstNode *rhs;
} AstBinary;

typedef struct AstCall {
	String callee;
	AstArgument *args;
	isize arg_count;
} AstCall;

typedef struct AstPrototype {
	String name;
	AstParameter *param;
	isize param_count;
} AstPrototype;

typedef struct AstFunction {
	AstPrototype *proto;
	struct AstNode *body;
} AstFunction;

typedef enum AstType {
	AstType_None,
	AstType_Number,
	AstType_Variable,
	AstType_Binary,
	AstType_Call,
} AstType;

typedef struct AstNode {
	AstType type;
	union {
		AstNumber number;
		AstVariable variable;
		AstBinary binary;
		AstCall call;
	};
} AstNode;

typedef struct AstParser {
	gbArena arena;
	gbAllocator arena_allocator;
	Token *token;
	isize token_count;
} AstParser;


typedef struct LLVMBackend {
	LLVMContextRef ctx;
	LLVMBuilderRef builder;
	LLVMModuleRef module;
	gbHashTable named_values;
	gbArena arena;
	gbAllocator arena_allocator;
} LLVMBackend;


static String
string_from_cstring(char *ptr) {
	isize len = 0;
	for (isize index = 0; ; index += 1) {
		char ch = ptr[index];
		if (ch == '\0') {
			break;
		} else {
			len += 1;
		}
	}
	String result = { ptr, len };
	return result;
}

static char *
string_to_cstring(String *str, gbAllocator allocator) {
	char *result = gb_alloc(allocator, (str->len + 1) * sizeof(char));
	gb_memcopy(result, str->ptr, str->len);
	result[str->len] = '\0';
	return result;
}

static void
string_print(String *str) {
	for (isize index = 0; index < str->len; index += 1) {
		gb_printf("%c", str->ptr[index]);
	}
}

static void
string_offset(String *str, isize offset, String *pre) {
	GB_ASSERT(str->len >= offset);
	if (pre != 0) {
		pre->ptr = str->ptr;
		pre->len = offset;
	}
	str->ptr += offset;
	str->len -= offset;
}

static isize
string_index_nonalphanum(String *str) {
	isize result = 0;
	while (result < str->len) {
		char ch = str->ptr[result];
		if (!gb_char_is_alphanumeric(ch)) {
			break;
		}
		result += 1;
	}
	return result;
}

static isize
string_index_nonfloat(String *str) {
	isize result = 0;
	while (result < str->len) {
		char ch = str->ptr[result];
		if (!gb_char_is_digit(ch) && ch != '.') {
			break;
		}
		result += 1;
	}
	return result;
}

static b32
string_cmp_cstring(String *str, char *cstring) {
	b32 result = true;
	isize index = 0;
	while (index < str->len) {
		char c1 = str->ptr[index];
		char c2 = cstring[index];
		if (c2 == '\0') {
			break;
		}
		if (c2 != c1) {
			result = false;
			break;
		}
		index += 1;
	}
	return result;
}

static void
string_offset_to_next_line(String *str) {
	for (isize index = 0; index < str->len; index += 1) {
		char ch = str->ptr[index];
		if (ch == '\n' || ch == '\r') {
			isize to_skip = index + 1;
			if (ch == '\r' && to_skip < str->len && str->ptr[to_skip] == '\n') {
				to_skip += 1;
			}
			string_offset(str, to_skip, 0);
			break;
		}
	}
}

static u64
string_hash(String *str) {
	u64 result = gb_murmur64(str->ptr, str->len);
	return result;
}

static b32
string_cmp(String *str1, String *str2) {
	b32 result = false;
	if (str1->len == str2->len) {
		result = true;
		for (isize index = 0; index < str1->len; index += 1) {
			char ch1 = str1->ptr[index];
			char ch2 = str2->ptr[index];
			if (ch1 != ch2) {
				result = false;
				break;
			}
		}
	}
	return result;
}


static Token
get_token(String *input) {
	Token result = { 0 };

	// NOTE(khvorov) Skip spaces and comments
	b32 skipped = true;
	while (skipped) {
		skipped = false;
		while ((input->len > 0) && gb_char_is_space(input->ptr[0])) {
			string_offset(input, 1, 0);
			skipped = true;
		}
		while ((input->len > 0) && input->ptr[0] == '#') {
			string_offset_to_next_line(input);
			skipped = true;
		}
	}

	if (input->len > 0) {
		if (gb_char_is_alpha(input->ptr[0])) {

			isize identifier_end = string_index_nonalphanum(input);
			string_offset(input, identifier_end, &result.identifier);

			if (string_cmp_cstring(&result.identifier, "def")) {
				result.type = TokenType_Def;
			} else if (string_cmp_cstring(&result.identifier, "extern")) {
				result.type = TokenType_Extern;
			} else {
				result.type = TokenType_Identifier;
			}

		} else if (gb_char_is_digit(input->ptr[0])) {

			isize number_end = string_index_nonfloat(input);
			result.number = strtod(input->ptr, 0);
			string_offset(input, number_end, &result.identifier);

			result.type = TokenType_Number;

		} else {
			result.type = TokenType_Ascii;
			result.ascii = input->ptr[0];
			string_offset(input, 1, 0);
		}
	}

	return result;
}

static i32
get_cur_tok_precedence(AstParser *parser) {
	i32 result = -1;

	if (parser->token_count > 0 && parser->token->type == TokenType_Ascii) {
		switch (parser->token->ascii) {
		case '<': { result = 10; } break;
		case '+': { result = 20; } break;
		case '-': { result = 20; } break;
		case '*': { result = 40; } break;
		}
	}

	return result;
}


static void
parser_advance(AstParser *parser) {
	GB_ASSERT(parser->token_count > 0);
	parser->token += 1;
	parser->token_count -= 1;
}

static AstNode *
parse_number(AstParser *parser) {
	Token *token = parser->token;
	GB_ASSERT(token->type == TokenType_Number);
	AstNode *num_node = gb_alloc_item(parser->arena_allocator, AstNode);
	num_node->type = AstType_Number;
	num_node->number.val = token->number;
	parser_advance(parser);
	return num_node;
}

static AstNode *parse_primary(AstParser *parser);
static AstNode *parse_binop_rhs(AstParser *parser, i32 precedence, AstNode *lhs);

static AstNode *
parse_expr(AstParser *parser) {
	AstNode *lhs = parse_primary(parser);
	AstNode *result = parse_binop_rhs(parser, 0, lhs);
	return result;
}

static AstNode *
parse_paren(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Ascii);
	GB_ASSERT(parser->token->ascii == '(');
	parser_advance(parser);

	AstNode *expr = parse_expr(parser);

	GB_ASSERT(parser->token->type == TokenType_Ascii);
	GB_ASSERT(parser->token->ascii == ')');
	parser_advance(parser);

	return expr;
}

static AstNode *
parse_iden(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Identifier);
	String name = parser->token->identifier;
	parser_advance(parser);

	AstNode *result = gb_alloc_item(parser->arena_allocator, AstNode);
	if (parser->token->type != TokenType_Ascii || parser->token->ascii != '(') {
		result->type = AstType_Variable;
		result->variable.name = name;
	} else {

		parser_advance(parser);
		result->type = AstType_Call;
		result->call.callee = name;
		result->call.args = 0;
		result->call.arg_count = 0;

		if (parser->token->type != TokenType_Ascii || parser->token->ascii != ')') {
			result->call.args = gb_alloc_item(parser->arena_allocator, AstArgument);
			AstArgument *current_arg = result->call.args;
			current_arg->val = parse_expr(parser);
			current_arg->next = 0;
			result->call.arg_count = 1;

			while (parser->token->type != TokenType_Ascii || parser->token->ascii != ')') {
				current_arg->next = gb_alloc_item(parser->arena_allocator, AstArgument);
				current_arg = current_arg->next;
				current_arg->val = parse_expr(parser);
				current_arg->next = 0;
				result->call.arg_count += 1;
			}
		}

		GB_ASSERT(parser->token->type == TokenType_Ascii);
		GB_ASSERT(parser->token->ascii == ')');
		parser_advance(parser);
	}

	return result;
}

static AstNode *
parse_primary(AstParser *parser) {

	AstNode *result = 0;

	switch (parser->token->type) {

	case TokenType_Identifier: {
		result = parse_iden(parser);
	} break;

	case TokenType_Number: {
		result = parse_number(parser);
	} break;

	case TokenType_Ascii: {
		if (parser->token->ascii == '(') {
			result = parse_paren(parser);
		}
	} break;

	}

	GB_ASSERT(result != 0);
	return result;
}

static AstNode *
parse_binop_rhs(AstParser *parser, i32 precedence, AstNode *lhs) {
	AstNode *result = lhs;

	while (true) {
		i32 token_precendence = get_cur_tok_precedence(parser);
		if (token_precendence < precedence) {
			break;
		}

		char binop = parser->token->ascii;
		parser_advance(parser);

		AstNode *rhs = parse_primary(parser);

		i32 next_prec = get_cur_tok_precedence(parser);
		if (token_precendence < next_prec) {
			rhs = parse_binop_rhs(parser, token_precendence + 1, rhs);
		}

		AstNode *merged = gb_alloc_item(parser->arena_allocator, AstNode);
		merged->type = AstType_Binary;
		merged->binary.op = binop;
		merged->binary.lhs = result;
		merged->binary.rhs = rhs;
		result = merged;
	}

	return result;
}

static AstPrototype *
parse_prototype(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Identifier);

	String fn_name = parser->token->identifier;
	parser_advance(parser);

	GB_ASSERT(parser->token->type == TokenType_Ascii && parser->token->ascii == '(');
	parser_advance(parser);

	AstPrototype *proto = gb_alloc_item(parser->arena_allocator, AstPrototype);
	proto->name = fn_name;
	proto->param = 0;
	proto->param_count = 0;

	while (parser->token->type != TokenType_Ascii || parser->token->ascii != ')') {
		GB_ASSERT(parser->token->type == TokenType_Identifier);
		AstParameter *param = gb_alloc_item(parser->arena_allocator, AstParameter);
		param->name = parser->token->identifier;
		proto->param_count += 1;
		parser_advance(parser);

		if (proto->param == 0) {
			proto->param = param;
		}
	}

	GB_ASSERT(parser->token->type == TokenType_Ascii && parser->token->ascii == ')');
	parser_advance(parser);

	return proto;
}

static AstFunction *
parse_definition(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Def);
	parser_advance(parser);

	AstFunction *result = gb_alloc_item(parser->arena_allocator, AstFunction);
	result->proto = parse_prototype(parser);
	result->body = parse_expr(parser);

	return result;
}

static AstPrototype *
parse_extern(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Extern);
	parser_advance(parser);

	AstPrototype *proto = parse_prototype(parser);
	return proto;
}

static AstFunction *
parse_top_level_expr(AstParser *parser) {
	AstFunction *fun = gb_alloc_item(parser->arena_allocator, AstFunction);
	fun->proto = gb_alloc_item(parser->arena_allocator, AstPrototype);
	gb_zero_item(fun->proto);
	fun->body = parse_expr(parser);
	return fun;
}

//
// SECTION LLVM
//

static LLVMValueRef lb_node(LLVMBackend *lb, AstNode *node);

static LLVMValueRef
lb_number(LLVMBackend *lb, AstNumber *number) {
	f64 val = number->val;
	LLVMTypeRef type_double = LLVMDoubleTypeInContext(lb->ctx);
	LLVMValueRef llvm_val = LLVMConstReal(type_double, val);
	return llvm_val;
}

static LLVMValueRef
lb_variable(LLVMBackend *lb, AstVariable *variable) {
	LLVMValueRef *find_result = gb_htab_get(&lb->named_values, &variable->name);
	GB_ASSERT_NOT_NULL(find_result);
	LLVMValueRef result = *find_result;
	return result;
}

static LLVMValueRef
lb_binary(LLVMBackend *lb, AstBinary *binary) {
	LLVMValueRef lhs = lb_node(lb, binary->lhs);
	LLVMValueRef rhs = lb_node(lb, binary->rhs);

	LLVMValueRef result = 0;

	switch (binary->op) {

	case '+': {
		result = LLVMBuildFAdd(lb->builder, lhs, rhs, "addtmp");
	} break;

	case '-': {
		result = LLVMBuildFSub(lb->builder, lhs, rhs, "subtmp");
	} break;

	case '*': {
		result = LLVMBuildFMul(lb->builder, lhs, rhs, "multmp");
	} break;

	case '<': {
		LLVMValueRef logical = LLVMBuildFCmp(lb->builder, LLVMRealULT, lhs, rhs, "cmptmp");
		LLVMTypeRef type_double = LLVMDoubleTypeInContext(lb->ctx);
		result = LLVMBuildUIToFP(lb->builder, logical, type_double, "booltemp");
	} break;

	default: {
		GB_PANIC("unexpected binary op");
	}
	}

	return result;
}

static LLVMValueRef
lb_call(LLVMBackend *lb, AstCall *call) {
	gbTempArenaMemory temp_memory = gb_temp_arena_memory_begin(&lb->arena);

	char* temp_callee_name = string_to_cstring(&call->callee, lb->arena_allocator);
	LLVMValueRef callee = LLVMGetNamedFunction(lb->module, temp_callee_name);
	GB_ASSERT(callee != 0);
	GB_ASSERT(LLVMCountParams(callee) == call->arg_count);

	LLVMValueRef *arg_vals = gb_alloc_array(lb->arena_allocator, LLVMValueRef, call->arg_count);
	isize arg_index = 0;
	for (AstArgument *arg = call->args; arg != 0; arg = arg->next) {
		arg_vals[arg_index] = lb_node(lb, arg->val);
		arg_index += 1;
	}

	LLVMValueRef result = LLVMBuildCall(lb->builder, callee, arg_vals, (unsigned int)call->arg_count, "calltmp");

	gb_temp_arena_memory_end(temp_memory);
	return result;
}

static LLVMValueRef
lb_proto(LLVMBackend *lb, AstPrototype *proto) {
	gbTempArenaMemory temp_memory = gb_temp_arena_memory_begin(&lb->arena);

	LLVMTypeRef *arg_types = gb_alloc_array(lb->arena_allocator, LLVMTypeRef, proto->param_count);
	LLVMTypeRef llvm_double = LLVMDoubleTypeInContext(lb->ctx);
	for (isize arg_type_index = 0; arg_type_index < proto->param_count; arg_type_index += 1) {
		arg_types[arg_type_index] = llvm_double;
	}

	LLVMTypeRef fun_type = LLVMFunctionType(llvm_double, arg_types, (unsigned int)proto->param_count, false);

	char *temp_fun_name = string_to_cstring(&proto->name, lb->arena_allocator);
	LLVMValueRef llvm_fun = LLVMAddFunction(lb->module, temp_fun_name, fun_type);

	for (isize arg_index = 0; arg_index < proto->param_count; arg_index += 1) {
		LLVMValueRef llvm_param = LLVMGetParam(llvm_fun, (unsigned int)arg_index);
		String param_name = proto->param[arg_index].name;
		LLVMSetValueName2(llvm_param, param_name.ptr, param_name.len);
	}

	gb_temp_arena_memory_end(temp_memory);
	return llvm_fun;
}

static LLVMValueRef
lb_function(LLVMBackend *lb, AstFunction *fun) {
	gbTempArenaMemory temp_memory = gb_temp_arena_memory_begin(&lb->arena);

	char* temp_fun_name = string_to_cstring(&fun->proto->name, lb->arena_allocator);
	LLVMValueRef llvm_existing_fun = LLVMGetNamedFunction(lb->module, temp_fun_name);
	GB_ASSERT(llvm_existing_fun == 0);

	LLVMValueRef llvm_proto = lb_proto(lb, fun->proto);

	LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(lb->ctx, llvm_proto, "entry");
	LLVMPositionBuilderAtEnd(lb->builder, entry_block);

	gb_htab_clear(&lb->named_values);
	for (isize arg_index = 0; arg_index < fun->proto->param_count; arg_index += 1) {
		LLVMValueRef llvm_param = LLVMGetParam(llvm_proto, (unsigned int)arg_index);
		String param_name = fun->proto->param[arg_index].name;
		gb_htab_set(&lb->named_values, &param_name, &llvm_param);
	}

	LLVMValueRef llvm_body_return = lb_node(lb, fun->body);
	LLVMBuildRet(lb->builder, llvm_body_return);

	LLVMVerifyFunction(llvm_proto, LLVMAbortProcessAction);

	gb_temp_arena_memory_end(temp_memory);
	return llvm_proto;
}

static LLVMValueRef
lb_node(LLVMBackend *lb, AstNode *node) {
	LLVMValueRef result = 0;

	switch (node->type) {
	case AstType_None: { GB_PANIC("unexpected AstType_None"); } break;
	case AstType_Number: { result = lb_number(lb, &node->number); } break;
	case AstType_Variable: { result = lb_variable(lb, &node->variable); } break;
	case AstType_Binary: { result = lb_binary(lb, &node->binary); } break;
	case AstType_Call: { result = lb_call(lb, &node->call); } break;
	}

	return result;
}

//
// SECTION Main
//

int
main() {

	String input = string_from_cstring("def foo(x y z) foo(1 2 3)");

	gbAllocator heap_allocator = gb_heap_allocator();

	gbDynamicArray tokens = { 0 };
	gb_array_init(&tokens, heap_allocator, sizeof(Token));
	while (true) {
		Token token = get_token(&input);
		if (token.type == TokenType_EOF) {
			break;
		}
		gb_array_append(&tokens, &token);
	}

	AstParser parser = { 0 };
	gb_arena_init_from_allocator(&parser.arena, heap_allocator, gb_megabytes(4));
	parser.arena_allocator = gb_arena_allocator(&parser.arena);
	parser.token = tokens.ptr;
	parser.token_count = tokens.len;

	AstFunction *top_level_node = 0;
	switch (parser.token->type) {
	case TokenType_EOF: {
		GB_ASSERT(!"unexpected eof");
	} break;

	case TokenType_Ascii: {
		if (parser.token->ascii == ';') {
			parser_advance(&parser);
		}
	} break;

	case TokenType_Def: {
		top_level_node = parse_definition(&parser);
	} break;

	case TokenType_Extern: {
		GB_ASSERT("unimplemented");
		//top_level_node = parse_extern(&parser);
	} break;

	default: {
		top_level_node = parse_top_level_expr(&parser);
	} break;
	}
	GB_ASSERT_NOT_NULL(top_level_node);

	LLVMBackend llvm_backend;
	llvm_backend.ctx = LLVMGetGlobalContext();
	llvm_backend.builder = LLVMCreateBuilderInContext(llvm_backend.ctx);
	llvm_backend.module = LLVMModuleCreateWithNameInContext("KaleidoscopeModule", llvm_backend.ctx);

	gb_htab_init(
		&llvm_backend.named_values, heap_allocator, sizeof(String), sizeof(LLVMValueRef),
		string_hash, string_cmp
	);

	gb_arena_init_from_allocator(&llvm_backend.arena, heap_allocator, gb_megabytes(4));
	llvm_backend.arena_allocator = gb_arena_allocator(&llvm_backend.arena);

	lb_function(&llvm_backend, top_level_node);
	LLVMDumpModule(llvm_backend.module);

	return 0;
}
