#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

#define INDEX_MAGIC "SQLIDX1"
#define INDEX_VERSION 1
#define INDEX_MAX_PATH_LEN 512
#define INDEX_MAX_PATH_DEPTH 32
#define INDEX_MAX_TABLE_INDEXES 32
#define INDEX_ROW_BUFFER_SIZE 1024

typedef struct {
    char magic[8];
    int version;
    char index_name[SQLPROC_MAX_NAME_LEN];
    char table_name[SQLPROC_MAX_NAME_LEN];
    char column_name[SQLPROC_MAX_NAME_LEN];
    int column_index;
    int column_type;
    int root_node_id;
    int next_node_id;
} IndexHeader;

typedef struct {
    int is_leaf;
    int key_count;
    int next_leaf_id;
    char keys[SQLPROC_BTREE_MAX_KEYS][SQLPROC_MAX_VALUE_LEN];
    long row_offsets[SQLPROC_BTREE_MAX_KEYS];
    int child_ids[SQLPROC_BTREE_MAX_KEYS + 1];
} BTreeNode;

static int collect_table_index_paths(const AppConfig *config,
                                     const TableSchema *schema,
                                     char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN],
                                     int *path_count,
                                     ErrorInfo *error);

/* 파일/인덱스 오류 메시지를 ErrorInfo에 저장한다. 위치 정보는 기록하지 않는다.
 *
 * @param error    오류 정보를 저장할 포인터
 * @param message  오류 메시지 문자열
 */
static void set_file_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

/* SQL 소스 위치 정보를 포함한 오류 메시지를 ErrorInfo에 저장한다.
 *
 * @param error     오류 정보를 저장할 포인터
 * @param message   오류 메시지 문자열
 * @param location  오류 발생 소스 위치 (줄, 열)
 */
static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

/* "index_dir/index_name.idx" 형태의 인덱스 파일 경로를 조립하여 dest에 저장한다.
 *
 * @param dest       결과 경로를 저장할 버퍼
 * @param dest_size  dest 버퍼 크기
 * @param index_dir  인덱스 파일 디렉토리 경로
 * @param index_name 인덱스 이름
 */
static void build_index_path(char *dest,
                             size_t dest_size,
                             const char *index_dir,
                             const char *index_name)
{
    snprintf(dest, dest_size, "%s/%s.idx", index_dir, index_name);
}

/* node_id를 인덱스 파일 내 바이트 오프셋으로 변환한다.
 *
 * @param node_id  B+ 트리 노드 ID
 * @return         해당 노드의 파일 내 바이트 오프셋
 */
static long node_offset(int node_id)
{
    return (long)sizeof(IndexHeader) + ((long)node_id * (long)sizeof(BTreeNode));
}

/* 문자열을 long 정수로 변환한다. 완전히 파싱되지 않으면 실패를 반환한다.
 *
 * @param text   변환할 문자열
 * @param value  결과를 저장할 long 포인터
 * @return       성공 시 1, 실패 시 0
 */
static int parse_int_text(const char *text, long *value)
{
    if (text[0] == '\0') return 0;
    char *end_pointer;
    *value = strtol(text, &end_pointer, 10);
    return *end_pointer == '\0';
}

/* 인덱스 키 두 개를 데이터 타입에 따라 비교한다.
 * int는 숫자 비교, string은 사전순 비교를 수행한다.
 *
 * @param data_type  컬럼 데이터 타입
 * @param left       왼쪽 키 문자열
 * @param right      오른쪽 키 문자열
 * @return           left < right이면 음수, 같으면 0, left > right이면 양수
 */
static int compare_keys(int data_type, const char *left, const char *right)
{
    if (data_type == DATA_TYPE_INT) {
        long left_value;
        long right_value;
        parse_int_text(left, &left_value);
        parse_int_text(right, &right_value);
        if (left_value < right_value) return -1;
        if (left_value > right_value) return  1;
        return 0;
    }
    return strcmp(left, right);
}

/* CSV 한 줄을 컬럼 값 배열로 파싱한다. 큰따옴표 이스케이프도 처리한다.
 *
 * @param line         파싱할 CSV 줄 문자열
 * @param values       결과 컬럼 값 배열
 * @param value_count  파싱된 컬럼 수를 저장할 포인터
 * @return             성공 시 1, 형식 오류 시 0
 */
static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count)
{
    int in_quotes = 0;
    int row_index = 0;
    int text_index = 0;

    for (int i = 0; line[i] != '\0' && line[i] != '\n' && line[i] != '\r'; i++) {
        if (row_index >= SQLPROC_MAX_COLUMNS) return 0;

        if (line[i] == '"') {
            if (in_quotes && line[i + 1] == '"') {
                if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) return 0;
                values[row_index][text_index] = '"';
                text_index += 1;
                i += 1;
                continue;
            }
            in_quotes = !in_quotes;
            continue;
        }

        if (!in_quotes && line[i] == ',') {
            values[row_index][text_index] = '\0';
            row_index += 1;
            text_index = 0;
            continue;
        }

        if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) return 0;
        values[row_index][text_index] = line[i];
        text_index += 1;
    }

    values[row_index][text_index] = '\0';
    *value_count = row_index + 1;
    return 1;
}

/* CSV 파일의 헤더 줄을 읽어 현재 스키마와 일치하는지 검증한다.
 *
 * @param file    열린 CSV 파일 포인터 (현재 위치에서 헤더를 읽음)
 * @param schema  검증 기준 테이블 스키마 포인터
 * @param error   오류 정보 저장 포인터
 * @return        일치하면 1, 아니면 0
 */
static int validate_header(FILE *file, const TableSchema *schema, ErrorInfo *error)
{
    char line[INDEX_ROW_BUFFER_SIZE];
    if (fgets(line, sizeof(line), file) == NULL) {
        set_file_error(error, "CSV 헤더를 읽을 수 없습니다.");
        return 0;
    }

    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int value_count;
    if (!parse_csv_line(line, values, &value_count)) {
        set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
        return 0;
    }

    if (value_count != schema->column_count) {
        set_file_error(error, "CSV 헤더가 스키마와 다릅니다.");
        return 0;
    }

    for (int i = 0; i < schema->column_count; i++) {
        if (strcmp(values[i], schema->columns[i].name) != 0) {
            set_file_error(error, "CSV 헤더 순서가 스키마와 다릅니다.");
            return 0;
        }
    }

    return 1;
}

/* 인덱스 파일의 헤더를 읽고 magic과 version을 검증한다.
 *
 * @param file    열린 인덱스 파일 포인터
 * @param header  결과를 저장할 IndexHeader 포인터
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
static int read_header(FILE *file, IndexHeader *header, ErrorInfo *error)
{
    rewind(file);
    if (fread(header, sizeof(*header), 1, file) != 1) {
        set_file_error(error, "인덱스 헤더를 읽을 수 없습니다.");
        return 0;
    }
    if (strcmp(header->magic, INDEX_MAGIC) != 0 || header->version != INDEX_VERSION) {
        set_file_error(error, "인덱스 파일 형식이 잘못되었습니다.");
        return 0;
    }
    return 1;
}

/* 메모리의 IndexHeader를 인덱스 파일 맨 앞에 저장한다.
 *
 * @param file    열린 인덱스 파일 포인터
 * @param header  저장할 IndexHeader 포인터
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
static int write_header(FILE *file, const IndexHeader *header, ErrorInfo *error)
{
    rewind(file);
    if (fwrite(header, sizeof(*header), 1, file) != 1) {
        set_file_error(error, "인덱스 헤더를 저장할 수 없습니다.");
        return 0;
    }
    fflush(file);
    return 1;
}

/* 지정한 node_id 위치의 B+ 트리 노드를 파일에서 읽는다.
 *
 * @param file     열린 인덱스 파일 포인터
 * @param node_id  읽을 노드 ID
 * @param node     결과를 저장할 BTreeNode 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
static int read_node(FILE *file, int node_id, BTreeNode *node, ErrorInfo *error)
{
    if (fseek(file, node_offset(node_id), SEEK_SET) != 0) {
        set_file_error(error, "인덱스 노드 위치로 이동할 수 없습니다.");
        return 0;
    }
    if (fread(node, sizeof(*node), 1, file) != 1) {
        set_file_error(error, "인덱스 노드를 읽을 수 없습니다.");
        return 0;
    }
    return 1;
}

/* 지정한 node_id 위치에 B+ 트리 노드를 파일에 저장한다.
 *
 * @param file     열린 인덱스 파일 포인터
 * @param node_id  저장할 노드 ID
 * @param node     저장할 BTreeNode 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
static int write_node(FILE *file, int node_id, const BTreeNode *node, ErrorInfo *error)
{
    if (fseek(file, node_offset(node_id), SEEK_SET) != 0) {
        set_file_error(error, "인덱스 노드 위치로 이동할 수 없습니다.");
        return 0;
    }
    if (fwrite(node, sizeof(*node), 1, file) != 1) {
        set_file_error(error, "인덱스 노드를 저장할 수 없습니다.");
        return 0;
    }
    fflush(file);
    return 1;
}

/* next_node_id를 사용해 새 빈 노드를 할당하고 파일에 기록한다.
 *
 * @param file     열린 인덱스 파일 포인터
 * @param header   현재 인덱스 헤더 포인터 (next_node_id 증가)
 * @param node_id  할당된 노드 ID를 저장할 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
static int allocate_node(FILE *file, IndexHeader *header, int *node_id, ErrorInfo *error)
{
    BTreeNode node;
    memset(&node, 0, sizeof(node));
    node.next_leaf_id = -1;

    *node_id = header->next_node_id;
    header->next_node_id += 1;

    if (!write_header(file, header, error)) return 0;
    return write_node(file, *node_id, &node, error);
}

/* 루트에서 시작해 주어진 키가 들어갈 리프 노드를 찾는다.
 * 탐색 경로(내부 노드 ID 목록)를 path에 기록한다.
 *
 * @param file         열린 인덱스 파일 포인터
 * @param header       인덱스 헤더 포인터
 * @param key          찾을 키 문자열
 * @param path         탐색 경로 내부 노드 ID 배열
 * @param path_length  경로 길이를 저장할 포인터
 * @param leaf_id      결과 리프 노드 ID를 저장할 포인터
 * @param error        오류 정보 저장 포인터
 * @return             성공 시 1, 실패 시 0
 */
static int find_leaf_node(FILE *file,
                          const IndexHeader *header,
                          const char *key,
                          int path[INDEX_MAX_PATH_DEPTH],
                          int *path_length,
                          int *leaf_id,
                          ErrorInfo *error)
{
    *path_length = 0;
    int node_id = header->root_node_id;

    while (1) {
        BTreeNode node;
        if (!read_node(file, node_id, &node, error)) return 0;

        if (node.is_leaf) {
            *leaf_id = node_id;
            return 1;
        }

        if (*path_length >= INDEX_MAX_PATH_DEPTH) {
            set_file_error(error, "B+ 트리 경로가 최대 깊이를 넘었습니다.");
            return 0;
        }

        path[*path_length] = node_id;
        *path_length += 1;

        int child_index = 0;
        while (child_index < node.key_count &&
               compare_keys(header->column_type, key, node.keys[child_index]) >= 0) {
            child_index += 1;
        }
        node_id = node.child_ids[child_index];
    }
}

/* 리프 노드에 키/오프셋 엔트리를 정렬된 위치에 삽입한다.
 * 호출 전에 리프에 빈 자리가 있어야 한다.
 *
 * @param leaf        삽입 대상 리프 노드 포인터
 * @param header      인덱스 헤더 포인터 (키 비교 타입 참조)
 * @param key         삽입할 키 문자열
 * @param row_offset  삽입할 CSV 행 오프셋
 */
static void insert_into_leaf(BTreeNode *leaf,
                             const IndexHeader *header,
                             const char *key,
                             long row_offset)
{
    int insert_index = 0;
    while (insert_index < leaf->key_count) {
        int compare_result = compare_keys(header->column_type, leaf->keys[insert_index], key);
        if (compare_result > 0) break;
        if (compare_result == 0 && leaf->row_offsets[insert_index] > row_offset) break;
        insert_index += 1;
    }

    for (int i = leaf->key_count; i > insert_index; i--) {
        snprintf(leaf->keys[i], sizeof(leaf->keys[i]), "%s", leaf->keys[i - 1]);
        leaf->row_offsets[i] = leaf->row_offsets[i - 1];
    }

    snprintf(leaf->keys[insert_index], sizeof(leaf->keys[insert_index]), "%s", key);
    leaf->row_offsets[insert_index] = row_offset;
    leaf->key_count += 1;
}

/* 꽉 찬 리프 노드를 두 개의 리프로 분할하고 부모에 올릴 분리 키를 반환한다.
 * 기존 키와 새 키를 임시 배열에 정렬한 뒤 절반씩 나눈다.
 * 오른쪽 리프의 첫 키가 promoted_key로 부모에 올라간다.
 *
 * @param header         인덱스 헤더 포인터
 * @param left_leaf      분할할 왼쪽 리프 노드 포인터 (수정됨)
 * @param key            삽입할 키 문자열
 * @param row_offset     삽입할 행 오프셋
 * @param right_leaf_id  새 오른쪽 리프의 노드 ID
 * @param right_leaf     새 오른쪽 리프 노드 포인터 (출력)
 * @param promoted_key   부모에 올릴 분리 키 버퍼
 */
static void split_leaf_node(const IndexHeader *header,
                            BTreeNode *left_leaf,
                            const char *key,
                            long row_offset,
                            int right_leaf_id,
                            BTreeNode *right_leaf,
                            char promoted_key[SQLPROC_MAX_VALUE_LEN])
{
    char temp_keys[SQLPROC_BTREE_MAX_KEYS + 1][SQLPROC_MAX_VALUE_LEN];
    long temp_offsets[SQLPROC_BTREE_MAX_KEYS + 1];

    memset(right_leaf, 0, sizeof(*right_leaf));
    right_leaf->is_leaf = 1;
    right_leaf->next_leaf_id = left_leaf->next_leaf_id;

    int insert_index = 0;
    while (insert_index < left_leaf->key_count) {
        int compare_result = compare_keys(header->column_type, left_leaf->keys[insert_index], key);
        if (compare_result > 0) break;
        if (compare_result == 0 && left_leaf->row_offsets[insert_index] > row_offset) break;
        insert_index += 1;
    }

    for (int i = 0; i < insert_index; i++) {
        snprintf(temp_keys[i], sizeof(temp_keys[i]), "%s", left_leaf->keys[i]);
        temp_offsets[i] = left_leaf->row_offsets[i];
    }
    snprintf(temp_keys[insert_index], sizeof(temp_keys[insert_index]), "%s", key);
    temp_offsets[insert_index] = row_offset;
    for (int i = insert_index; i < left_leaf->key_count; i++) {
        snprintf(temp_keys[i + 1], sizeof(temp_keys[i + 1]), "%s", left_leaf->keys[i]);
        temp_offsets[i + 1] = left_leaf->row_offsets[i];
    }

    int total_keys = SQLPROC_BTREE_MAX_KEYS + 1;
    int left_count = (total_keys + 1) / 2;

    memset(left_leaf->keys, 0, sizeof(left_leaf->keys));
    memset(left_leaf->row_offsets, 0, sizeof(left_leaf->row_offsets));
    left_leaf->key_count = left_count;
    for (int i = 0; i < left_count; i++) {
        snprintf(left_leaf->keys[i], sizeof(left_leaf->keys[i]), "%s", temp_keys[i]);
        left_leaf->row_offsets[i] = temp_offsets[i];
    }

    right_leaf->key_count = total_keys - left_count;
    for (int i = 0; i < right_leaf->key_count; i++) {
        snprintf(right_leaf->keys[i], sizeof(right_leaf->keys[i]), "%s", temp_keys[left_count + i]);
        right_leaf->row_offsets[i] = temp_offsets[left_count + i];
    }

    left_leaf->next_leaf_id = right_leaf_id;
    snprintf(promoted_key, SQLPROC_MAX_VALUE_LEN, "%s", right_leaf->keys[0]);
}

/* 꽉 찬 내부 노드를 분할하고 가운데 키를 부모로 올릴 준비를 한다.
 * left_child_id의 위치를 찾아 새 키와 right_child_id를 끼워 넣은 뒤
 * 가운데 키를 기준으로 좌우 내부 노드로 나눈다.
 *
 * @param header          인덱스 헤더 포인터 (unused, 향후 확장용)
 * @param left_node       분할할 왼쪽 내부 노드 포인터 (수정됨)
 * @param key             삽입할 분리 키
 * @param right_child_id  새 오른쪽 자식 노드 ID
 * @param left_child_id   기존 왼쪽 자식 노드 ID (삽입 위치 기준)
 * @param right_node      새 오른쪽 내부 노드 포인터 (출력)
 * @param promoted_key    부모에 올릴 분리 키 버퍼
 */
static void split_internal_node(const IndexHeader *header,
                                BTreeNode *left_node,
                                const char *key,
                                int right_child_id,
                                int left_child_id,
                                BTreeNode *right_node,
                                char promoted_key[SQLPROC_MAX_VALUE_LEN])
{
    (void)header;

    char temp_keys[SQLPROC_BTREE_MAX_KEYS + 1][SQLPROC_MAX_VALUE_LEN];
    int temp_children[SQLPROC_BTREE_MAX_KEYS + 2];

    memset(right_node, 0, sizeof(*right_node));
    right_node->is_leaf = 0;

    int insert_index = 0;
    while (insert_index <= left_node->key_count &&
           left_node->child_ids[insert_index] != left_child_id) {
        insert_index += 1;
    }

    for (int i = 0; i < insert_index; i++) {
        temp_children[i] = left_node->child_ids[i];
    }
    temp_children[insert_index] = left_child_id;
    temp_children[insert_index + 1] = right_child_id;
    for (int i = insert_index + 1; i <= left_node->key_count; i++) {
        temp_children[i + 1] = left_node->child_ids[i];
    }

    for (int i = 0; i < insert_index; i++) {
        snprintf(temp_keys[i], sizeof(temp_keys[i]), "%s", left_node->keys[i]);
    }
    snprintf(temp_keys[insert_index], sizeof(temp_keys[insert_index]), "%s", key);
    for (int i = insert_index; i < left_node->key_count; i++) {
        snprintf(temp_keys[i + 1], sizeof(temp_keys[i + 1]), "%s", left_node->keys[i]);
    }

    int total_keys = SQLPROC_BTREE_MAX_KEYS + 1;
    int middle_index = total_keys / 2;
    snprintf(promoted_key, SQLPROC_MAX_VALUE_LEN, "%s", temp_keys[middle_index]);

    memset(left_node->keys, 0, sizeof(left_node->keys));
    memset(left_node->child_ids, 0, sizeof(left_node->child_ids));
    left_node->key_count = middle_index;
    for (int i = 0; i < middle_index; i++) {
        snprintf(left_node->keys[i], sizeof(left_node->keys[i]), "%s", temp_keys[i]);
        left_node->child_ids[i] = temp_children[i];
    }
    left_node->child_ids[middle_index] = temp_children[middle_index];

    right_node->key_count = total_keys - middle_index - 1;
    for (int i = 0; i < right_node->key_count; i++) {
        snprintf(right_node->keys[i], sizeof(right_node->keys[i]), "%s", temp_keys[middle_index + 1 + i]);
        right_node->child_ids[i] = temp_children[middle_index + 1 + i];
    }
    right_node->child_ids[right_node->key_count] =
        temp_children[middle_index + 1 + right_node->key_count];
}

/* 분할된 자식 노드의 분리 키를 부모 내부 노드에 반영한다.
 * 부모가 없으면 새 루트를 생성하고, 부모도 꽉 차면 분할을 상위로 전파한다.
 * 재귀 대신 반복문으로 경로를 따라 올라간다.
 *
 * @param file            열린 인덱스 파일 포인터
 * @param header          인덱스 헤더 포인터 (root_node_id 갱신 가능)
 * @param left_child_id   분할된 왼쪽 자식 노드 ID
 * @param key             부모에 삽입할 분리 키
 * @param right_child_id  분할된 오른쪽 자식 노드 ID
 * @param path            루트에서 리프까지의 내부 노드 ID 경로
 * @param path_length     경로 길이
 * @param error           오류 정보 저장 포인터
 * @return                성공 시 1, 실패 시 0
 */
static int insert_into_parent(FILE *file,
                              IndexHeader *header,
                              int left_child_id,
                              const char *key,
                              int right_child_id,
                              int path[INDEX_MAX_PATH_DEPTH],
                              int path_length,
                              ErrorInfo *error)
{
    char current_key[SQLPROC_MAX_VALUE_LEN];
    snprintf(current_key, sizeof(current_key), "%s", key);

    if (path_length == 0) {
        int new_root_id;
        if (!allocate_node(file, header, &new_root_id, error)) return 0;

        BTreeNode new_root;
        memset(&new_root, 0, sizeof(new_root));
        new_root.is_leaf = 0;
        new_root.key_count = 1;
        snprintf(new_root.keys[0], sizeof(new_root.keys[0]), "%s", current_key);
        new_root.child_ids[0] = left_child_id;
        new_root.child_ids[1] = right_child_id;
        header->root_node_id = new_root_id;

        if (!write_header(file, header, error)) return 0;
        return write_node(file, new_root_id, &new_root, error);
    }

    while (path_length > 0) {
        int parent_id = path[path_length - 1];
        BTreeNode parent;
        if (!read_node(file, parent_id, &parent, error)) return 0;

        if (parent.key_count < SQLPROC_BTREE_MAX_KEYS) {
            int insert_index = 0;
            while (insert_index <= parent.key_count &&
                   parent.child_ids[insert_index] != left_child_id) {
                insert_index += 1;
            }

            for (int i = parent.key_count; i > insert_index; i--) {
                snprintf(parent.keys[i], sizeof(parent.keys[i]), "%s", parent.keys[i - 1]);
            }
            for (int i = parent.key_count + 1; i > insert_index + 1; i--) {
                parent.child_ids[i] = parent.child_ids[i - 1];
            }

            snprintf(parent.keys[insert_index], sizeof(parent.keys[insert_index]), "%s", current_key);
            parent.child_ids[insert_index + 1] = right_child_id;
            parent.key_count += 1;
            return write_node(file, parent_id, &parent, error);
        }

        int new_right_id;
        if (!allocate_node(file, header, &new_right_id, error)) return 0;

        BTreeNode right_node;
        char promoted_key[SQLPROC_MAX_VALUE_LEN];
        split_internal_node(header, &parent, current_key, right_child_id,
                            left_child_id, &right_node, promoted_key);

        if (!write_node(file, parent_id, &parent, error)) return 0;
        if (!write_node(file, new_right_id, &right_node, error)) return 0;

        left_child_id = parent_id;
        right_child_id = new_right_id;
        snprintf(current_key, sizeof(current_key), "%s", promoted_key);
        path_length -= 1;
    }

    {
        int new_root_id;
        if (!allocate_node(file, header, &new_root_id, error)) return 0;

        BTreeNode new_root;
        memset(&new_root, 0, sizeof(new_root));
        new_root.is_leaf = 0;
        new_root.key_count = 1;
        snprintf(new_root.keys[0], sizeof(new_root.keys[0]), "%s", current_key);
        new_root.child_ids[0] = left_child_id;
        new_root.child_ids[1] = right_child_id;
        header->root_node_id = new_root_id;

        if (!write_header(file, header, error)) return 0;
        return write_node(file, new_root_id, &new_root, error);
    }
}

/* B+ 트리에 키/오프셋 엔트리를 삽입한다.
 * 리프가 꽉 차면 분할하고 insert_into_parent를 호출한다.
 *
 * @param file        열린 인덱스 파일 포인터
 * @param header      인덱스 헤더 포인터
 * @param key         삽입할 키 문자열
 * @param row_offset  삽입할 CSV 행 오프셋
 * @param error       오류 정보 저장 포인터
 * @return            성공 시 1, 실패 시 0
 */
static int insert_entry(FILE *file,
                        IndexHeader *header,
                        const char *key,
                        long row_offset,
                        ErrorInfo *error)
{
    int path[INDEX_MAX_PATH_DEPTH];
    int path_length;
    int leaf_id;

    if (!find_leaf_node(file, header, key, path, &path_length, &leaf_id, error)) return 0;

    BTreeNode leaf;
    if (!read_node(file, leaf_id, &leaf, error)) return 0;

    if (leaf.key_count < SQLPROC_BTREE_MAX_KEYS) {
        insert_into_leaf(&leaf, header, key, row_offset);
        return write_node(file, leaf_id, &leaf, error);
    }

    int right_leaf_id;
    if (!allocate_node(file, header, &right_leaf_id, error)) return 0;

    BTreeNode right_leaf;
    char promoted_key[SQLPROC_MAX_VALUE_LEN];
    split_leaf_node(header, &leaf, key, row_offset, right_leaf_id, &right_leaf, promoted_key);

    if (!write_node(file, leaf_id, &leaf, error)) return 0;
    if (!write_node(file, right_leaf_id, &right_leaf, error)) return 0;

    return insert_into_parent(file, header, leaf_id, promoted_key, right_leaf_id,
                              path, path_length, error);
}

/* 빈 루트 리프를 포함한 새 인덱스 파일을 초기화하여 디스크에 생성한다.
 *
 * @param path          생성할 인덱스 파일 경로
 * @param index_name    인덱스 이름
 * @param table_name    테이블 이름
 * @param column_name   인덱스 컬럼 이름
 * @param column_index  스키마 내 컬럼 인덱스
 * @param column_type   컬럼 데이터 타입
 * @param error         오류 정보 저장 포인터
 * @return              성공 시 1, 실패 시 0
 */
static int initialize_index_file(const char *path,
                                 const char *index_name,
                                 const char *table_name,
                                 const char *column_name,
                                 int column_index,
                                 int column_type,
                                 ErrorInfo *error)
{
    FILE *file = fopen(path, "wb+");
    if (file == NULL) {
        set_file_error(error, "인덱스 파일을 만들 수 없습니다.");
        return 0;
    }

    IndexHeader header;
    BTreeNode root;
    memset(&header, 0, sizeof(header));
    memset(&root, 0, sizeof(root));

    snprintf(header.magic, sizeof(header.magic), "%s", INDEX_MAGIC);
    header.version = INDEX_VERSION;
    snprintf(header.index_name, sizeof(header.index_name), "%s", index_name);
    snprintf(header.table_name, sizeof(header.table_name), "%s", table_name);
    snprintf(header.column_name, sizeof(header.column_name), "%s", column_name);
    header.column_index = column_index;
    header.column_type = column_type;
    header.root_node_id = 0;
    header.next_node_id = 1;

    root.is_leaf = 1;
    root.next_leaf_id = -1;

    if (!write_header(file, &header, error)) { fclose(file); return 0; }
    if (!write_node(file, 0, &root, error))  { fclose(file); return 0; }

    fclose(file);
    return 1;
}

/* CSV 데이터 파일의 모든 기존 행을 인덱스 파일에 삽입한다.
 * 데이터 파일이 없으면 성공으로 반환한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @param schema  테이블 스키마 포인터
 * @param header  인덱스 헤더 정보 포인터 (column_index, column_type 참조)
 * @param path    인덱스 파일 경로
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
static int append_existing_rows(const AppConfig *config,
                                const TableSchema *schema,
                                const IndexHeader *header,
                                const char *path,
                                ErrorInfo *error)
{
    char data_path[INDEX_MAX_PATH_LEN];
    snprintf(data_path, sizeof(data_path), "%s/%s.csv", config->data_dir, schema->table_name);

    FILE *data_file = fopen(data_path, "rb");
    if (data_file == NULL) {
        if (errno == ENOENT) return 1;
        set_file_error(error, "기존 데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!validate_header(data_file, schema, error)) {
        fclose(data_file);
        return 0;
    }

    FILE *index_file = fopen(path, "rb+");
    if (index_file == NULL) {
        fclose(data_file);
        set_file_error(error, "생성한 인덱스 파일을 다시 열 수 없습니다.");
        return 0;
    }

    char line[INDEX_ROW_BUFFER_SIZE];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    while (1) {
        long row_offset = ftell(data_file);
        if (fgets(line, sizeof(line), data_file) == NULL) break;

        int value_count;
        memset(values, 0, sizeof(values));
        if (!parse_csv_line(line, values, &value_count) || value_count != schema->column_count) {
            fclose(index_file);
            fclose(data_file);
            set_file_error(error, "기존 데이터 파일 행 형식이 잘못되었습니다.");
            return 0;
        }

        if (schema->columns[header->column_index].type == DATA_TYPE_INT &&
            values[header->column_index][0] == '\0') {
            continue;
        }

        IndexHeader current_header;
        if (!read_header(index_file, &current_header, error)) {
            fclose(index_file);
            fclose(data_file);
            return 0;
        }

        if (!insert_entry(index_file, &current_header, values[header->column_index], row_offset, error)) {
            fclose(index_file);
            fclose(data_file);
            return 0;
        }
    }

    fclose(index_file);
    fclose(data_file);
    return 1;
}

/* CREATE INDEX 문을 실행하여 B+ 트리 인덱스 파일을 생성하고 기존 행을 삽입한다.
 *
 * @param config     실행 설정 구조체 포인터
 * @param statement  CREATE INDEX AST 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
int create_index_from_statement(const AppConfig *config,
                                const CreateIndexStatement *statement,
                                ErrorInfo *error)
{
    TableSchema schema;
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) return 0;

    int column_index = -1;
    for (int i = 0; i < schema.column_count; i++) {
        if (strcmp(schema.columns[i].name, statement->column_name) == 0) {
            column_index = i;
            break;
        }
    }

    if (column_index < 0) {
        set_runtime_error(error, "CREATE INDEX 대상 컬럼이 스키마에 없습니다.",
                          statement->column_location);
        return 0;
    }

    char path[INDEX_MAX_PATH_LEN];
    build_index_path(path, sizeof(path), config->index_dir, statement->index_name);

    FILE *existing_file = fopen(path, "rb");
    if (existing_file != NULL) {
        fclose(existing_file);
        set_runtime_error(error, "같은 이름의 인덱스 파일이 이미 존재합니다.",
                          statement->index_location);
        return 0;
    }

    if (!initialize_index_file(path, statement->index_name, statement->table_name,
                               statement->column_name, column_index,
                               schema.columns[column_index].type, error)) {
        return 0;
    }

    IndexHeader header;
    memset(&header, 0, sizeof(header));
    snprintf(header.table_name, sizeof(header.table_name), "%s", statement->table_name);
    snprintf(header.column_name, sizeof(header.column_name), "%s", statement->column_name);
    header.column_index = column_index;
    header.column_type = schema.columns[column_index].type;

    if (!append_existing_rows(config, &schema, &header, path, error)) {
        remove(path);
        return 0;
    }

    return 1;
}

/* 인덱스 파일을 열고 헤더를 읽어 table_name, column_name이 일치하면 file과 header를 반환한다.
 * 일치하지 않으면 파일을 닫고 0을 반환한다.
 *
 * @param path         인덱스 파일 경로
 * @param table_name   기대하는 테이블 이름
 * @param column_name  기대하는 컬럼 이름
 * @param file         일치 시 열린 파일 포인터를 저장할 포인터
 * @param header       일치 시 헤더를 저장할 IndexHeader 포인터
 * @param error        오류 정보 저장 포인터
 * @return             일치하면 1, 아니면 0
 */
static int open_matching_index(const char *path,
                               const char *table_name,
                               const char *column_name,
                               FILE **file,
                               IndexHeader *header,
                               ErrorInfo *error)
{
    *file = fopen(path, "rb+");
    if (*file == NULL) return 0;

    if (!read_header(*file, header, error)) {
        fclose(*file);
        return 0;
    }

    if (strcmp(header->table_name, table_name) != 0 ||
        strcmp(header->column_name, column_name) != 0) {
        fclose(*file);
        return 0;
    }

    return 1;
}

/* 인덱스 헤더가 현재 스키마와 일치하는지 확인한다.
 *
 * @param header  검사할 인덱스 헤더 포인터
 * @param schema  기준 테이블 스키마 포인터
 * @return        일치하면 1, 아니면 0
 */
static int header_matches_current_schema(const IndexHeader *header,
                                         const TableSchema *schema)
{
    if (header->column_index < 0 || header->column_index >= schema->column_count) return 0;
    if (strcmp(schema->columns[header->column_index].name, header->column_name) != 0) return 0;
    return (int)schema->columns[header->column_index].type == header->column_type;
}

/* 삽입된 행의 값을 해당 테이블의 모든 인덱스 파일에 반영한다.
 *
 * @param config          실행 설정 구조체 포인터
 * @param schema          테이블 스키마 포인터
 * @param row_values      삽입된 행의 컬럼별 값 배열
 * @param row_offset      CSV 파일 내 행의 바이트 오프셋
 * @param changed_index   인덱스 수정이 시작됐는지 여부를 저장할 포인터
 * @param error           오류 정보 저장 포인터
 * @return                성공 시 1, 실패 시 0
 */
int update_all_indexes_for_row(const AppConfig *config,
                               const TableSchema *schema,
                               char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                               long row_offset,
                               int *changed_index,
                               ErrorInfo *error)
{
    char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN];
    int path_count;

    *changed_index = 0;
    if (!collect_table_index_paths(config, schema, paths, &path_count, error)) return 0;

    for (int path_index = 0; path_index < path_count; path_index++) {
        FILE *file = fopen(paths[path_index], "rb+");
        if (file == NULL) {
            set_file_error(error, "인덱스 파일을 열 수 없습니다.");
            return 0;
        }

        IndexHeader header;
        if (!read_header(file, &header, error)) { fclose(file); return 0; }
        if (!header_matches_current_schema(&header, schema)) {
            fclose(file);
            set_file_error(error, "인덱스와 현재 스키마가 맞지 않습니다.");
            return 0;
        }

        if (header.column_type == DATA_TYPE_INT &&
            row_values[header.column_index][0] == '\0') {
            fclose(file);
            continue;
        }

        *changed_index = 1;
        if (!insert_entry(file, &header, row_values[header.column_index], row_offset, error)) {
            fclose(file);
            return 0;
        }

        fclose(file);
    }

    return 1;
}

/* 인덱스 키가 WHERE 조건을 만족하는지 확인한다.
 *
 * @param data_type  컬럼 데이터 타입
 * @param index_key  인덱스 키 문자열
 * @param predicate  비교할 WHERE 조건 포인터
 * @return           조건 만족 시 1, 불만족 시 0
 */
static int key_matches_predicate(int data_type,
                                 const char *index_key,
                                 const Predicate *predicate)
{
    int compare_result = compare_keys(data_type, index_key, predicate->value.text);
    if (predicate->operator_type == COMPARE_EQUAL)         return compare_result == 0;
    if (predicate->operator_type == COMPARE_LESS)          return compare_result <  0;
    if (predicate->operator_type == COMPARE_LESS_EQUAL)    return compare_result <= 0;
    if (predicate->operator_type == COMPARE_GREATER)       return compare_result >  0;
    return compare_result >= 0;
}

/* 오프셋 배열에 새 오프셋을 추가한다. 최대 개수 초과 시 오류를 반환한다.
 *
 * @param offsets       오프셋 배열
 * @param offset_count  현재 오프셋 개수 포인터
 * @param row_offset    추가할 오프셋
 * @param error         오류 정보 저장 포인터
 * @return              성공 시 1, 최대 개수 초과 시 0
 */
static int append_offset(long offsets[SQLPROC_MAX_INDEX_RESULTS],
                         int *offset_count,
                         long row_offset,
                         ErrorInfo *error)
{
    if (*offset_count >= SQLPROC_MAX_INDEX_RESULTS) {
        set_file_error(error, "인덱스 결과 수가 최대 개수를 넘었습니다.");
        return 0;
    }
    offsets[*offset_count] = row_offset;
    *offset_count += 1;
    return 1;
}

/* 오프셋 배열을 오름차순으로 버블 정렬한다.
 *
 * @param offsets       정렬할 오프셋 배열
 * @param offset_count  배열 크기
 */
static void sort_offsets(long offsets[SQLPROC_MAX_INDEX_RESULTS], int offset_count)
{
    for (int i = 0; i < offset_count; i++) {
        for (int j = i + 1; j < offset_count; j++) {
            if (offsets[i] > offsets[j]) {
                long temp = offsets[i];
                offsets[i] = offsets[j];
                offsets[j] = temp;
            }
        }
    }
}

/* 인덱스 파일의 모든 리프 노드를 순회하며 WHERE 조건에 맞는 row 오프셋을 수집한다.
 * 수집 후 오프셋을 오름차순으로 정렬한다.
 *
 * @param file          열린 인덱스 파일 포인터
 * @param header        인덱스 헤더 포인터
 * @param predicate     비교할 WHERE 조건 포인터
 * @param offsets       결과 오프셋 배열
 * @param offset_count  결과 오프셋 개수를 저장할 포인터
 * @param error         오류 정보 저장 포인터
 * @return              성공 시 1, 실패 시 0
 */
static int collect_offsets_from_index(FILE *file,
                                      const IndexHeader *header,
                                      const Predicate *predicate,
                                      long offsets[SQLPROC_MAX_INDEX_RESULTS],
                                      int *offset_count,
                                      ErrorInfo *error)
{
    *offset_count = 0;

    for (int node_id = 0; node_id < header->next_node_id; node_id++) {
        BTreeNode leaf;
        if (!read_node(file, node_id, &leaf, error)) return 0;
        if (!leaf.is_leaf) continue;

        for (int i = 0; i < leaf.key_count; i++) {
            if (!key_matches_predicate(header->column_type, leaf.keys[i], predicate)) continue;
            if (!append_offset(offsets, offset_count, leaf.row_offsets[i], error)) return 0;
        }
    }

    sort_offsets(offsets, *offset_count);
    return 1;
}

/* 인덱스 디렉토리에서 해당 테이블과 관련된 모든 인덱스 파일 경로를 수집한다.
 *
 * @param config      실행 설정 구조체 포인터
 * @param schema      테이블 스키마 포인터
 * @param paths       결과 경로 배열
 * @param path_count  결과 경로 개수를 저장할 포인터
 * @param error       오류 정보 저장 포인터
 * @return            성공 시 1, 실패 시 0
 */
static int collect_table_index_paths(const AppConfig *config,
                                     const TableSchema *schema,
                                     char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN],
                                     int *path_count,
                                     ErrorInfo *error)
{
    *path_count = 0;

    DIR *directory = opendir(config->index_dir);
    if (directory == NULL) {
        if (errno == ENOENT) return 1;
        set_file_error(error, "인덱스 디렉터리를 열 수 없습니다.");
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strstr(entry->d_name, ".idx") == NULL) continue;

        char path[INDEX_MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", config->index_dir, entry->d_name);

        FILE *file = fopen(path, "rb");
        if (file == NULL) continue;

        IndexHeader header;
        if (!read_header(file, &header, error)) {
            fclose(file);
            closedir(directory);
            return 0;
        }
        fclose(file);

        if (strcmp(header.table_name, schema->table_name) != 0) continue;

        if (!header_matches_current_schema(&header, schema)) {
            set_file_error(error, "인덱스와 현재 스키마가 맞지 않습니다.");
            closedir(directory);
            return 0;
        }

        if (*path_count >= INDEX_MAX_TABLE_INDEXES) {
            set_file_error(error, "한 테이블에 연결된 인덱스 수가 너무 많습니다.");
            closedir(directory);
            return 0;
        }

        snprintf(paths[*path_count], INDEX_MAX_PATH_LEN, "%s", path);
        *path_count += 1;
    }

    closedir(directory);
    return 1;
}

/* 기존 인덱스 파일을 임시 파일로 재빌드한 뒤 원본 파일과 교체한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @param schema  테이블 스키마 포인터
 * @param path    재빌드할 인덱스 파일 경로
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
static int rebuild_single_index_file(const AppConfig *config,
                                     const TableSchema *schema,
                                     const char *path,
                                     ErrorInfo *error)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        set_file_error(error, "복구할 인덱스 파일을 열 수 없습니다.");
        return 0;
    }

    IndexHeader header;
    if (!read_header(file, &header, error)) { fclose(file); return 0; }
    fclose(file);

    if (!header_matches_current_schema(&header, schema)) {
        set_file_error(error, "인덱스와 현재 스키마가 맞지 않습니다.");
        return 0;
    }

    char temp_path[INDEX_MAX_PATH_LEN];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    remove(temp_path);

    if (!initialize_index_file(temp_path, header.index_name, header.table_name,
                               header.column_name, header.column_index,
                               header.column_type, error)) {
        remove(temp_path);
        return 0;
    }

    IndexHeader rebuild_header;
    memset(&rebuild_header, 0, sizeof(rebuild_header));
    snprintf(rebuild_header.table_name, sizeof(rebuild_header.table_name), "%s", header.table_name);
    snprintf(rebuild_header.column_name, sizeof(rebuild_header.column_name), "%s", header.column_name);
    rebuild_header.column_index = header.column_index;
    rebuild_header.column_type = header.column_type;

    if (!append_existing_rows(config, schema, &rebuild_header, temp_path, error)) {
        remove(temp_path);
        return 0;
    }

    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        set_file_error(error, "복구한 인덱스 파일을 교체할 수 없습니다.");
        return 0;
    }

    return 1;
}

/* 테이블의 모든 인덱스 파일을 현재 CSV 데이터 기준으로 재빌드한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @param schema  테이블 스키마 포인터
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
int rebuild_indexes_for_table(const AppConfig *config,
                              const TableSchema *schema,
                              ErrorInfo *error)
{
    char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN];
    int path_count;

    if (!collect_table_index_paths(config, schema, paths, &path_count, error)) return 0;

    for (int i = 0; i < path_count; i++) {
        if (!rebuild_single_index_file(config, schema, paths[i], error)) return 0;
    }

    return 1;
}

/* WHERE 절에 사용할 인덱스를 탐색하여 조건에 맞는 row 오프셋 목록을 반환한다.
 * 등호(=) 인덱스를 범위 인덱스보다 우선 선택하며, 선택된 인덱스 하나만 사용한다.
 * WHERE 절이 없거나 인덱스가 없으면 used_index를 0으로 설정하고 성공을 반환한다. 
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
                                     ErrorInfo *error)
{
    *offset_count = 0;
    *used_index = 0;

    if (statement->where_clause.count == 0) return 1;

    char chosen_path[INDEX_MAX_PATH_LEN];
    chosen_path[0] = '\0';
    int chosen_predicate = -1;
    int prefer_equality = 0;

    DIR *directory = opendir(config->index_dir);
    if (directory == NULL) {
        if (errno == ENOENT) return 1;
        set_file_error(error, "인덱스 디렉터리를 열 수 없습니다.");
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strstr(entry->d_name, ".idx") == NULL) continue;

        for (int predicate_index = 0;
             predicate_index < statement->where_clause.count;
             predicate_index++) {
            if (chosen_predicate >= 0 && prefer_equality) continue;

            char path[INDEX_MAX_PATH_LEN];
            snprintf(path, sizeof(path), "%s/%s", config->index_dir, entry->d_name);

            FILE *file;
            IndexHeader header;
            if (!open_matching_index(path, schema->table_name,
                                     statement->where_clause.items[predicate_index].column_name,
                                     &file, &header, error)) {
                continue;
            }
            fclose(file);

            snprintf(chosen_path, sizeof(chosen_path), "%s", path);
            chosen_predicate = predicate_index;
            if (statement->where_clause.items[predicate_index].operator_type == COMPARE_EQUAL) {
                prefer_equality = 1;
            }
        }
    }
    closedir(directory);

    if (chosen_predicate < 0) return 1;

    {
        FILE *file = fopen(chosen_path, "rb");
        if (file == NULL) {
            set_file_error(error, "선택한 인덱스 파일을 열 수 없습니다.");
            return 0;
        }

        IndexHeader header;
        if (!read_header(file, &header, error)) { fclose(file); return 0; }

        if (!header_matches_current_schema(&header, schema)) {
            fclose(file);
            set_file_error(error, "인덱스와 현재 스키마가 맞지 않습니다.");
            return 0;
        }
        
        // 인덱스 결과 수가 너무 많으면 인덱스 사용을 포기하고, 
        // full scan으로 되돌릴 수 있도록 *used_index를 0으로 돌려준다. (full scan fallback)
        if (!collect_offsets_from_index(file, &header,
                                        &statement->where_clause.items[chosen_predicate],
                                        offsets, offset_count, error)) {
            if (strcmp(error->message, "인덱스 결과 수가 최대 개수를 넘었습니다.") == 0) {
                memset(error, 0, sizeof(*error));
                *offset_count = 0;
                *used_index = 0;
                fclose(file);
                return 1;
            }
            fclose(file);
            return 0;
        }

        fclose(file);
    }

    *used_index = 1;
    return 1;
}
