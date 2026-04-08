#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/* 오류 메시지와 위치 정보를 ErrorInfo에 저장한다.
 *
 * @param error    오류 정보를 저장할 포인터
 * @param message  오류 메시지 문자열
 * @param line     오류 발생 줄 번호
 * @param column   오류 발생 열 번호
 */
static void set_error(ErrorInfo *error, const char *message, int line, int column)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = line;
    error->column = column;
}

/* src 문자열을 소문자로 변환하여 dest에 복사한다.
 *
 * @param dest       결과를 저장할 버퍼
 * @param dest_size  dest 버퍼 크기
 * @param src        원본 문자열
 */
static void to_lowercase_copy(char *dest, size_t dest_size, const char *src)
{
    size_t i;
    for (i = 0; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = (char)tolower((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

/* 소문자로 정규화된 단어가 SQL 예약어인지 판별하여 토큰 종류를 반환한다.
 *
 * @param text  소문자로 정규화된 단어 문자열
 * @return      해당 예약어의 TokenType, 예약어가 아니면 TOKEN_IDENTIFIER
 */
static TokenType keyword_type(const char *text)
{
    if (strcmp(text, "insert") == 0) return TOKEN_KEYWORD_INSERT;
    if (strcmp(text, "into") == 0)   return TOKEN_KEYWORD_INTO;
    if (strcmp(text, "values") == 0) return TOKEN_KEYWORD_VALUES;
    if (strcmp(text, "select") == 0) return TOKEN_KEYWORD_SELECT;
    if (strcmp(text, "from") == 0)   return TOKEN_KEYWORD_FROM;
    if (strcmp(text, "where") == 0)  return TOKEN_KEYWORD_WHERE;
    if (strcmp(text, "and") == 0)    return TOKEN_KEYWORD_AND;
    if (strcmp(text, "create") == 0) return TOKEN_KEYWORD_CREATE;
    if (strcmp(text, "index") == 0)  return TOKEN_KEYWORD_INDEX;
    if (strcmp(text, "on") == 0)     return TOKEN_KEYWORD_ON;
    return TOKEN_IDENTIFIER;
}

/* TokenList 끝에 토큰 1개를 추가한다.
 *
 * @param tokens  토큰을 추가할 목록 포인터
 * @param type    추가할 토큰 종류
 * @param text    토큰 텍스트 문자열
 * @param line    토큰 시작 줄 번호
 * @param column  토큰 시작 열 번호
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 토큰 수 초과 시 0
 */
static int append_token(TokenList *tokens,
                        TokenType type,
                        const char *text,
                        int line,
                        int column,
                        ErrorInfo *error)
{
    if (tokens->count >= SQLPROC_MAX_TOKENS) {
        set_error(error, "토큰 수가 최대 개수를 넘었습니다.", line, column);
        return 0;
    }

    Token *token = &tokens->items[tokens->count];
    token->type = type;
    snprintf(token->text, sizeof(token->text), "%s", text);
    token->line = line;
    token->column = column;
    tokens->count += 1;
    return 1;
}

/* SQL 문자열에서 알파벳/숫자/밑줄로 이루어진 단어 토큰을 읽는다.
 *
 * @param sql_text  전체 SQL 문자열
 * @param index     현재 읽기 위치 (읽은 만큼 전진)
 * @param line      현재 줄 번호
 * @param column    현재 열 번호
 * @param tokens    결과를 추가할 토큰 목록
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
static int read_word(const char *sql_text,
                     int *index,
                     int line,
                     int column,
                     TokenList *tokens,
                     ErrorInfo *error)
{
    int start = *index;
    while (isalnum((unsigned char)sql_text[*index]) || sql_text[*index] == '_') {
        *index += 1;
    }

    int length = *index - start;
    if (length >= (int)SQLPROC_MAX_VALUE_LEN) {
        set_error(error, "식별자 길이가 너무 깁니다.", line, column);
        return 0;
    }

    char raw_text[SQLPROC_MAX_VALUE_LEN];
    memcpy(raw_text, sql_text + start, (size_t)length);
    raw_text[length] = '\0';

    char lower_text[SQLPROC_MAX_VALUE_LEN];
    to_lowercase_copy(lower_text, sizeof(lower_text), raw_text);

    TokenType type = keyword_type(lower_text);
    if (type == TOKEN_IDENTIFIER) {
        return append_token(tokens, type, lower_text, line, column, error);
    }
    return append_token(tokens, type, raw_text, line, column, error);
}

/* SQL 문자열에서 정수 리터럴 토큰을 읽는다. 선행 '-'도 허용한다.
 *
 * @param sql_text  전체 SQL 문자열
 * @param index     현재 읽기 위치 (읽은 만큼 전진)
 * @param line      현재 줄 번호
 * @param column    현재 열 번호
 * @param tokens    결과를 추가할 토큰 목록
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
static int read_number(const char *sql_text,
                       int *index,
                       int line,
                       int column,
                       TokenList *tokens,
                       ErrorInfo *error)
{
    int start = *index;
    if (sql_text[*index] == '-') {
        *index += 1;
    }
    while (isdigit((unsigned char)sql_text[*index])) {
        *index += 1;
    }

    int length = *index - start;
    if (length <= 0 || length >= (int)SQLPROC_MAX_VALUE_LEN) {
        set_error(error, "숫자 리터럴이 잘못되었습니다.", line, column);
        return 0;
    }

    char number_text[SQLPROC_MAX_VALUE_LEN];
    memcpy(number_text, sql_text + start, (size_t)length);
    number_text[length] = '\0';
    return append_token(tokens, TOKEN_NUMBER, number_text, line, column, error);
}

/* SQL 문자열에서 작은따옴표로 감싼 문자열 리터럴 토큰을 읽는다.
 *
 * @param sql_text  전체 SQL 문자열
 * @param index     현재 읽기 위치 (읽은 만큼 전진)
 * @param line      현재 줄 번호
 * @param column    현재 열 번호
 * @param tokens    결과를 추가할 토큰 목록
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
static int read_string(const char *sql_text,
                       int *index,
                       int line,
                       int column,
                       TokenList *tokens,
                       ErrorInfo *error)
{
    *index += 1;
    int text_index = 0;
    char string_text[SQLPROC_MAX_VALUE_LEN];

    while (sql_text[*index] != '\0' && sql_text[*index] != '\'') {
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

/* SQL 문자열에서 기호 토큰 1개를 읽는다. 두 글자 연산자(<=, >=)도 처리한다.
 *
 * @param sql_text  전체 SQL 문자열
 * @param index     현재 읽기 위치 (읽은 만큼 전진)
 * @param line      현재 줄 번호
 * @param column    현재 열 번호
 * @param tokens    결과를 추가할 토큰 목록
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 지원하지 않는 문자이면 0
 */
static int read_symbol(const char *sql_text,
                       int *index,
                       int line,
                       int column,
                       TokenList *tokens,
                       ErrorInfo *error)
{
    char text[3];
    text[0] = sql_text[*index];
    text[1] = '\0';
    text[2] = '\0';

    if (sql_text[*index] == ',') { *index += 1; return append_token(tokens, TOKEN_COMMA,     text, line, column, error); }
    if (sql_text[*index] == ';') { *index += 1; return append_token(tokens, TOKEN_SEMICOLON, text, line, column, error); }
    if (sql_text[*index] == '(') { *index += 1; return append_token(tokens, TOKEN_LPAREN,    text, line, column, error); }
    if (sql_text[*index] == ')') { *index += 1; return append_token(tokens, TOKEN_RPAREN,    text, line, column, error); }
    if (sql_text[*index] == '*') { *index += 1; return append_token(tokens, TOKEN_STAR,      text, line, column, error); }
    if (sql_text[*index] == '=') { *index += 1; return append_token(tokens, TOKEN_EQUAL,     text, line, column, error); }

    if (sql_text[*index] == '<' && sql_text[*index + 1] == '=') {
        text[1] = '=';
        *index += 2;
        return append_token(tokens, TOKEN_LESS_EQUAL, text, line, column, error);
    }
    if (sql_text[*index] == '>' && sql_text[*index + 1] == '=') {
        text[1] = '=';
        *index += 2;
        return append_token(tokens, TOKEN_GREATER_EQUAL, text, line, column, error);
    }
    if (sql_text[*index] == '<') { *index += 1; return append_token(tokens, TOKEN_LESS,    text, line, column, error); }
    if (sql_text[*index] == '>') { *index += 1; return append_token(tokens, TOKEN_GREATER, text, line, column, error); }

    set_error(error, "지원하지 않는 문자를 찾았습니다.", line, column);
    return 0;
}

/* SQL 문자열 전체를 순회하며 TokenList를 생성한다.
 *
 * @param sql_text  입력 SQL 문자열
 * @param tokens    결과 토큰 목록 포인터
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
int tokenize_sql(const char *sql_text, TokenList *tokens, ErrorInfo *error)
{
    memset(tokens, 0, sizeof(*tokens));
    memset(error, 0, sizeof(*error));

    int index = 0;
    int line = 1;
    int column = 1;

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
            (sql_text[index] == '-' && isdigit((unsigned char)sql_text[index + 1]))) {
            if (!read_number(sql_text, &index, line, column, tokens, error)) {
                return 0;
            }
            column = tokens->items[tokens->count - 1].column +
                     (int)strlen(tokens->items[tokens->count - 1].text);
            continue;
        }

        if (sql_text[index] == '\'') {
            if (!read_string(sql_text, &index, line, column, tokens, error)) {
                return 0;
            }
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

/* TokenType 열거값에 대응하는 이름 문자열을 반환한다.
 *
 * @param type  토큰 종류 열거값
 * @return      해당 열거값의 이름 문자열 (알 수 없으면 "UNKNOWN")
 */
const char *token_type_name(TokenType type)
{
    if (type == TOKEN_EOF)              return "EOF";
    if (type == TOKEN_IDENTIFIER)       return "IDENTIFIER";
    if (type == TOKEN_NUMBER)           return "NUMBER";
    if (type == TOKEN_STRING)           return "STRING";
    if (type == TOKEN_COMMA)            return "COMMA";
    if (type == TOKEN_SEMICOLON)        return "SEMICOLON";
    if (type == TOKEN_LPAREN)           return "LPAREN";
    if (type == TOKEN_RPAREN)           return "RPAREN";
    if (type == TOKEN_STAR)             return "STAR";
    if (type == TOKEN_EQUAL)            return "EQUAL";
    if (type == TOKEN_LESS)             return "LESS";
    if (type == TOKEN_LESS_EQUAL)       return "LESS_EQUAL";
    if (type == TOKEN_GREATER)          return "GREATER";
    if (type == TOKEN_GREATER_EQUAL)    return "GREATER_EQUAL";
    if (type == TOKEN_KEYWORD_INSERT)   return "INSERT";
    if (type == TOKEN_KEYWORD_INTO)     return "INTO";
    if (type == TOKEN_KEYWORD_VALUES)   return "VALUES";
    if (type == TOKEN_KEYWORD_SELECT)   return "SELECT";
    if (type == TOKEN_KEYWORD_FROM)     return "FROM";
    if (type == TOKEN_KEYWORD_WHERE)    return "WHERE";
    if (type == TOKEN_KEYWORD_AND)      return "AND";
    if (type == TOKEN_KEYWORD_CREATE)   return "CREATE";
    if (type == TOKEN_KEYWORD_INDEX)    return "INDEX";
    if (type == TOKEN_KEYWORD_ON)       return "ON";
    return "UNKNOWN";
}
