#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/* 오류 메시지를 ErrorInfo에 저장한다. 줄/열 정보는 기록하지 않는다.
 *
 * @param error    오류 정보를 저장할 포인터
 * @param message  오류 메시지 문자열
 */
static void set_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
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

/* 스키마 파일의 타입 문자열("int", "string")을 DataType 열거값으로 변환한다.
 *
 * @param text       소문자로 정규화된 타입 문자열
 * @param data_type  결과를 저장할 DataType 포인터
 * @return           성공 시 1, 알 수 없는 타입이면 0
 */
static int parse_data_type(const char *text, DataType *data_type)
{
    if (strcmp(text, "int") == 0)    { *data_type = DATA_TYPE_INT;    return 1; }
    if (strcmp(text, "string") == 0) { *data_type = DATA_TYPE_STRING; return 1; }
    return 0;
}

/* 스키마 디렉토리에서 테이블 스키마 파일을 읽어 TableSchema를 채운다.
 * 스키마 파일 형식 예: id:int:pk,name:string,age:int
 *
 * @param schema_dir  스키마 파일 디렉토리 경로
 * @param table_name  테이블 이름
 * @param schema      결과를 저장할 TableSchema 포인터
 * @param error       오류 정보 저장 포인터
 * @return            성공 시 1, 실패 시 0
 */
int load_table_schema(const char *schema_dir,
                      const char *table_name,
                      TableSchema *schema,
                      ErrorInfo *error)
{
    memset(schema, 0, sizeof(*schema));
    memset(error, 0, sizeof(*error));
    snprintf(schema->table_name, sizeof(schema->table_name), "%s", table_name);
    schema->primary_key_column_index = -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.schema", schema_dir, table_name);

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, "스키마 파일을 열 수 없습니다.");
        return 0;
    }

    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_error(error, "스키마 파일이 비어 있습니다.");
        return 0;
    }
    fclose(file);

    line[strcspn(line, "\r\n")] = '\0';
    char *cursor = line;

    while (*cursor != '\0') {
        if (schema->column_count >= SQLPROC_MAX_COLUMNS) {
            set_error(error, "스키마 컬럼 수가 최대 개수를 넘었습니다.");
            return 0;
        }

        char *entry = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor += 1;
        }
        if (*cursor == ',') {
            *cursor = '\0';
            cursor += 1;
        }

        char *colon = strchr(entry, ':');
        if (colon == NULL) {
            set_error(error, "스키마 형식이 잘못되었습니다.");
            return 0;
        }
        *colon = '\0';

        char lower_modifier[SQLPROC_MAX_NAME_LEN];
        char *modifier = strchr(colon + 1, ':');
        if (modifier != NULL) {
            *modifier = '\0';
            modifier += 1;
            to_lowercase_copy(lower_modifier, sizeof(lower_modifier), modifier);
        } else {
            lower_modifier[0] = '\0';
        }

        char lower_name[SQLPROC_MAX_NAME_LEN];
        char lower_type[SQLPROC_MAX_NAME_LEN];
        to_lowercase_copy(lower_name, sizeof(lower_name), entry);
        to_lowercase_copy(lower_type, sizeof(lower_type), colon + 1);

        if (!parse_data_type(lower_type, &schema->columns[schema->column_count].type)) {
            set_error(error, "지원하지 않는 스키마 타입입니다.");
            return 0;
        }

        if (lower_modifier[0] != '\0') {
            if (strcmp(lower_modifier, "pk") != 0) {
                set_error(error, "지원하지 않는 스키마 제약 조건입니다.");
                return 0;
            }
            if (schema->primary_key_column_index >= 0) {
                set_error(error, "PRIMARY KEY는 하나만 지정할 수 있습니다.");
                return 0;
            }
            schema->columns[schema->column_count].is_primary_key = 1;
            schema->primary_key_column_index = schema->column_count;
        }

        snprintf(schema->columns[schema->column_count].name,
                 sizeof(schema->columns[schema->column_count].name),
                 "%s",
                 lower_name);
        schema->column_count += 1;
    }

    if (schema->column_count == 0) {
        set_error(error, "스키마에 컬럼이 없습니다.");
        return 0;
    }

    return 1;
}
