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
#define SQLPROC_BTREE_MAX_KEYS 4
#define SQLPROC_MAX_INDEX_RESULTS 2048

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
    int has_input_path;
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
    char name[SQLPROC_MAX_NAME_LEN];
    DataType type;
    int is_primary_key;
} ColumnSchema;

typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    int column_count;
    int primary_key_column_index;
    ColumnSchema columns[SQLPROC_MAX_COLUMNS];
} TableSchema;

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
    int has_column_list;
    int column_count;
    int value_count;
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

/* 커맨드라인 인수를 파싱하여 AppConfig를 채운다.
 *
 * @param argc    인수 개수
 * @param argv    인수 배열
 * @param config  결과를 저장할 설정 구조체 포인터
 * @return        성공 시 1, 실패 시 0
 */
int parse_arguments(int argc, char **argv, AppConfig *config);

/* 설정에 따라 파일 모드 또는 대화형 모드로 프로그램을 실행한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @return        성공 시 1, 실패 시 0
 */
int run_program(const AppConfig *config);

/* SQL 파일을 읽어 버퍼에 저장한다.
 *
 * @param path         읽을 파일 경로
 * @param buffer       내용을 저장할 버퍼
 * @param buffer_size  버퍼 크기
 * @param error        오류 정보 저장 포인터
 * @return             성공 시 1, 실패 시 0
 */
int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error);

/* SQL 문자열을 토큰 목록으로 변환한다.
 *
 * @param sql_text  입력 SQL 문자열
 * @param tokens    결과 토큰 목록 포인터
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
int tokenize_sql(const char *sql_text, TokenList *tokens, ErrorInfo *error);

/* 토큰 목록을 파싱하여 SQL 프로그램 AST를 생성한다.
 *
 * @param tokens   입력 토큰 목록 포인터
 * @param program  결과 AST 저장 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
int parse_program(const TokenList *tokens, SqlProgram *program, ErrorInfo *error);

/* 스키마 디렉토리에서 테이블 스키마 파일을 읽어 파싱한다.
 *
 * @param schema_dir  스키마 파일 디렉토리 경로
 * @param table_name  테이블 이름
 * @param schema      결과 스키마 저장 포인터
 * @param error       오류 정보 저장 포인터
 * @return            성공 시 1, 실패 시 0
 */
int load_table_schema(const char *schema_dir,
                      const char *table_name,
                      TableSchema *schema,
                      ErrorInfo *error);

/* SQL 프로그램의 모든 문장을 순서대로 실행한다.
 *
 * @param config   실행 설정 구조체 포인터
 * @param program  실행할 SQL 프로그램 AST 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
int execute_program(const AppConfig *config, const SqlProgram *program, ErrorInfo *error);

/* CREATE INDEX 문을 실행하여 B+ 트리 인덱스 파일을 생성한다.
 *
 * @param config     실행 설정 구조체 포인터
 * @param statement  CREATE INDEX AST 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
int create_index_from_statement(const AppConfig *config,
                                const CreateIndexStatement *statement,
                                ErrorInfo *error);

/* 삽입된 행의 값을 해당 테이블의 모든 인덱스에 반영한다.
 *
 * @param config         실행 설정 구조체 포인터
 * @param schema         테이블 스키마 포인터
 * @param row_values     삽입된 행의 컬럼별 값 배열
 * @param row_offset     CSV 파일 내 행의 바이트 오프셋
 * @param changed_index  업데이트된 인덱스 수를 저장할 포인터
 * @param error          오류 정보 저장 포인터
 * @return               성공 시 1, 실패 시 0
 */
int update_all_indexes_for_row(const AppConfig *config,
                               const TableSchema *schema,
                               char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                               long row_offset,
                               int *changed_index,
                               ErrorInfo *error);

/* CSV 파일의 현재 내용을 기반으로 테이블의 모든 인덱스를 재빌드한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @param schema  테이블 스키마 포인터
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
int rebuild_indexes_for_table(const AppConfig *config,
                              const TableSchema *schema,
                              ErrorInfo *error);

/* SELECT 문의 WHERE 절에 맞는 인덱스를 찾아 해당 행 오프셋 목록을 반환한다.
 *
 * @param config        실행 설정 구조체 포인터
 * @param schema        테이블 스키마 포인터
 * @param statement     SELECT AST 포인터
 * @param offsets       결과 오프셋 배열
 * @param offset_count  결과 오프셋 개수를 저장할 포인터
 * @param used_index    인덱스 사용 여부를 저장할 포인터
 * @param error         오류 정보 저장 포인터
 * @return              성공 시 1, 실패 시 0
 */
int try_collect_offsets_from_indexes(const AppConfig *config,
                                     const TableSchema *schema,
                                     const SelectStatement *statement,
                                     long offsets[SQLPROC_MAX_INDEX_RESULTS],
                                     int *offset_count,
                                     int *used_index,
                                     ErrorInfo *error);

/* StatementType 열거값을 사람이 읽기 쉬운 문자열로 반환한다.
 *
 * @param type  문장 종류 열거값
 * @return      해당 열거값의 이름 문자열
 */
const char *statement_type_name(StatementType type);

/* CompareOperator 열거값을 사람이 읽기 쉬운 문자열로 반환한다.
 *
 * @param operator_type  비교 연산자 열거값
 * @return               해당 열거값의 이름 문자열
 */
const char *compare_operator_name(CompareOperator operator_type);

/* TokenType 열거값을 사람이 읽기 쉬운 문자열로 반환한다.
 *
 * @param type  토큰 종류 열거값
 * @return      해당 열거값의 이름 문자열
 */
const char *token_type_name(TokenType type);

/* ErrorInfo의 오류 메시지와 위치 정보를 stderr에 출력한다.
 *
 * @param error  출력할 오류 정보 포인터
 */
void print_error(const ErrorInfo *error);

#endif
