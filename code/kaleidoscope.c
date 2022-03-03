#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

typedef uint8_t u8;
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
	u8 ascii;
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

typedef struct AstBinary {
	char op;
	struct AstNode *lhs;
	struct AstNode *rhs;
} AstBinary;

typedef struct AstCall {
	String callee;
	struct AstNodeArray *args;
	u8 arg_count;
} AstCall;

typedef struct AstPrototype {
	String name;
	String arg_names[4];
} AstPrototype;

typedef struct AstBlock {
	struct AstNode *nodes;
	u64 node_count;		
} AstBlock;

typedef struct AstFunction {
	AstPrototype *proto;
	AstBlock body;
} AstFunction;

typedef enum AstType {
	AstType_None,
	AstType_Number,
	AstType_Variable,
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


static AstNode
parse_ast_number(TokenArray *tokens, u64 *token_index) {
	Token *token = tokens->ptr + *token_index;
	assert(token->type == TokenType_Number);
	AstNumber number = { token->number };
	AstNode result = { AstType_Number, number };
	*token_index += 1;
	return result;
}

static AstNodeArray
make_ast_node_array(u64 cap) {
	AstNodeArray result = { 0, 0, cap };
	result.ptr = malloc(sizeof(AstNode) * result.cap);
	return result;
}

static void
ast_node_array_push(AstNodeArray *arr, AstNode node) {
	assert(arr->len <= arr->cap);
	if (arr->len == arr->cap) {
		arr->cap *= 2;
		arr->ptr = realloc(arr->ptr, sizeof(AstNode) * arr->cap);
	}
	arr->ptr[arr->len] = node;
	arr->len += 1;
}

static AstBlock
parse_ast_block(TokenArray *tokens, u64 *token_index) {

	AstNodeArray ast_nodes = make_ast_node_array(20);

	while (*token_index < tokens->len) {
		Token token = tokens->ptr[*token_index];

		AstNode ast_node = { 0 };
		switch (token.type) {
		case TokenType_EOF: {
			assert(!"unexpected eof");
		} break;

		case TokenType_Def: {
			printf("def\n");
			*token_index += 1;
		} break;

		case TokenType_Extern: {
			printf("extern\n");
			*token_index += 1;
		} break;

		case TokenType_Identifier: {
			printf("TokenType_Identifier: '");
			string_print(&token.identifier);
			printf("'\n");
			*token_index += 1;
		} break;

		case TokenType_Number: {
			printf("TokenType_Number: '%f'\n", token.number);
			ast_node = parse_ast_number(tokens, token_index);
		} break;

		case TokenType_Ascii: {
			printf("TokenType_Ascii: '%c'\n", token.ascii);
			*token_index += 1;
		} break;
		}

		ast_node_array_push(&ast_nodes, ast_node);
	}

	AstBlock block = { ast_nodes.ptr, ast_nodes.len };
	return block;
}


int
main() {

	String input = string_from_cstring("#comment here \ntest 123.456 #comment\r\n def + -, extern #comment\r ");

	TokenArray tokens = make_token_array();

	while (true) {
		Token token = get_token(&input);
		if (token.type == TokenType_EOF) {
			break;
		}
		token_array_push(&tokens, token);
	}

	u64 token_index = 0;
	AstBlock ast_block = parse_ast_block(&tokens, &token_index);
	assert(token_index == tokens.len);

	return 0;
}
