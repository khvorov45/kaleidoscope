#define GB_IMPLEMENTATION
#define GB_STATIC
#include "gb.h"

#include "llvm-c/Core.h"


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

typedef struct AstFnParameter {
	String name;
} AstFnParameter;

typedef struct AstBinary {
	char op;
	struct AstNode *lhs;
	struct AstNode *rhs;
} AstBinary;

typedef struct AstCall {
	String callee;
	struct AstNode *args;
	u8 arg_count;
} AstCall;

typedef struct AstPrototype {
	String name;
	struct AstNode *param;
	u8 param_count;
} AstPrototype;

typedef struct AstBlock {
	struct AstNode *nodes;
	isize node_count;		
} AstBlock;

typedef struct AstFunction {
	struct AstNode *proto;
	struct AstNode *body;
} AstFunction;

typedef enum AstType {
	AstType_None,
	AstType_Number,
	AstType_Variable,
	AstType_FnParameter,
	AstType_Binary,
	AstType_Call,
	AstType_Prototype,
	AstType_Block,
	AstType_Function,
} AstType;

typedef struct AstNode {
	AstType type;
	union {
		AstNumber number;
		AstVariable variable;
		AstFnParameter fn_parameter;
		AstBinary binary;
		AstCall call;
		AstPrototype prototype;
		AstFunction function;
		AstBlock block;
	};
} AstNode;

typedef struct AstParser {
	gbDynamicArray nodes;
	Token *token;
	isize token_count;
} AstParser;

typedef struct NamedValues {
	isize ignore;
} NamedValues;

typedef struct LLVMBackend {
	LLVMContextRef ctx;
	LLVMBuilderRef builder;
	NamedValues named_values;
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
	AstNumber number = { token->number };
	AstNode num_node = { AstType_Number, .number = number };
	parser_advance(parser);
	AstNode *result = gb_array_append(&parser->nodes, &num_node);
	return result;
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

	AstNode *result = 0;
	if (parser->token->type != TokenType_Ascii || parser->token->ascii != '(') {
		AstVariable var = { name };
		AstNode var_node = { AstType_Variable, .variable = var };
		result = gb_array_append(&parser->nodes, &var_node);
	} else {

		parser_advance(parser);
		AstCall call = { name, (AstNode *)parser->nodes.ptr + parser->nodes.len, 0 };

		while (parser->token->type != TokenType_Ascii || parser->token->ascii != ')') {
			GB_ASSERT(call.arg_count < 255);
			parse_expr(parser);
			call.arg_count += 1;
		}
		parser_advance(parser);

		AstNode call_node = { AstType_Call, .call = call };
		result = gb_array_append(&parser->nodes, &call_node);
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

		AstBinary merged = { binop, result, rhs };
		AstNode merged_node = { AstType_Binary, .binary = merged };
		result = gb_array_append(&parser->nodes, &merged_node);
	}

	return result;
}

static AstNode *
parse_prototype(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Identifier);

	String fn_name = parser->token->identifier;
	parser_advance(parser);

	GB_ASSERT(parser->token->type == TokenType_Ascii && parser->token->ascii == '(');
	parser_advance(parser);

	AstPrototype proto = { fn_name, (AstNode *)parser->nodes.ptr + parser->nodes.len, 0 };
	while (parser->token->type == TokenType_Ascii && parser->token->ascii == ')') {
		GB_ASSERT(parser->token->type == TokenType_Identifier);
		GB_ASSERT(proto.param_count < 255);
		String param_name = parser->token->identifier;
		AstFnParameter param = { param_name };
		AstNode param_node = { AstType_FnParameter, .fn_parameter = param };
		gb_array_append(&parser->nodes, &param_node);
		proto.param_count += 1;
		parser_advance(parser);
	}

	AstNode proto_node = { AstType_Prototype, .prototype = proto };
	AstNode *result = gb_array_append(&parser->nodes, &proto_node);
	return result;
}

static AstNode *
parse_definition(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Def);
	parser_advance(parser);
	
	AstNode *proto = parse_prototype(parser);
	AstNode *body = parse_expr(parser);

	AstFunction fn = { proto, body };
	AstNode fn_node = { AstType_Function, .function = fn };
	AstNode *result = gb_array_append(&parser->nodes, &fn_node);
	return result;
}

static AstNode *
parse_extern(AstParser *parser) {
	GB_ASSERT(parser->token->type == TokenType_Extern);
	parser_advance(parser);

	AstNode *proto = parse_prototype(parser);
	return proto;
}

static AstNode *
parse_top_level_expr(AstParser *parser) {
	AstNode *expr = parse_expr(parser);
	AstPrototype proto = { 0 };
	AstNode proto_node = { AstType_Prototype, .prototype = proto };
	AstNode *proto_node_ptr = gb_array_append(&parser->nodes, &proto_node);
	AstFunction fn = { proto_node_ptr, expr };
	AstNode fn_node = { AstType_Function, .function = fn };
	AstNode *fn_node_ptr = gb_array_append(&parser->nodes, &fn_node);
	return fn_node_ptr;
}

static void
named_values_init(NamedValues *named_values, gbAllocator allocator) {
	return;
}

static LLVMValueRef
named_values_get(NamedValues *named_values, u64 hash) {
	LLVMValueRef result = 0;
	return result;
}


static LLVMValueRef lb_node(LLVMBackend *lb, AstNode *node);

static LLVMValueRef
lb_number(LLVMBackend *lb, AstNode *node) {
	GB_ASSERT(node->type == AstType_Number);
	f64 val = node->number.val;
	LLVMTypeRef type_double = LLVMDoubleTypeInContext(lb->ctx);	
	LLVMValueRef llvm_val = LLVMConstReal(type_double, val);
	return llvm_val;
}

static LLVMValueRef
lb_variable(LLVMBackend *lb, AstNode *node) {
	GB_ASSERT(node->type == AstType_Variable);
	String name = node->variable.name;
	u64 name_hash = gb_murmur64(name.ptr, name.len);
	LLVMValueRef result = named_values_get(&lb->named_values, name_hash);
	return result;
}

static LLVMValueRef
lb_binary(LLVMBackend *lb, AstNode *node) {
	GB_ASSERT(node->type == AstType_Binary);

	LLVMValueRef lhs = lb_node(lb, node->binary.lhs);
	LLVMValueRef rhs = lb_node(lb, node->binary.rhs);

	LLVMValueRef result = 0;

	switch (node->binary.op) {
	
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
lb_node(LLVMBackend *lb, AstNode *node) {
	LLVMValueRef result = 0;
	return result;
}

int
main() {

	String input = string_from_cstring("a + 1 + 2 - 1 > 3 + 2 + f + s + a");

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
	gb_array_init(&parser.nodes, heap_allocator, sizeof(AstNode));
	parser.token = tokens.ptr;
	parser.token_count = tokens.len;

	AstNode *top_level_node = 0;
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
		top_level_node = parse_extern(&parser);
	} break;

	default: {
		top_level_node = parse_top_level_expr(&parser);
	} break;
	}
	GB_ASSERT_NOT_NULL(top_level_node);

	LLVMBackend llvm_backend;
	llvm_backend.ctx = LLVMGetGlobalContext();
	llvm_backend.builder = LLVMCreateBuilderInContext(llvm_backend.ctx);
	named_values_init(&llvm_backend.named_values, heap_allocator);

	lb_node(&llvm_backend, top_level_node);

	return 0;
}
