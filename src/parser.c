#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

typedef struct {
    const TokenList *tokens;
    int position;
} ParserState;

/* 현재 토큰 위치 정보를 포함한 오류 메시지를 ErrorInfo에 저장한다.
 *
 * @param error    오류 정보를 저장할 포인터
 * @param token    오류가 발생한 토큰 포인터
 * @param message  오류 메시지 문자열
 */
static void set_error(ErrorInfo *error, const Token *token, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = token->line;
    error->column = token->column;
}

/* 현재 파싱 위치의 토큰을 반환한다.
 *
 * @param state  파서 상태 포인터
 * @return       현재 위치 토큰 포인터
 */
static const Token *current_token(ParserState *state)
{
    return &state->tokens->items[state->position];
}

/* 직전에 소비한 토큰을 반환한다.
 *
 * @param state  파서 상태 포인터
 * @return       이전 위치 토큰 포인터
 */
static const Token *previous_token(ParserState *state)
{
    return &state->tokens->items[state->position - 1];
}

/* EOF를 넘지 않는 범위에서 다음 토큰으로 위치를 이동한다.
 *
 * @param state  파서 상태 포인터
 */
static void advance_token(ParserState *state)
{
    if (state->position < state->tokens->count - 1) {
        state->position += 1;
    }
}

/* 현재 토큰이 지정한 종류인지 확인한다.
 *
 * @param state          파서 상태 포인터
 * @param expected_type  기대하는 토큰 종류
 * @return               일치하면 1, 아니면 0
 */
static int token_matches(ParserState *state, TokenType expected_type)
{
    return current_token(state)->type == expected_type;
}

/* 현재 토큰이 기대한 종류이면 소비하고, 아니면 오류를 기록한다.
 *
 * @param state          파서 상태 포인터
 * @param expected_type  기대하는 토큰 종류
 * @param error          오류 정보 저장 포인터
 * @param message        기대 불일치 시 오류 메시지
 * @return               성공 시 1, 실패 시 0
 */
static int consume_token(ParserState *state, TokenType expected_type, ErrorInfo *error, const char *message)
{
    if (!token_matches(state, expected_type)) {
        set_error(error, current_token(state), message);
        return 0;
    }
    advance_token(state);
    return 1;
}

/* 토큰 텍스트를 AST의 name 필드에 복사하고 소스 위치도 저장한다.
 *
 * @param dest      복사 대상 이름 버퍼
 * @param location  위치 정보를 저장할 포인터 (NULL 허용)
 * @param token     원본 토큰 포인터
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 이름이 너무 길면 0
 */
static int copy_name(char dest[SQLPROC_MAX_NAME_LEN],
                     SourceLocation *location,
                     const Token *token,
                     ErrorInfo *error)
{
    if ((int)strlen(token->text) >= SQLPROC_MAX_NAME_LEN) {
        set_error(error, token, "이름 길이가 너무 깁니다.");
        return 0;
    }
    snprintf(dest, SQLPROC_MAX_NAME_LEN, "%s", token->text);
    if (location != NULL) {
        location->line = token->line;
        location->column = token->column;
    }
    return 1;
}

/* 식별자 토큰을 읽어 이름과 소스 위치를 저장한다.
 *
 * @param state     파서 상태 포인터
 * @param dest      이름을 저장할 버퍼
 * @param location  위치 정보를 저장할 포인터
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
static int parse_identifier(ParserState *state,
                            char dest[SQLPROC_MAX_NAME_LEN],
                            SourceLocation *location,
                            ErrorInfo *error)
{
    if (!token_matches(state, TOKEN_IDENTIFIER)) {
        set_error(error, current_token(state), "식별자가 필요합니다.");
        return 0;
    }
    if (!copy_name(dest, location, current_token(state), error)) {
        return 0;
    }
    advance_token(state);
    return 1;
}

/* 정수 또는 문자열 리터럴 토큰을 읽어 LiteralValue에 저장한다.
 *
 * @param state  파서 상태 포인터
 * @param value  결과를 저장할 LiteralValue 포인터
 * @param error  오류 정보 저장 포인터
 * @return       성공 시 1, 리터럴이 아닌 토큰이면 0
 */
static int parse_literal(ParserState *state, LiteralValue *value, ErrorInfo *error)
{
    if (token_matches(state, TOKEN_NUMBER)) {
        value->type = LITERAL_INT;
        snprintf(value->text, sizeof(value->text), "%s", current_token(state)->text);
        value->location.line = current_token(state)->line;
        value->location.column = current_token(state)->column;
        advance_token(state);
        return 1;
    }

    if (token_matches(state, TOKEN_STRING)) {
        value->type = LITERAL_STRING;
        snprintf(value->text, sizeof(value->text), "%s", current_token(state)->text);
        value->location.line = current_token(state)->line;
        value->location.column = current_token(state)->column;
        advance_token(state);
        return 1;
    }

    set_error(error, current_token(state), "정수 또는 문자열 리터럴이 필요합니다.");
    return 0;
}

/* WHERE 절에서 비교 연산자 토큰을 읽어 CompareOperator에 저장한다.
 *
 * @param state          파서 상태 포인터
 * @param operator_type  결과를 저장할 CompareOperator 포인터
 * @param error          오류 정보 저장 포인터
 * @return               성공 시 1, 비교 연산자가 아니면 0
 */
static int parse_operator(ParserState *state, CompareOperator *operator_type, ErrorInfo *error)
{
    if (token_matches(state, TOKEN_EQUAL))         { *operator_type = COMPARE_EQUAL;         advance_token(state); return 1; }
    if (token_matches(state, TOKEN_LESS))          { *operator_type = COMPARE_LESS;          advance_token(state); return 1; }
    if (token_matches(state, TOKEN_LESS_EQUAL))    { *operator_type = COMPARE_LESS_EQUAL;    advance_token(state); return 1; }
    if (token_matches(state, TOKEN_GREATER))       { *operator_type = COMPARE_GREATER;       advance_token(state); return 1; }
    if (token_matches(state, TOKEN_GREATER_EQUAL)) { *operator_type = COMPARE_GREATER_EQUAL; advance_token(state); return 1; }

    set_error(error, current_token(state), "비교 연산자가 필요합니다.");
    return 0;
}

/* WHERE 조건 1개를 "컬럼 연산자 리터럴" 형태로 읽어 Predicate에 저장한다.
 *
 * @param state      파서 상태 포인터
 * @param predicate  결과를 저장할 Predicate 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int parse_predicate(ParserState *state, Predicate *predicate, ErrorInfo *error)
{
    if (!parse_identifier(state, predicate->column_name, &predicate->column_location, error)) {
        return 0;
    }
    predicate->operator_location.line = current_token(state)->line;
    predicate->operator_location.column = current_token(state)->column;
    if (!parse_operator(state, &predicate->operator_type, error)) {
        return 0;
    }
    return parse_literal(state, &predicate->value, error);
}

/* WHERE 절 전체를 읽어 최대 2개의 조건을 WhereClause에 저장한다.
 * WHERE 키워드가 없으면 빈 WhereClause를 반환한다.
 * 세 번째 AND 조건은 오류로 처리한다.
 *
 * @param state        파서 상태 포인터
 * @param where_clause 결과를 저장할 WhereClause 포인터
 * @param error        오류 정보 저장 포인터
 * @return             성공 시 1, 실패 시 0
 */
static int parse_where_clause(ParserState *state, WhereClause *where_clause, ErrorInfo *error)
{
    memset(where_clause, 0, sizeof(*where_clause));

    if (!token_matches(state, TOKEN_KEYWORD_WHERE)) {
        return 1;
    }
    advance_token(state);

    if (!parse_predicate(state, &where_clause->items[0], error)) {
        return 0;
    }
    where_clause->count = 1;

    if (token_matches(state, TOKEN_KEYWORD_AND)) {
        advance_token(state);
        if (!parse_predicate(state, &where_clause->items[1], error)) {
            return 0;
        }
        where_clause->count = 2;

        if (token_matches(state, TOKEN_KEYWORD_AND)) {
            set_error(error, current_token(state), "WHERE 조건은 최대 2개까지만 지원합니다.");
            return 0;
        }
    }

    return 1;
}

/* VALUES 괄호 안의 리터럴 목록을 읽어 values 배열에 저장한다.
 * expected_count >= 0이면 파싱된 개수가 정확히 일치해야 한다.
 *
 * @param state          파서 상태 포인터
 * @param values         결과 리터럴을 저장할 배열
 * @param value_count    파싱된 리터럴 개수를 저장할 포인터
 * @param expected_count 기대 개수 (음수이면 개수 무제한)
 * @param error          오류 정보 저장 포인터
 * @return               성공 시 1, 실패 시 0
 */
static int parse_value_list(ParserState *state,
                            LiteralValue values[SQLPROC_MAX_COLUMNS],
                            int *value_count,
                            int expected_count,
                            ErrorInfo *error)
{
    int parsed_count = 0;

    while (1) {
        if (parsed_count >= SQLPROC_MAX_COLUMNS) {
            set_error(error, current_token(state), "값 수가 최대 개수를 넘었습니다.");
            return 0;
        }
        if (expected_count >= 0 && parsed_count >= expected_count) {
            set_error(error, current_token(state), "값 수가 컬럼 수보다 많습니다.");
            return 0;
        }
        if (!parse_literal(state, &values[parsed_count], error)) {
            return 0;
        }
        parsed_count += 1;
        if (!token_matches(state, TOKEN_COMMA)) {
            break;
        }
        advance_token(state);
    }

    if (expected_count >= 0 && parsed_count != expected_count) {
        set_error(error, previous_token(state), "컬럼 수와 값 수가 일치하지 않습니다.");
        return 0;
    }
    *value_count = parsed_count;
    return 1;
}

/* INSERT 문을 파싱하여 Statement에 저장한다.
 * 컬럼 목록이 있는 형태와 없는 형태 모두 지원한다.
 *
 * @param state      파서 상태 포인터
 * @param statement  결과를 저장할 Statement 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int parse_insert_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    InsertStatement *insert_statement = &statement->insert_statement;
    memset(insert_statement, 0, sizeof(*insert_statement));
    statement->type = STATEMENT_INSERT;
    statement->location.line = current_token(state)->line;
    statement->location.column = current_token(state)->column;

    if (!consume_token(state, TOKEN_KEYWORD_INSERT, error, "INSERT 키워드가 필요합니다.")) return 0;
    if (!consume_token(state, TOKEN_KEYWORD_INTO,   error, "INTO 키워드가 필요합니다."))   return 0;
    if (!parse_identifier(state, insert_statement->table_name, &insert_statement->table_location, error)) return 0;

    insert_statement->column_count = 0;
    insert_statement->value_count = 0;

    if (token_matches(state, TOKEN_LPAREN)) {
        insert_statement->has_column_list = 1;
        advance_token(state);

        while (1) {
            if (insert_statement->column_count >= SQLPROC_MAX_COLUMNS) {
                set_error(error, current_token(state), "컬럼 수가 최대 개수를 넘었습니다.");
                return 0;
            }
            int col = insert_statement->column_count;
            if (!parse_identifier(state,
                                  insert_statement->column_names[col],
                                  &insert_statement->column_locations[col],
                                  error)) {
                return 0;
            }
            insert_statement->column_count += 1;
            if (!token_matches(state, TOKEN_COMMA)) break;
            advance_token(state);
        }

        if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) return 0;
    }

    if (!consume_token(state, TOKEN_KEYWORD_VALUES, error, "VALUES 키워드가 필요합니다.")) return 0;
    if (!consume_token(state, TOKEN_LPAREN,          error, "( 가 필요합니다."))           return 0;

    int expected = insert_statement->has_column_list ? insert_statement->column_count : -1;
    if (!parse_value_list(state, insert_statement->values, &insert_statement->value_count, expected, error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) return 0;
    return 1;
}

/* SELECT 문을 파싱하여 Statement에 저장한다.
 * SELECT * 와 SELECT col1, col2 형태를 모두 지원한다.
 *
 * @param state      파서 상태 포인터
 * @param statement  결과를 저장할 Statement 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int parse_select_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    SelectStatement *select_statement = &statement->select_statement;
    memset(select_statement, 0, sizeof(*select_statement));
    statement->type = STATEMENT_SELECT;
    statement->location.line = current_token(state)->line;
    statement->location.column = current_token(state)->column;

    if (!consume_token(state, TOKEN_KEYWORD_SELECT, error, "SELECT 키워드가 필요합니다.")) return 0;

    if (token_matches(state, TOKEN_STAR)) {
        select_statement->select_all = 1;
        advance_token(state);
    } else {
        select_statement->column_count = 0;
        while (1) {
            if (select_statement->column_count >= SQLPROC_MAX_COLUMNS) {
                set_error(error, current_token(state), "컬럼 수가 최대 개수를 넘었습니다.");
                return 0;
            }
            int col = select_statement->column_count;
            if (!parse_identifier(state,
                                  select_statement->column_names[col],
                                  &select_statement->column_locations[col],
                                  error)) {
                return 0;
            }
            select_statement->column_count += 1;
            if (!token_matches(state, TOKEN_COMMA)) break;
            advance_token(state);
        }
    }

    if (!consume_token(state, TOKEN_KEYWORD_FROM, error, "FROM 키워드가 필요합니다.")) return 0;
    if (!parse_identifier(state, select_statement->table_name, &select_statement->table_location, error)) return 0;

    return parse_where_clause(state, &select_statement->where_clause, error);
}

/* CREATE INDEX 문을 파싱하여 Statement에 저장한다.
 *
 * @param state      파서 상태 포인터
 * @param statement  결과를 저장할 Statement 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int parse_create_index_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    CreateIndexStatement *ci = &statement->create_index_statement;
    memset(ci, 0, sizeof(*ci));
    statement->type = STATEMENT_CREATE_INDEX;
    statement->location.line = current_token(state)->line;
    statement->location.column = current_token(state)->column;

    if (!consume_token(state, TOKEN_KEYWORD_CREATE, error, "CREATE 키워드가 필요합니다.")) return 0;
    if (!consume_token(state, TOKEN_KEYWORD_INDEX,  error, "INDEX 키워드가 필요합니다."))  return 0;
    if (!parse_identifier(state, ci->index_name, &ci->index_location, error)) return 0;
    if (!consume_token(state, TOKEN_KEYWORD_ON, error, "ON 키워드가 필요합니다.")) return 0;
    if (!parse_identifier(state, ci->table_name, &ci->table_location, error)) return 0;
    if (!consume_token(state, TOKEN_LPAREN, error, "( 가 필요합니다.")) return 0;
    if (!parse_identifier(state, ci->column_name, &ci->column_location, error)) return 0;
    if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) return 0;
    return 1;
}

/* 현재 토큰의 시작 키워드를 보고 적합한 문장 파서를 호출한다.
 *
 * @param state      파서 상태 포인터
 * @param statement  결과를 저장할 Statement 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 지원하지 않는 문장이면 0
 */
static int parse_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    if (token_matches(state, TOKEN_KEYWORD_INSERT)) return parse_insert_statement(state, statement, error);
    if (token_matches(state, TOKEN_KEYWORD_SELECT)) return parse_select_statement(state, statement, error);
    if (token_matches(state, TOKEN_KEYWORD_CREATE)) return parse_create_index_statement(state, statement, error);

    set_error(error, current_token(state), "지원하지 않는 SQL 문장입니다.");
    return 0;
}

/* 토큰 목록 전체를 파싱하여 SQL 문장 목록을 SqlProgram에 저장한다.
 * 각 문장은 세미콜론으로 끝나야 한다.
 *
 * @param tokens   입력 토큰 목록 포인터
 * @param program  결과 AST를 저장할 SqlProgram 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
int parse_program(const TokenList *tokens, SqlProgram *program, ErrorInfo *error)
{
    memset(program, 0, sizeof(*program));
    memset(error, 0, sizeof(*error));

    ParserState state;
    state.tokens = tokens;
    state.position = 0;

    if (token_matches(&state, TOKEN_EOF)) {
        set_error(error, current_token(&state), "SQL 문장이 비어 있습니다.");
        return 0;
    }

    while (!token_matches(&state, TOKEN_EOF)) {
        if (program->count >= SQLPROC_MAX_STATEMENTS) {
            set_error(error, current_token(&state), "문장 수가 최대 개수를 넘었습니다.");
            return 0;
        }
        if (!parse_statement(&state, &program->items[program->count], error)) {
            return 0;
        }
        program->count += 1;
        if (!consume_token(&state, TOKEN_SEMICOLON, error, "문장 끝에는 세미콜론이 필요합니다.")) {
            return 0;
        }
    }

    return 1;
}

/* StatementType 열거값에 대응하는 이름 문자열을 반환한다.
 *
 * @param type  문장 종류 열거값
 * @return      해당 열거값의 이름 문자열 (알 수 없으면 "UNKNOWN")
 */
const char *statement_type_name(StatementType type)
{
    if (type == STATEMENT_INSERT)       return "INSERT";
    if (type == STATEMENT_SELECT)       return "SELECT";
    if (type == STATEMENT_CREATE_INDEX) return "CREATE INDEX";
    return "UNKNOWN";
}

/* CompareOperator 열거값에 대응하는 연산자 문자열을 반환한다.
 *
 * @param operator_type  비교 연산자 열거값
 * @return               해당 연산자 문자열 (알 수 없으면 "?")
 */
const char *compare_operator_name(CompareOperator operator_type)
{
    if (operator_type == COMPARE_EQUAL)         return "=";
    if (operator_type == COMPARE_LESS)          return "<";
    if (operator_type == COMPARE_LESS_EQUAL)    return "<=";
    if (operator_type == COMPARE_GREATER)       return ">";
    if (operator_type == COMPARE_GREATER_EQUAL) return ">=";
    return "?";
}
