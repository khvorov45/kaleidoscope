#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "llvm-c/Core.h"

typedef uint8_t u8;
typedef int32_t i32;
typedef uint64_t u64;

typedef double f64;


typedef struct String {
	char *ptr;
	u64 len; // NOTE(khvorov) Does not include the null terminator
} String;


typedef enum TokenType {
	TokenType_EOF,

	TokenType_Def,
	TokenType_Extern,

	TokenType_Identifier,
	TokenType_Number,

	TokenType_Ascii,
} TokenType;

typedef struct Token {
	TokenType type;
	String identifier;
	f64 number;
	char ascii;
} Token;

typedef struct TokenArray {
	Token *ptr;
	u64 len;
	u64 cap;
} TokenArray;


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
	u64 node_count;		
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

typedef struct AstNodeArray {
	AstNode *ptr;
	u64 len;
	u64 cap;
} AstNodeArray;

typedef struct AstParser {
	AstNodeArray nodes;
	Token *token;
	u64 token_count;
} AstParser;


static bool
char_is_alpha(char ch) {
	bool is_lower = ch >= 'a' && ch <= 'z';
	bool is_upper = ch >= 'A' && ch <= 'Z';
	bool result = is_lower || is_upper;
	return result;
}

static bool
char_is_digit(char ch) {
	bool result = ch >= '0' && ch <= '9';
	return result;
}

static bool
char_is_alphanum(char ch) {
	bool is_number = char_is_digit(ch);
	bool is_alpha = char_is_alpha(ch);
	bool result = is_number || is_alpha;
	return result;
}

static bool
char_is_space(char ch) {
	bool result = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
	return result;
}


static String
string_from_cstring(char *ptr) {
	u64 len = 0;
	for (u64 index = 0; ; index += 1) {
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
	for (u64 index = 0; index < str->len; index += 1) {
		printf("%c", str->ptr[index]);
	}
}

static void
string_offset(String *str, u64 offset, String *pre) {
	assert(str->len >= offset);
	if (pre != 0) {
		pre->ptr = str->ptr;
		pre->len = offset;	
	}
	str->ptr += offset;
	str->len -= offset;
}

static u64
string_index_nonalphanum(String *str) {
	u64 result = 0;
	while (result < str->len) {
		char ch = str->ptr[result];
		if (!char_is_alphanum(ch)) {
			break;
		}
		result += 1;
	}
	return result;
}

static u64
string_index_nonfloat(String *str) {
	u64 result = 0;
	while (result < str->len) {
		char ch = str->ptr[result];
		if (!char_is_digit(ch) && ch != '.') {
			break;
		}
		result += 1;
	}
	return result;
}

static bool
string_cmp_cstring(String *str, char *cstring) {
	bool result = true;
	u64 index = 0;
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
	for (u64 index = 0; index < str->len; index += 1) {
		char ch = str->ptr[index];
		if (ch == '\n' || ch == '\r') {
			u64 to_skip = index + 1;
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
	bool skipped = true;
	while (skipped) {
		skipped = false;
		while ((input->len > 0) && char_is_space(input->ptr[0])) {
			string_offset(input, 1, 0);
			skipped = true;
		}
		while ((input->len > 0) && input->ptr[0] == '#') {
			string_offset_to_next_line(input);
			skipped = true;
		}
	}

	if (input->len > 0) {
		if (char_is_alpha(input->ptr[0])) {

			u64 identifier_end = string_index_nonalphanum(input);
			string_offset(input, identifier_end, &result.identifier);

			if (string_cmp_cstring(&result.identifier, "def")) {
				result.type = TokenType_Def;	
			} else if (string_cmp_cstring(&result.identifier, "extern")) {
				result.type = TokenType_Extern;
			} else {
				result.type = TokenType_Identifier;
			}

		} else if (char_is_digit(input->ptr[0])) {

			u64 number_end = string_index_nonfloat(input);
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

static TokenArray
make_token_array() {
	TokenArray result = { 0, 0, 100 };
	result.ptr = malloc(sizeof(Token) * result.cap);
	return result;
}

static void
token_array_push(TokenArray *arr, Token token) {
	assert(arr->len <= arr->cap);
	if (arr->len == arr->cap) {
		arr->cap *= 2;
		arr->ptr = realloc(arr->ptr, sizeof(Token) * arr->cap);
	}
	arr->ptr[arr->len] = token;
	arr->len += 1;
}


static void 
parser_advance(AstParser *parser) {
	assert(parser->token_count > 0);
	parser->token += 1;
	parser->token_count -= 1;
}

static AstNodeArray
make_ast_node_array() {
	AstNodeArray result = { 0, 0, 100 };
	result.ptr = malloc(sizeof(AstNode) * result.cap);
	return result;
}

static AstNode *
ast_node_array_push(AstNodeArray *arr, AstNode node) {
	assert(arr->len <= arr->cap);
	if (arr->len == arr->cap) {
		arr->cap *= 2;
		arr->ptr = realloc(arr->ptr, sizeof(AstNode) * arr->cap);
	}
	AstNode *result = arr->ptr + arr->len;
	*result = node;
	arr->len += 1;
	return result;
}

static AstNode *
parse_number(AstParser *parser) {
	Token *token = parser->token;
	assert(token->type == TokenType_Number);
	AstNumber number = { token->number };
	AstNode num_node = { AstType_Number, .number = number };
	parser_advance(parser);

	AstNode *result = ast_node_array_push(&parser->nodes, num_node);
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
	assert(parser->token->type == TokenType_Ascii);
	assert(parser->token->ascii == '(');
	parser_advance(parser);

	AstNode *expr = parse_expr(parser);

	assert(parser->token->type == TokenType_Ascii);
	assert(parser->token->ascii == ')');
	parser_advance(parser);

	return expr;
}

static AstNode *
parse_iden(AstParser *parser) {
	assert(parser->token->type == TokenType_Identifier);
	String name = parser->token->identifier;
	parser_advance(parser);

	AstNode *result = 0;
	if (parser->token->type != TokenType_Ascii || parser->token->ascii != '(') {
		AstVariable var = { name };
		AstNode var_node = { AstType_Variable, .variable = var };
		result = ast_node_array_push(&parser->nodes, var_node);
	} else {

		parser_advance(parser);
		AstCall call = { name, parser->nodes.ptr + parser->nodes.len, 0 };

		while (parser->token->type != TokenType_Ascii || parser->token->ascii != ')') {
			assert(call.arg_count < 255);
			parse_expr(parser);
			call.arg_count += 1;
		}
		parser_advance(parser);

		AstNode call_node = { AstType_Call, .call = call };
		result = ast_node_array_push(&parser->nodes, call_node);
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

	assert(result != 0);
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
		result = ast_node_array_push(&parser->nodes, merged_node);
	}

	return result;
}

static AstNode *
parse_prototype(AstParser *parser) {
	assert(parser->token->type == TokenType_Identifier);

	String fn_name = parser->token->identifier;
	parser_advance(parser);

	assert(parser->token->type == TokenType_Ascii && parser->token->ascii == '(');
	parser_advance(parser);

	AstPrototype proto = { fn_name, parser->nodes.ptr + parser->nodes.len, 0 };
	while (parser->token->type == TokenType_Ascii && parser->token->ascii == ')') {
		assert(parser->token->type == TokenType_Identifier);
		assert(proto.param_count < 255);
		String param_name = parser->token->identifier;
		AstFnParameter param = { param_name };
		AstNode param_node = { AstType_FnParameter, .fn_parameter = param };
		ast_node_array_push(&parser->nodes, param_node);
		proto.param_count += 1;
		parser_advance(parser);
	}

	AstNode proto_node = { AstType_Prototype, .prototype = proto };
	AstNode *result = ast_node_array_push(&parser->nodes, proto_node);
	return result;
}

static AstNode *
parse_definition(AstParser *parser) {
	assert(parser->token->type == TokenType_Def);
	parser_advance(parser);
	
	AstNode *proto = parse_prototype(parser);
	AstNode *body = parse_expr(parser);

	AstFunction fn = { proto, body };
	AstNode fn_node = { AstType_Function, .function = fn };
	AstNode *result = ast_node_array_push(&parser->nodes, fn_node);
	return result;
}

static AstNode *
parse_extern(AstParser *parser) {
	assert(parser->token->type == TokenType_Extern);
	parser_advance(parser);

	AstNode *proto = parse_prototype(parser);
	return proto;
}

static AstNode *
parse_top_level_expr(AstParser *parser) {
	AstNode *expr = parse_expr(parser);
	AstPrototype proto = { 0 };
	AstNode proto_node = { AstType_Prototype, .prototype = proto };
	AstNode *proto_node_ptr = ast_node_array_push(&parser->nodes, proto_node);
	AstFunction fn = { proto_node_ptr, expr };
	AstNode fn_node = { AstType_Function, .function = fn };
	AstNode *fn_node_ptr = ast_node_array_push(&parser->nodes, fn_node);
	return fn_node_ptr;
}


static LLVMValueRef
lb_number(AstNode *node) {
	assert(node->type == AstType_Number);
	f64 val = node->number.val;
	LLVMContextRef ctx = LLVMGetGlobalContext();
	LLVMTypeRef type_double = LLVMDoubleTypeInContext(ctx);	
	LLVMValueRef llvm_val = LLVMConstReal(type_double, val);
	return llvm_val;
}


int
main() {

	String input = string_from_cstring("a + 1");

	TokenArray tokens = make_token_array();
	while (true) {
		Token token = get_token(&input);
		if (token.type == TokenType_EOF) {
			break;
		}
		token_array_push(&tokens, token);
	}

	AstNodeArray ast_node_array = make_ast_node_array();
	AstParser parser = { ast_node_array, tokens.ptr, tokens.len };

	while (parser.token_count > 0) {
		Token *token = parser.token;

		switch (token->type) {
		case TokenType_EOF: {
			assert(!"unexpected eof");
		} break;

		case TokenType_Ascii: {
			if (token->ascii == ';') {
				parser_advance(&parser);
			}
		} break;

		case TokenType_Def: {
			parse_definition(&parser);
		} break;

		case TokenType_Extern: {
			parse_extern(&parser);
		} break;

		default: {
			parse_top_level_expr(&parser);
		} break;
		}
	}

	return 0;
}
