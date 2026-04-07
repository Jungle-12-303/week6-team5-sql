#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

typedef struct {
    const TokenList *tokens;
    int position;
} ParserState;

static void set_error(ErrorInfo *error, const Token *token, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = token->line;
    error->column = token->column;
}

static const Token *current_token(ParserState *state)
{
    return &state->tokens->items[state->position];
}

static const Token *previous_token(ParserState *state)
{
    return &state->tokens->items[state->position - 1];
}

static int advance_token(ParserState *state)
{
    if (state->position < state->tokens->count - 1) {
        state->position += 1;
    }

    return 1;
}

static int token_matches(ParserState *state, TokenType expected_type)
{
    return current_token(state)->type == expected_type;
}

static int consume_token(ParserState *state, TokenType expected_type, ErrorInfo *error, const char *message)
{
    if (!token_matches(state, expected_type)) {
        set_error(error, current_token(state), message);
        return 0;
    }

    advance_token(state);
    return 1;
}

static int copy_name(char dest[SQLPROC_MAX_NAME_LEN], const Token *token, ErrorInfo *error)
{
    if ((int)strlen(token->text) >= SQLPROC_MAX_NAME_LEN) {
        set_error(error, token, "이름 길이가 너무 깁니다.");
        return 0;
    }

    snprintf(dest, SQLPROC_MAX_NAME_LEN, "%s", token->text);
    return 1;
}

static int parse_identifier(ParserState *state, char dest[SQLPROC_MAX_NAME_LEN], ErrorInfo *error)
{
    if (!token_matches(state, TOKEN_IDENTIFIER)) {
        set_error(error, current_token(state), "식별자가 필요합니다.");
        return 0;
    }

    if (!copy_name(dest, current_token(state), error)) {
        return 0;
    }

    advance_token(state);
    return 1;
}

static int parse_literal(ParserState *state, LiteralValue *value, ErrorInfo *error)
{
    if (token_matches(state, TOKEN_NUMBER)) {
        value->type = LITERAL_INT;
        snprintf(value->text, sizeof(value->text), "%s", current_token(state)->text);
        advance_token(state);
        return 1;
    }

    if (token_matches(state, TOKEN_STRING)) {
        value->type = LITERAL_STRING;
        snprintf(value->text, sizeof(value->text), "%s", current_token(state)->text);
        advance_token(state);
        return 1;
    }

    set_error(error, current_token(state), "정수 또는 문자열 리터럴이 필요합니다.");
    return 0;
}

static int parse_operator(ParserState *state, CompareOperator *operator_type, ErrorInfo *error)
{
    if (token_matches(state, TOKEN_EQUAL)) {
        *operator_type = COMPARE_EQUAL;
        advance_token(state);
        return 1;
    }

    if (token_matches(state, TOKEN_LESS)) {
        *operator_type = COMPARE_LESS;
        advance_token(state);
        return 1;
    }

    if (token_matches(state, TOKEN_LESS_EQUAL)) {
        *operator_type = COMPARE_LESS_EQUAL;
        advance_token(state);
        return 1;
    }

    if (token_matches(state, TOKEN_GREATER)) {
        *operator_type = COMPARE_GREATER;
        advance_token(state);
        return 1;
    }

    if (token_matches(state, TOKEN_GREATER_EQUAL)) {
        *operator_type = COMPARE_GREATER_EQUAL;
        advance_token(state);
        return 1;
    }

    set_error(error, current_token(state), "비교 연산자가 필요합니다.");
    return 0;
}

static int parse_predicate(ParserState *state, Predicate *predicate, ErrorInfo *error)
{
    if (!parse_identifier(state, predicate->column_name, error)) {
        return 0;
    }

    if (!parse_operator(state, &predicate->operator_type, error)) {
        return 0;
    }

    return parse_literal(state, &predicate->value, error);
}

/*
 * 무엇을 하는가:
 * - SELECT 문의 WHERE 절을 읽어 최대 2개의 조건을 구조화합니다.
 *
 * 왜 필요한가:
 * - 다음 단계의 실행기와 인덱스 선택 로직이 WHERE 정보를 일정한 모양으로
 *   받을 수 있어야 하기 때문입니다.
 *
 * 입력과 출력:
 * - 입력: 현재 토큰 위치가 WHERE 키워드를 가리키는 파서 상태
 * - 출력: WhereClause 구조체에 조건 정보를 채운 뒤 성공 여부를 반환
 *
 * 핵심 흐름:
 * - 첫 번째 조건을 읽고, AND가 나오면 두 번째 조건까지 한 번 더 읽습니다.
 * - 세 번째 조건은 허용하지 않으므로 바로 에러로 처리합니다.
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

static int parse_column_name_list(ParserState *state,
                                  char column_names[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_NAME_LEN],
                                  int *column_count,
                                  ErrorInfo *error)
{
    *column_count = 0;

    while (1) {
        if (*column_count >= SQLPROC_MAX_COLUMNS) {
            set_error(error, current_token(state), "컬럼 수가 최대 개수를 넘었습니다.");
            return 0;
        }

        if (!parse_identifier(state, column_names[*column_count], error)) {
            return 0;
        }

        *column_count += 1;

        if (!token_matches(state, TOKEN_COMMA)) {
            break;
        }

        advance_token(state);
    }

    return 1;
}

static int parse_value_list(ParserState *state,
                            LiteralValue values[SQLPROC_MAX_COLUMNS],
                            int expected_count,
                            ErrorInfo *error)
{
    int value_count;

    value_count = 0;

    while (1) {
        if (value_count >= expected_count) {
            set_error(error, current_token(state), "값 수가 컬럼 수보다 많습니다.");
            return 0;
        }

        if (!parse_literal(state, &values[value_count], error)) {
            return 0;
        }

        value_count += 1;

        if (!token_matches(state, TOKEN_COMMA)) {
            break;
        }

        advance_token(state);
    }

    if (value_count != expected_count) {
        set_error(error, previous_token(state), "컬럼 수와 값 수가 일치하지 않습니다.");
        return 0;
    }

    return 1;
}

static int parse_insert_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    InsertStatement *insert_statement;

    insert_statement = &statement->insert_statement;
    memset(insert_statement, 0, sizeof(*insert_statement));
    statement->type = STATEMENT_INSERT;

    if (!consume_token(state, TOKEN_KEYWORD_INSERT, error, "INSERT 키워드가 필요합니다.")) {
        return 0;
    }

    if (!consume_token(state, TOKEN_KEYWORD_INTO, error, "INTO 키워드가 필요합니다.")) {
        return 0;
    }

    if (!parse_identifier(state, insert_statement->table_name, error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_LPAREN, error, "( 가 필요합니다.")) {
        return 0;
    }

    if (!parse_column_name_list(state,
                                insert_statement->column_names,
                                &insert_statement->column_count,
                                error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) {
        return 0;
    }

    if (!consume_token(state, TOKEN_KEYWORD_VALUES, error, "VALUES 키워드가 필요합니다.")) {
        return 0;
    }

    if (!consume_token(state, TOKEN_LPAREN, error, "( 가 필요합니다.")) {
        return 0;
    }

    if (!parse_value_list(state,
                          insert_statement->values,
                          insert_statement->column_count,
                          error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) {
        return 0;
    }

    return 1;
}

static int parse_select_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    SelectStatement *select_statement;

    select_statement = &statement->select_statement;
    memset(select_statement, 0, sizeof(*select_statement));
    statement->type = STATEMENT_SELECT;

    if (!consume_token(state, TOKEN_KEYWORD_SELECT, error, "SELECT 키워드가 필요합니다.")) {
        return 0;
    }

    if (token_matches(state, TOKEN_STAR)) {
        select_statement->select_all = 1;
        advance_token(state);
    } else {
        if (!parse_column_name_list(state,
                                    select_statement->column_names,
                                    &select_statement->column_count,
                                    error)) {
            return 0;
        }
    }

    if (!consume_token(state, TOKEN_KEYWORD_FROM, error, "FROM 키워드가 필요합니다.")) {
        return 0;
    }

    if (!parse_identifier(state, select_statement->table_name, error)) {
        return 0;
    }

    return parse_where_clause(state, &select_statement->where_clause, error);
}

static int parse_create_index_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    CreateIndexStatement *create_index_statement;

    create_index_statement = &statement->create_index_statement;
    memset(create_index_statement, 0, sizeof(*create_index_statement));
    statement->type = STATEMENT_CREATE_INDEX;

    if (!consume_token(state, TOKEN_KEYWORD_CREATE, error, "CREATE 키워드가 필요합니다.")) {
        return 0;
    }

    if (!consume_token(state, TOKEN_KEYWORD_INDEX, error, "INDEX 키워드가 필요합니다.")) {
        return 0;
    }

    if (!parse_identifier(state, create_index_statement->index_name, error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_KEYWORD_ON, error, "ON 키워드가 필요합니다.")) {
        return 0;
    }

    if (!parse_identifier(state, create_index_statement->table_name, error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_LPAREN, error, "( 가 필요합니다.")) {
        return 0;
    }

    if (!parse_identifier(state, create_index_statement->column_name, error)) {
        return 0;
    }

    if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) {
        return 0;
    }

    return 1;
}

static int parse_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    if (token_matches(state, TOKEN_KEYWORD_INSERT)) {
        return parse_insert_statement(state, statement, error);
    }

    if (token_matches(state, TOKEN_KEYWORD_SELECT)) {
        return parse_select_statement(state, statement, error);
    }

    if (token_matches(state, TOKEN_KEYWORD_CREATE)) {
        return parse_create_index_statement(state, statement, error);
    }

    set_error(error, current_token(state), "지원하지 않는 SQL 문장입니다.");
    return 0;
}

int parse_program(const TokenList *tokens, SqlProgram *program, ErrorInfo *error)
{
    ParserState state;

    memset(program, 0, sizeof(*program));
    memset(error, 0, sizeof(*error));

    state.tokens = tokens;
    state.position = 0;

    while (!token_matches(&state, TOKEN_EOF)) {
        if (program->count >= SQLPROC_MAX_STATEMENTS) {
            set_error(error, current_token(&state), "문장 수가 최대 개수를 넘었습니다.");
            return 0;
        }

        if (!parse_statement(&state, &program->items[program->count], error)) {
            return 0;
        }

        program->count += 1;

        if (!consume_token(&state,
                           TOKEN_SEMICOLON,
                           error,
                           "문장 끝에는 세미콜론이 필요합니다.")) {
            return 0;
        }
    }

    return 1;
}

const char *statement_type_name(StatementType type)
{
    if (type == STATEMENT_INSERT) {
        return "INSERT";
    }

    if (type == STATEMENT_SELECT) {
        return "SELECT";
    }

    if (type == STATEMENT_CREATE_INDEX) {
        return "CREATE INDEX";
    }

    return "UNKNOWN";
}

const char *compare_operator_name(CompareOperator operator_type)
{
    if (operator_type == COMPARE_EQUAL) {
        return "=";
    }

    if (operator_type == COMPARE_LESS) {
        return "<";
    }

    if (operator_type == COMPARE_LESS_EQUAL) {
        return "<=";
    }

    if (operator_type == COMPARE_GREATER) {
        return ">";
    }

    if (operator_type == COMPARE_GREATER_EQUAL) {
        return ">=";
    }

    return "?";
}
