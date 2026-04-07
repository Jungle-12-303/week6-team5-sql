#ifndef SQLPROC_H
#define SQLPROC_H

#include <stddef.h>

/*
 * 이 헤더는 프로젝트 전체에서 공유하는 "공용 계약"입니다.
 * - 최대 길이 상수
 * - 토큰 / AST / 스키마 / 실행 설정 구조체
 * - 모듈 간에 호출하는 함수 선언
 *
 * 각 .c 파일은 이 헤더를 통해 같은 데이터 구조를 공유합니다.
 */

#define SQLPROC_MAX_NAME_LEN 64
#define SQLPROC_MAX_VALUE_LEN 64
#define SQLPROC_MAX_COLUMNS 16
#define SQLPROC_MAX_PREDICATES 2
#define SQLPROC_MAX_TOKENS 512
#define SQLPROC_MAX_STATEMENTS 32
#define SQLPROC_MAX_ERROR_LEN 256
#define SQLPROC_MAX_SQL_SIZE 8192
#define SQLPROC_BTREE_MAX_KEYS 4
#define SQLPROC_MAX_INDEX_RESULTS 2048

typedef enum {
    DATA_TYPE_INT,
    DATA_TYPE_STRING
} DataType;

/* 토크나이저가 SQL 문자열을 잘라 낸 결과 토큰 종류입니다. */
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

/* 파서가 구분하는 최상위 SQL 문장 종류입니다. */
typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_CREATE_INDEX
} StatementType;

/* 파서 단계에서 읽은 리터럴의 실제 타입입니다. */
typedef enum {
    LITERAL_INT,
    LITERAL_STRING
} LiteralType;

/* WHERE 절 비교 연산자 종류입니다. */
typedef enum {
    COMPARE_EQUAL,
    COMPARE_LESS,
    COMPARE_LESS_EQUAL,
    COMPARE_GREATER,
    COMPARE_GREATER_EQUAL
} CompareOperator;

/* 파서/실행기 오류가 발생한 SQL 상의 위치입니다. */
typedef struct {
    int line;
    int column;
} SourceLocation;

/*
 * 프로그램 실행 시 필요한 경로 설정입니다.
 * - schema_dir: <table>.schema 파일 위치
 * - data_dir: <table>.csv 파일 위치
 * - index_dir: <index>.idx 파일 위치
 * - input_path: 선택적으로 넘긴 SQL 파일 경로
 * - has_input_path: input_path가 실제로 주어졌는지 여부
 */
typedef struct {
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char input_path[256];
    int has_input_path;
} AppConfig;

/* 사용자에게 보여 줄 오류 메시지와 선택적 위치 정보입니다. */
typedef struct {
    char message[SQLPROC_MAX_ERROR_LEN];
    int line;
    int column;
} ErrorInfo;

/* 토크나이저가 만든 토큰 1개입니다. */
typedef struct {
    TokenType type;
    char text[SQLPROC_MAX_VALUE_LEN];
    int line;
    int column;
} Token;

/* SQL 문자열 전체를 자른 토큰 배열입니다. */
typedef struct {
    Token items[SQLPROC_MAX_TOKENS];
    int count;
} TokenList;

/* 파서가 읽은 정수/문자열 리터럴입니다. */
typedef struct {
    LiteralType type;
    char text[SQLPROC_MAX_VALUE_LEN];
    SourceLocation location;
} LiteralValue;

/* 스키마의 컬럼 1개 정의입니다. */
typedef struct {
    char name[SQLPROC_MAX_NAME_LEN];
    DataType type;
    int is_primary_key;
} ColumnSchema;

/* 테이블 스키마 전체 정의입니다. */
typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    int column_count;
    int primary_key_column_index;
    ColumnSchema columns[SQLPROC_MAX_COLUMNS];
} TableSchema;

/* WHERE 절의 조건 1개입니다. 예: age >= 20 */
typedef struct {
    char column_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation column_location;
    CompareOperator operator_type;
    SourceLocation operator_location;
    LiteralValue value;
} Predicate;

/* 현재 프로젝트가 지원하는 WHERE 절 전체 정보입니다. */
typedef struct {
    int count;
    Predicate items[SQLPROC_MAX_PREDICATES];
} WhereClause;

/* INSERT 문 AST입니다. */
typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    int has_column_list;
    int column_count;
    int value_count;
    char column_names[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_NAME_LEN];
    SourceLocation column_locations[SQLPROC_MAX_COLUMNS];
    LiteralValue values[SQLPROC_MAX_COLUMNS];
} InsertStatement;

/* SELECT 문 AST입니다. */
typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    int select_all;
    int column_count;
    char column_names[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_NAME_LEN];
    SourceLocation column_locations[SQLPROC_MAX_COLUMNS];
    WhereClause where_clause;
} SelectStatement;

/* CREATE INDEX 문 AST입니다. */
typedef struct {
    char index_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation index_location;
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    char column_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation column_location;
} CreateIndexStatement;

/* 최상위 SQL 문장 1개입니다. */
typedef struct {
    StatementType type;
    SourceLocation location;
    InsertStatement insert_statement;
    SelectStatement select_statement;
    CreateIndexStatement create_index_statement;
} Statement;

/* SQL 파일 또는 입력 버퍼에서 읽은 문장들의 목록입니다. */
typedef struct {
    Statement items[SQLPROC_MAX_STATEMENTS];
    int count;
} SqlProgram;

/* app.c */
int parse_arguments(int argc, char **argv, AppConfig *config);
int run_program(const AppConfig *config);

/* app.c / tokenizer.c / parser.c / schema.c */
int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error);
int tokenize_sql(const char *sql_text, TokenList *tokens, ErrorInfo *error);
int parse_program(const TokenList *tokens, SqlProgram *program, ErrorInfo *error);
int load_table_schema(const char *schema_dir,
                      const char *table_name,
                      TableSchema *schema,
                      ErrorInfo *error);

/* executor.c / btree_index.c */
int execute_program(const AppConfig *config, const SqlProgram *program, ErrorInfo *error);
int create_index_from_statement(const AppConfig *config,
                                const CreateIndexStatement *statement,
                                ErrorInfo *error);
int update_all_indexes_for_row(const AppConfig *config,
                               const TableSchema *schema,
                               char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                               long row_offset,
                               int *changed_index,
                               ErrorInfo *error);
int rebuild_indexes_for_table(const AppConfig *config,
                              const TableSchema *schema,
                              ErrorInfo *error);
int try_collect_offsets_from_indexes(const AppConfig *config,
                                     const TableSchema *schema,
                                     const SelectStatement *statement,
                                     long offsets[SQLPROC_MAX_INDEX_RESULTS],
                                     int *offset_count,
                                     int *used_index,
                                     ErrorInfo *error);

/* 디버깅/오류 메시지용 문자열 변환 함수입니다. */
const char *statement_type_name(StatementType type);
const char *compare_operator_name(CompareOperator operator_type);
const char *token_type_name(TokenType type);
void print_error(const ErrorInfo *error);

#endif
