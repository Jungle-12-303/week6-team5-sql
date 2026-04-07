#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

static void set_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

static void to_lowercase_copy(char *dest, size_t dest_size, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = (char)tolower((unsigned char)src[i]);
    }

    dest[i] = '\0';
}

static int parse_data_type(const char *text, DataType *data_type)
{
    if (strcmp(text, "int") == 0) {
        *data_type = DATA_TYPE_INT;
        return 1;
    }

    if (strcmp(text, "string") == 0) {
        *data_type = DATA_TYPE_STRING;
        return 1;
    }

    return 0;
}

int load_table_schema(const char *schema_dir,
                      const char *table_name,
                      TableSchema *schema,
                      ErrorInfo *error)
{
    char path[512];
    char line[1024];
    char *entry;
    char *cursor;
    FILE *file;

    memset(schema, 0, sizeof(*schema));
    memset(error, 0, sizeof(*error));
    snprintf(schema->table_name, sizeof(schema->table_name), "%s", table_name);
    snprintf(path, sizeof(path), "%s/%s.schema", schema_dir, table_name);

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, "스키마 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_error(error, "스키마 파일이 비어 있습니다.");
        return 0;
    }

    fclose(file);

    line[strcspn(line, "\r\n")] = '\0';
    cursor = line;

    while (*cursor != '\0') {
        char *colon;
        char lower_name[SQLPROC_MAX_NAME_LEN];
        char lower_type[SQLPROC_MAX_NAME_LEN];

        if (schema->column_count >= SQLPROC_MAX_COLUMNS) {
            set_error(error, "스키마 컬럼 수가 최대 개수를 넘었습니다.");
            return 0;
        }

        entry = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor += 1;
        }

        if (*cursor == ',') {
            *cursor = '\0';
            cursor += 1;
        }

        colon = strchr(entry, ':');
        if (colon == NULL) {
            set_error(error, "스키마 형식이 잘못되었습니다.");
            return 0;
        }

        *colon = '\0';
        to_lowercase_copy(lower_name, sizeof(lower_name), entry);
        to_lowercase_copy(lower_type, sizeof(lower_type), colon + 1);

        if (!parse_data_type(lower_type, &schema->columns[schema->column_count].type)) {
            set_error(error, "지원하지 않는 스키마 타입입니다.");
            return 0;
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
