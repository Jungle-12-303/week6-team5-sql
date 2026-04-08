#ifndef SQLPROC_H
#define SQLPROC_H

#include <stddef.h>

/* 이름, 문자열 값, 컬럼 수 같은 기본 크기 제한값들이다. */
#define SQLPROC_MAX_NAME_LEN 64
#define SQLPROC_MAX_VALUE_LEN 64
#define SQLPROC_MAX_COLUMNS 16
#define SQLPROC_MAX_PREDICATES 2

/* 토큰화와 파싱 단계에서 사용할 최대 개수 제한값들이다. */
#define SQLPROC_MAX_TOKENS 512
#define SQLPROC_MAX_STATEMENTS 32

/* 오류 메시지, SQL 입력, 인덱스 후보 수의 최대 크기 제한값들이다. */
#define SQLPROC_MAX_ERROR_LEN 256
#define SQLPROC_MAX_SQL_SIZE 8192
#define SQLPROC_MAX_INDEX_RESULTS 2048

/* 초심자가 split 흐름을 따라가기 쉽도록 작게 둔 B+ 트리 최대 키 수이다. */
#define SQLPROC_BTREE_MAX_KEYS 4

/* 스키마 컬럼 타입을 표현하는 열거형이다. */
typedef enum {
    DATA_TYPE_INT,
    DATA_TYPE_STRING
} DataType;

/* 토크나이저가 만들어 내는 토큰 종류를 표현하는 열거형이다. */
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

/* SQL 문장 종류를 구분하는 열거형이다. */
typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_CREATE_INDEX
} StatementType;

/* 리터럴 값이 정수인지 문자열인지 구분하는 열거형이다. */
typedef enum {
    LITERAL_INT,
    LITERAL_STRING
} LiteralType;

/* WHERE 절에서 사용하는 비교 연산자 종류를 표현하는 열거형이다. */
typedef enum {
    COMPARE_EQUAL,
    COMPARE_LESS,
    COMPARE_LESS_EQUAL,
    COMPARE_GREATER,
    COMPARE_GREATER_EQUAL
} CompareOperator;

/* SQL 원문 안의 줄 번호와 열 번호를 저장한다. */
typedef struct {
    int line;
    int column;
} SourceLocation;

/* 프로그램 실행에 필요한 디렉토리 경로와 입력 방식 정보를 저장한다. */
typedef struct {
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char input_path[256];
    int has_input_path;
} AppConfig;

/* 오류 메시지와 원문 위치 정보를 함께 전달할 때 사용하는 구조체이다. */
typedef struct {
    char message[SQLPROC_MAX_ERROR_LEN];
    int line;
    int column;
} ErrorInfo;

/* 토크나이저가 만든 토큰 1개를 표현한다. */
typedef struct {
    TokenType type;
    char text[SQLPROC_MAX_VALUE_LEN];
    int line;
    int column;
} Token;

/* 토큰 배열과 실제 사용 개수를 함께 묶은 구조체이다. */
typedef struct {
    Token items[SQLPROC_MAX_TOKENS];
    int count;
} TokenList;

/* 정수/문자열 리터럴 값과 해당 위치를 저장한다. */
typedef struct {
    LiteralType type;
    char text[SQLPROC_MAX_VALUE_LEN];
    SourceLocation location;
} LiteralValue;

/* 스키마의 컬럼 이름, 타입, PRIMARY KEY 여부를 저장한다. */
typedef struct {
    char name[SQLPROC_MAX_NAME_LEN];
    DataType type;
    int is_primary_key;
} ColumnSchema;

/* 테이블 이름과 컬럼 목록, PRIMARY KEY 위치를 저장한다. */
typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    int column_count;
    int primary_key_column_index;
    ColumnSchema columns[SQLPROC_MAX_COLUMNS];
} TableSchema;

/* WHERE 절의 조건 1개를 "컬럼 연산자 값" 형태로 저장한다. */
typedef struct {
    char column_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation column_location;
    CompareOperator operator_type;
    SourceLocation operator_location;
    LiteralValue value;
} Predicate;

/* 최대 2개의 WHERE 조건을 묶어 저장한다. */
typedef struct {
    int count;
    Predicate items[SQLPROC_MAX_PREDICATES];
} WhereClause;

/* INSERT 문에서 필요한 테이블명, 컬럼 목록, 값 목록을 저장한다. */
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

/* SELECT 문에서 필요한 선택 컬럼과 WHERE 절 정보를 저장한다. */
typedef struct {
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    int select_all;
    int column_count;
    char column_names[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_NAME_LEN];
    SourceLocation column_locations[SQLPROC_MAX_COLUMNS];
    WhereClause where_clause;
} SelectStatement;

/* CREATE INDEX 문에서 필요한 인덱스명, 테이블명, 컬럼명을 저장한다. */
typedef struct {
    char index_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation index_location;
    char table_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation table_location;
    char column_name[SQLPROC_MAX_NAME_LEN];
    SourceLocation column_location;
} CreateIndexStatement;

/* 문장 종류와 실제 문장 데이터를 함께 담는 공용 AST 노드이다. */
typedef struct {
    StatementType type;
    SourceLocation location;
    InsertStatement insert_statement;
    SelectStatement select_statement;
    CreateIndexStatement create_index_statement;
} Statement;

/* 파싱된 SQL 문장 여러 개를 순서대로 저장하는 구조체이다. */
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
 * @return        성공 시 0, 오류 시 1
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
 * @param changed_index  업데이트된 인덱스 수를 저장할 포인터 ("하나라도 변경됐는지"를 나타내는 값)
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

/* SELECT 문의 WHERE 절에 사용할 인덱스를 탐색해 조건에 맞는 row 오프셋 후보를 수집한다.
 *
 * 인덱스를 찾지 못해도 오류로 처리하지 않고 성공으로 반환하며, 이 경우 *used_index는 0으로 설정된다.
 * 
 * 또한 인덱스 결과 수가 너무 많으면 인덱스 사용을 포기하고, full scan으로 되돌릴 수 있도록 *used_index를 0으로 돌려준다. (full scan fallback)
 *  
 * 
 * @param config        실행 설정 구조체 포인터
 * @param schema        테이블 스키마 포인터
 * @param statement     SELECT AST 포인터
 * @param offsets       인덱스 후보 row 오프셋을 저장할 배열
 * @param offset_count  저장된 오프셋 개수를 돌려줄 포인터
 * @param used_index    실제로 인덱스를 사용했는지 저장할 포인터
 * @param error         오류 정보 저장 포인터
 * @return              오류 없이 처리되면 1, 실제 오류가 나면 0
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
