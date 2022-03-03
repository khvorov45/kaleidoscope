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


int 
main() {

	String input = string_from_cstring("#comment here \ntest 123.456 #comment\r\n def + -, extern #comment\r ");

	while (true) {
		Token token = get_token(&input);

		switch (token.type) {
		case TokenType_EOF: {
			printf("eof\n");
		} break;

		case TokenType_Def: {
			printf("def\n");
		} break;

		case TokenType_Extern: {
			printf("extern\n");
		} break;

		case TokenType_Identifier: {
			printf("TokenType_Identifier: '");
			string_print(&token.identifier);
			printf("'\n");
		} break;

		case TokenType_Number: {
			printf("TokenType_Number: '%f'\n", token.number);
		} break;

		case TokenType_Ascii: {
			printf("TokenType_Ascii: '%c'\n", token.ascii);
		} break;
		}

		if (token.type == TokenType_EOF) {
			break;
		}
	}

	return 0;
}
