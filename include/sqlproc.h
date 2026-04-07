#ifndef SQLPROC_H
#define SQLPROC_H

#include <stddef.h>

#define SQLPROC_MAX_NAME_LEN 64
#define SQLPROC_MAX_VALUE_LEN 64
#define SQLPROC_MAX_COLUMNS 16
#define SQLPROC_MAX_PREDICATES 2
#define SQLPROC_MAX_TOKENS 512
#define SQLPROC_MAX_STATEMENTS 32
#define SQLPROC_MAX_ERROR_LEN 256
#define SQLPROC_MAX_SQL_SIZE 8192

typedef enum {
    DATA_TYPE_INT,
    DATA_TYPE_STRING
} DataType;

typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_COMMA,
    TOKEN_SEMICOLON,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_STAR,
    TOKEN_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_KEYWORD_INSERT,
    TOKEN_KEYWORD_INTO,
    TOKEN_KEYWORD_VALUES,
    TOKEN_KEYWORD_SELECT,
    TOKEN_KEYWORD_FROM,
    TOKEN_KEYWORD_WHERE,
    TOKEN_KEYWORD_AND,
    TOKEN_KEYWORD_CREATE,
    TOKEN_KEYWORD_INDEX,
    TOKEN_KEYWORD_ON
} TokenType;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_CREATE_INDEX
} StatementType;

typedef enum {
    LITERAL_INT,
    LITERAL_STRING
} LiteralType;

typedef enum {
    COMPARE_EQUAL,
    COMPARE_LESS,
    COMPARE_LESS_EQUAL,
    COMPARE_GREATER,
    COMPARE_GREATER_EQUAL
} CompareOperator;

typedef struct {
    int line;
    int column;
} SourceLocation;

typedef struct {
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char input_path[256];
} AppConfig;

typedef struct {
    char message[SQLPROC_MAX_ERROR_LEN];
    int line;
    int column;
} ErrorInfo;

typedef struct {
    TokenType type;
    char text[SQLPROC_MAX_VALUE_LEN];
    int line;
    int column;
} Token;

typedef struct {
    Token items[SQLPROC_MAX_TOKENS];
    int count;
} TokenList;

typedef struct {
    LiteralType type;
    char text[SQLPROC_MAX_VALUE_LEN];
    SourceLocation location;
} LiteralValue;

typedef struct {
    char column_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation column_location;
    CompareOperator operator_type;
    SourceLocation operator_location;
    LiteralValue value;
} Predicate;

typedef struct {
    int count;
    Predicate items[SQLPROC_MAX_PREDICATES];
} WhereClause;

typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    int column_count;
    char column_names[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_NAME_LEN];
    SourceLocation column_locations[SQLPROC_MAX_COLUMNS];
    LiteralValue values[SQLPROC_MAX_COLUMNS];
} InsertStatement;

typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    int select_all;
    int column_count;
    char column_names[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_NAME_LEN];
    SourceLocation column_locations[SQLPROC_MAX_COLUMNS];
    WhereClause where_clause;
} SelectStatement;

typedef struct {
    char index_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation index_location;
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    char column_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation column_location;
} CreateIndexStatement;

typedef struct {
    StatementType type;
    SourceLocation location;
    InsertStatement insert_statement;
    SelectStatement select_statement;
    CreateIndexStatement create_index_statement;
} Statement;

typedef struct {
    Statement items[SQLPROC_MAX_STATEMENTS];
    int count;
} SqlProgram;

int parse_arguments(int argc, char **argv, AppConfig *config);
int run_program(const AppConfig *config);

int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error);
int tokenize_sql(const char *sql_text, TokenList *tokens, ErrorInfo *error);
int parse_program(const TokenList *tokens, SqlProgram *program, ErrorInfo *error);

const char *statement_type_name(StatementType type);
const char *compare_operator_name(CompareOperator operator_type);
const char *token_type_name(TokenType type);
void print_error(const ErrorInfo *error);

#endif
