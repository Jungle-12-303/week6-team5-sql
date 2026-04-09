#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/*
 * tokenizer.c는 SQL 문자열을 TokenList로 자르는 모듈입니다.
 * 파서는 원본 문자열 대신 이 토큰 배열을 입력으로 받아 문법을 해석합니다.
 */

static void set_error(ErrorInfo *error, const char *message, int line, int column)
{
    /* 토크나이저 오류는 현재 줄/열 번호를 함께 저장합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = line;
    error->column = column;
}

static void to_lowercase_copy(char *dest, size_t dest_size, const char *src)
{
    size_t i;

    /* 키워드 비교와 식별자 정규화를 위해 소문자 복사본을 만듭니다. */
    for (i = 0; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = (char)tolower((unsigned char)src[i]);
    }

    dest[i] = '\0';
}

static TokenType keyword_type(const char *text)
{
    /* 읽은 단어가 예약어인지 일반 식별자인지 구분합니다. */
    if (strcmp(text, "insert") == 0) {
        return TOKEN_KEYWORD_INSERT;
    }

    if (strcmp(text, "into") == 0) {
        return TOKEN_KEYWORD_INTO;
    }

    if (strcmp(text, "values") == 0) {
        return TOKEN_KEYWORD_VALUES;
    }

    if (strcmp(text, "select") == 0) {
        return TOKEN_KEYWORD_SELECT;
    }

    if (strcmp(text, "from") == 0) {
        return TOKEN_KEYWORD_FROM;
    }

    return TOKEN_IDENTIFIER;
}

static int append_token(TokenList *tokens,
                        TokenType type,
                        const char *text,
                        int line,
                        int column,
                        ErrorInfo *error)
{
    Token *token;

    /* TokenList 뒤에 새 토큰 1개를 추가합니다. */
    if (tokens->count >= SQLPROC_MAX_TOKENS) {
        set_error(error, "토큰 수가 최대 개수를 넘었습니다.", line, column);
        return 0;
    }

    token = &tokens->items[tokens->count];
    token->type = type;
    snprintf(token->text, sizeof(token->text), "%s", text);
    token->line = line;
    token->column = column;
    tokens->count += 1;
    return 1;
}

static int read_word(const char *sql_text,
                     int *index,
                     int line,
                     int column,
                     TokenList *tokens,
                     ErrorInfo *error)
{
    char raw_text[SQLPROC_MAX_VALUE_LEN];
    char lower_text[SQLPROC_MAX_VALUE_LEN];
    int start;
    int length;
    TokenType type;

    /*
     * 알파벳/숫자/밑줄로 이어진 단어를 읽습니다.
     * 예: users, select
     */
    start = *index;
    while (isalnum((unsigned char)sql_text[*index]) || sql_text[*index] == '_') {
        *index += 1;
    }

    length = *index - start;
    if (length >= (int)sizeof(raw_text)) {
        set_error(error, "식별자 길이가 너무 깁니다.", line, column);
        return 0;
    }

    memcpy(raw_text, sql_text + start, (size_t)length);
    raw_text[length] = '\0';
    to_lowercase_copy(lower_text, sizeof(lower_text), raw_text);
    type = keyword_type(lower_text);

    /* 식별자는 소문자로 저장하고, 키워드는 읽은 형태 그대로 토큰 종류만 바꿉니다. */
    if (type == TOKEN_IDENTIFIER) {
        return append_token(tokens, type, lower_text, line, column, error);
    }

    return append_token(tokens, type, raw_text, line, column, error);
}

static int read_number(const char *sql_text,
                       int *index,
                       int line,
                       int column,
                       TokenList *tokens,
                       ErrorInfo *error)
{
    char number_text[SQLPROC_MAX_VALUE_LEN];
    int start;
    int length;

    /* 정수 리터럴을 읽습니다. 현재 구현은 선행 '-'도 허용합니다. */
    start = *index;

    if (sql_text[*index] == '-') {
        *index += 1;
    }

    while (isdigit((unsigned char)sql_text[*index])) {
        *index += 1;
    }

    length = *index - start;
    if (length <= 0 || length >= (int)sizeof(number_text)) {
        set_error(error, "숫자 리터럴이 잘못되었습니다.", line, column);
        return 0;
    }

    memcpy(number_text, sql_text + start, (size_t)length);
    number_text[length] = '\0';
    return append_token(tokens, TOKEN_NUMBER, number_text, line, column, error);
}

static int read_string(const char *sql_text,
                       int *index,
                       int line,
                       int column,
                       TokenList *tokens,
                       ErrorInfo *error)
{
    char string_text[SQLPROC_MAX_VALUE_LEN];
    int text_index;

    /* 작은따옴표로 감싼 한 줄 문자열 리터럴을 읽고, 내부 내용만 토큰에 저장합니다. */
    *index += 1;
    text_index = 0;

    while (sql_text[*index] != '\0' && sql_text[*index] != '\'') {
        if (sql_text[*index] == '\n' || sql_text[*index] == '\r') {
            set_error(error,
                      "문자열 리터럴 안에는 줄바꿈을 넣을 수 없습니다.",
                      line,
                      column + text_index + 1);
            return 0;
        }

        if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
            set_error(error, "문자열 길이가 최대 길이를 넘었습니다.", line, column);
            return 0;
        }

        string_text[text_index] = sql_text[*index];
        text_index += 1;
        *index += 1;
    }

    if (sql_text[*index] != '\'') {
        set_error(error, "문자열 리터럴이 닫히지 않았습니다.", line, column);
        return 0;
    }

    string_text[text_index] = '\0';
    *index += 1;
    return append_token(tokens, TOKEN_STRING, string_text, line, column, error);
}

static int read_symbol(const char *sql_text,
                       int *index,
                       int line,
                       int column,
                       TokenList *tokens,
                       ErrorInfo *error)
{
    char text[2];

    /*
     * 기호 토큰을 읽습니다.
     * 지원 기호: , ; ( ) *
     */
    text[0] = sql_text[*index];
    text[1] = '\0';

    if (sql_text[*index] == ',') {
        *index += 1;
        return append_token(tokens, TOKEN_COMMA, text, line, column, error);
    }

    if (sql_text[*index] == ';') {
        *index += 1;
        return append_token(tokens, TOKEN_SEMICOLON, text, line, column, error);
    }

    if (sql_text[*index] == '(') {
        *index += 1;
        return append_token(tokens, TOKEN_LPAREN, text, line, column, error);
    }

    if (sql_text[*index] == ')') {
        *index += 1;
        return append_token(tokens, TOKEN_RPAREN, text, line, column, error);
    }

    if (sql_text[*index] == '*') {
        *index += 1;
        return append_token(tokens, TOKEN_STAR, text, line, column, error);
    }

    set_error(error, "지원하지 않는 문자를 찾았습니다.", line, column);
    return 0;
}

int tokenize_sql(const char *sql_text, TokenList *tokens, ErrorInfo *error)
{
    int index;
    int line;
    int column;

    /*
     * SQL 문자열 전체를 왼쪽부터 훑으면서 토큰 배열을 만듭니다.
     * 공백은 건너뛰고, line/column은 파서 오류 메시지를 위해 계속 추적합니다.
     */
    memset(tokens, 0, sizeof(*tokens));
    memset(error, 0, sizeof(*error));

    index = 0;
    line = 1;
    column = 1;

    while (sql_text[index] != '\0') {
        if (sql_text[index] == ' ' || sql_text[index] == '\t' || sql_text[index] == '\r') {
            index += 1;
            column += 1;
            continue;
        }

        if (sql_text[index] == '\n') {
            index += 1;
            line += 1;
            column = 1;
            continue;
        }

        if (isalpha((unsigned char)sql_text[index]) || sql_text[index] == '_') {
            if (!read_word(sql_text, &index, line, column, tokens, error)) {
                return 0;
            }
            column = tokens->items[tokens->count - 1].column +
                     (int)strlen(tokens->items[tokens->count - 1].text);
            continue;
        }

        if (isdigit((unsigned char)sql_text[index]) ||
            (sql_text[index] == '-' && sql_text[index + 1] != '\0' &&
             isdigit((unsigned char)sql_text[index + 1]))) {
            if (!read_number(sql_text, &index, line, column, tokens, error)) {
                return 0;
            }
            column = tokens->items[tokens->count - 1].column +
                     (int)strlen(tokens->items[tokens->count - 1].text);
            continue;
        }

        if (sql_text[index] == '\'') {
            /* 문자열은 작은따옴표를 포함해 소비한 길이만큼 column을 갱신합니다. */
            if (!read_string(sql_text, &index, line, column, tokens, error)) {
                return 0;
            }

            /* 문자열은 내용 길이에 여닫는 작은따옴표 2개까지 함께 소비합니다. */
            column += (int)strlen(tokens->items[tokens->count - 1].text) + 2;
            continue;
        }

        if (!read_symbol(sql_text, &index, line, column, tokens, error)) {
            return 0;
        }

        column += (int)strlen(tokens->items[tokens->count - 1].text);
    }

    return append_token(tokens, TOKEN_EOF, "", line, column, error);
}
