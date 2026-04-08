#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/*
 * parser.c는 TokenList를 SqlProgram으로 바꾸는 모듈입니다.
 * 현재 지원 문장:
 * - INSERT
 * - SELECT
 */

typedef struct {
    const TokenList *tokens;
    int position;
} ParserState;

static void set_error(ErrorInfo *error, const Token *token, const char *message)
{
    /* 파서 오류는 현재 바라보는 토큰의 위치를 함께 기록합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = token->line;
    error->column = token->column;
}

static const Token *current_token(ParserState *state)
{
    /* 현재 파싱 위치의 토큰을 돌려줍니다. */
    return &state->tokens->items[state->position];
}

static const Token *previous_token(ParserState *state)
{
    /* 직전에 소비한 토큰을 돌려줍니다. */
    if (state->position == 0) {
        return &state->tokens->items[0];
    }
    return &state->tokens->items[state->position - 1];
}

static void advance_token(ParserState *state)
{
    /* EOF를 넘기지 않는 범위에서 다음 토큰으로 이동합니다. */
    if (state->position < state->tokens->count - 1) {
        state->position += 1;
    }
}

static int token_matches(ParserState *state, TokenType expected_type)
{
    /* 현재 토큰이 기대한 종류인지 단순 비교합니다. */
    return current_token(state)->type == expected_type;
}

static int consume_token(ParserState *state, TokenType expected_type, ErrorInfo *error, const char *message)
{
    /*
     * 현재 토큰이 expected_type이면 소비하고,
     * 아니면 주어진 메시지로 파서 오류를 만듭니다.
     */
    if (!token_matches(state, expected_type)) {
        set_error(error, current_token(state), message);
        return 0;
    }

    advance_token(state);
    return 1;
}

static int copy_name(char dest[SQLPROC_MAX_NAME_LEN],
                     SourceLocation *location,
                     const Token *token,
                     ErrorInfo *error)
{
    /* 토큰 문자열을 구조체 내부 name 필드로 복사하고 위치도 함께 저장합니다. */
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

static int parse_identifier(ParserState *state,
                            char dest[SQLPROC_MAX_NAME_LEN],
                            SourceLocation *location,
                            ErrorInfo *error)
{
    /* 현재 토큰이 식별자여야 하는 자리를 읽습니다. */
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

static int parse_literal(ParserState *state, LiteralValue *value, ErrorInfo *error)
{
    /* 숫자 또는 문자열 리터럴을 LiteralValue 구조체로 바꿉니다. */
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

static int parse_value_list(ParserState *state,
                            LiteralValue values[SQLPROC_MAX_COLUMNS],
                            int *value_count,
                            int expected_count,
                            ErrorInfo *error)
{
    int parsed_count;

    /*
     * VALUES (...) 내부의 리터럴 목록을 읽습니다.
     * expected_count >= 0 이면 개수가 정확히 일치해야 하고,
     * expected_count < 0 이면 개수 제한만 확인합니다.
     */
    parsed_count = 0;

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

static int parse_insert_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    InsertStatement *insert_statement;

    /*
     * 지원 예:
     * - INSERT INTO users (id, name) VALUES (1, 'kim')
     * - INSERT INTO users VALUES (1, 'kim', 20)
     */
    insert_statement = &statement->insert_statement;
    memset(insert_statement, 0, sizeof(*insert_statement));
    statement->type = STATEMENT_INSERT;
    statement->location.line = current_token(state)->line;
    statement->location.column = current_token(state)->column;

    if (!consume_token(state, TOKEN_KEYWORD_INSERT, error, "INSERT 키워드가 필요합니다.")) {
        return 0;
    }

    if (!consume_token(state, TOKEN_KEYWORD_INTO, error, "INTO 키워드가 필요합니다.")) {
        return 0;
    }

    if (!parse_identifier(state,
                          insert_statement->table_name,
                          &insert_statement->table_location,
                          error)) {
        return 0;
    }

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

            if (!parse_identifier(state,
                                  insert_statement->column_names[insert_statement->column_count],
                                  &insert_statement->column_locations[insert_statement->column_count],
                                  error)) {
                return 0;
            }

            insert_statement->column_count += 1;

            if (!token_matches(state, TOKEN_COMMA)) {
                break;
            }

            advance_token(state);
        }

        if (!consume_token(state, TOKEN_RPAREN, error, ") 가 필요합니다.")) {
            return 0;
        }
    }

    if (!consume_token(state, TOKEN_KEYWORD_VALUES, error, "VALUES 키워드가 필요합니다.")) {
        return 0;
    }

    if (!consume_token(state, TOKEN_LPAREN, error, "( 가 필요합니다.")) {
        return 0;
    }

    if (!parse_value_list(state,
                          insert_statement->values,
                          &insert_statement->value_count,
                          insert_statement->has_column_list ? insert_statement->column_count : -1,
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

    /* SELECT * 또는 SELECT col1, col2 형태를 읽고 뒤에 FROM을 연결합니다. */
    select_statement = &statement->select_statement;
    memset(select_statement, 0, sizeof(*select_statement));
    statement->type = STATEMENT_SELECT;
    statement->location.line = current_token(state)->line;
    statement->location.column = current_token(state)->column;

    if (!consume_token(state, TOKEN_KEYWORD_SELECT, error, "SELECT 키워드가 필요합니다.")) {
        return 0;
    }

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

            if (!parse_identifier(state,
                                  select_statement->column_names[select_statement->column_count],
                                  &select_statement->column_locations[select_statement->column_count],
                                  error)) {
                return 0;
            }

            select_statement->column_count += 1;

            if (!token_matches(state, TOKEN_COMMA)) {
                break;
            }

            advance_token(state);
        }
    }

    if (!consume_token(state, TOKEN_KEYWORD_FROM, error, "FROM 키워드가 필요합니다.")) {
        return 0;
    }

    return parse_identifier(state, select_statement->table_name, NULL, error);
}

static int parse_statement(ParserState *state, Statement *statement, ErrorInfo *error)
{
    /* 현재 토큰의 시작 키워드를 보고 어떤 문장 파서를 호출할지 결정합니다. */
    if (token_matches(state, TOKEN_KEYWORD_INSERT)) {
        return parse_insert_statement(state, statement, error);
    }

    if (token_matches(state, TOKEN_KEYWORD_SELECT)) {
        return parse_select_statement(state, statement, error);
    }

    set_error(error, current_token(state), "지원하지 않는 SQL 문장입니다.");
    return 0;
}

int parse_program(const TokenList *tokens, SqlProgram *program, ErrorInfo *error)
{
    ParserState state;

    /*
     * 토큰 배열 전체를 끝까지 읽어 여러 SQL 문장을 SqlProgram에 담습니다.
     * 각 문장은 세미콜론으로 끝나야 합니다.
     */
    memset(program, 0, sizeof(*program));
    memset(error, 0, sizeof(*error));

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

        if (!consume_token(&state,
                           TOKEN_SEMICOLON,
                           error,
                           "문장 끝에는 세미콜론이 필요합니다.")) {
            return 0;
        }
    }

    return 1;
}

