#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

/*
 * btree_index.c는 디스크 영속형 B+ 트리 인덱스를 담당합니다.
 * 역할:
 * - CREATE INDEX 시 .idx 파일 생성
 * - INSERT 뒤 인덱스 엔트리 추가
 * - SELECT WHERE에서 인덱스를 이용해 row offset 후보 수집
 * - 필요 시 CSV를 기준으로 인덱스 재구축
 */

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

/*
 * IndexHeader는 .idx 파일 맨 앞에 저장되는 메타데이터입니다.
 * - 어떤 테이블/컬럼 인덱스인지
 * - 루트 노드가 어디인지
 * - 다음 새 노드 id가 무엇인지
 */

/*
 * leaf와 internal 노드를 같은 크기의 구조체로 저장합니다.
 * - leaf: keys + row_offsets + next_leaf_id 사용
 * - internal: keys + child_ids 사용
 *
 * 이렇게 하면 node id에서 파일 위치를 계산할 때
 * `sizeof(BTreeNode)`만큼 곱해 바로 이동할 수 있습니다.
 */
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

static void set_file_error(ErrorInfo *error, const char *message)
{
    /* 인덱스 파일 입출력 오류는 SQL 위치 없이 메시지만 기록합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    /* CREATE INDEX 문 자체 문제는 SQL 위치 정보와 함께 저장합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

static void build_index_path(char *dest,
                             size_t dest_size,
                             const char *index_dir,
                             const char *index_name)
{
    /* index_dir/idx_users_age.idx 같은 인덱스 파일 경로를 조립합니다. */
    snprintf(dest, dest_size, "%s/%s.idx", index_dir, index_name);
}

static long node_offset(int node_id)
{
    /* node id를 실제 파일 오프셋으로 바꾸는 계산 함수입니다. */
    return (long)sizeof(IndexHeader) + ((long)node_id * (long)sizeof(BTreeNode));
}

static int parse_int_text(const char *text, long *value)
{
    char *end_pointer;

    /* 문자열 키를 정수 키로 해석할 수 있는지 확인하면서 long으로 바꿉니다. */
    if (text[0] == '\0') {
        return 0;
    }

    *value = strtol(text, &end_pointer, 10);
    return *end_pointer == '\0';
}

static int compare_keys(int data_type, const char *left, const char *right)
{
    /* 인덱스 키 타입에 따라 숫자 비교 또는 문자열 비교를 수행합니다. */
    if (data_type == DATA_TYPE_INT) {
        long left_value;
        long right_value;

        parse_int_text(left, &left_value);
        parse_int_text(right, &right_value);

        if (left_value < right_value) {
            return -1;
        }

        if (left_value > right_value) {
            return 1;
        }

        return 0;
    }

    return strcmp(left, right);
}

static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count)
{
    int in_quotes;
    int row_index;
    int text_index;
    int i;

    /* CSV 한 줄을 컬럼 배열로 분해합니다. 인덱스 재구축과 검증에서 재사용합니다. */
    in_quotes = 0;
    row_index = 0;
    text_index = 0;

    for (i = 0; line[i] != '\0' && line[i] != '\n' && line[i] != '\r'; i++) {
        if (row_index >= SQLPROC_MAX_COLUMNS) {
            return 0;
        }

        if (line[i] == '"') {
            if (in_quotes && line[i + 1] == '"') {
                if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
                    return 0;
                }

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

        if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
            return 0;
        }

        values[row_index][text_index] = line[i];
        text_index += 1;
    }

    values[row_index][text_index] = '\0';
    *value_count = row_index + 1;
    return 1;
}

static int validate_header(FILE *file, const TableSchema *schema, ErrorInfo *error)
{
    char line[INDEX_ROW_BUFFER_SIZE];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int value_count;
    int i;

    /* 인덱스를 만들거나 재구축하기 전에 CSV 헤더가 현재 스키마와 맞는지 확인합니다. */
    if (fgets(line, sizeof(line), file) == NULL) {
        set_file_error(error, "CSV 헤더를 읽을 수 없습니다.");
        return 0;
    }

    if (!parse_csv_line(line, values, &value_count)) {
        set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
        return 0;
    }

    if (value_count != schema->column_count) {
        set_file_error(error, "CSV 헤더가 스키마와 다릅니다.");
        return 0;
    }

    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(values[i], schema->columns[i].name) != 0) {
            set_file_error(error, "CSV 헤더 순서가 스키마와 다릅니다.");
            return 0;
        }
    }

    return 1;
}

static int read_header(FILE *file, IndexHeader *header, ErrorInfo *error)
{
    /* 인덱스 파일 헤더를 읽고 magic/version까지 검증합니다. */
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

static int write_header(FILE *file, const IndexHeader *header, ErrorInfo *error)
{
    /* 메모리의 IndexHeader를 파일 맨 앞에 다시 저장합니다. */
    rewind(file);

    if (fwrite(header, sizeof(*header), 1, file) != 1) {
        set_file_error(error, "인덱스 헤더를 저장할 수 없습니다.");
        return 0;
    }

    fflush(file);
    return 1;
}

static int read_node(FILE *file, int node_id, BTreeNode *node, ErrorInfo *error)
{
    /* 특정 node id 위치의 B+ 트리 노드를 파일에서 읽습니다. */
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

static int write_node(FILE *file, int node_id, const BTreeNode *node, ErrorInfo *error)
{
    /* 특정 node id 위치에 B+ 트리 노드를 저장합니다. */
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

static int allocate_node(FILE *file, IndexHeader *header, int *node_id, ErrorInfo *error)
{
    BTreeNode node;

    /* next_node_id를 사용해 새 빈 노드를 하나 할당하고 파일에 기록합니다. */
    memset(&node, 0, sizeof(node));
    node.next_leaf_id = -1;

    *node_id = header->next_node_id;
    header->next_node_id += 1;

    if (!write_header(file, header, error)) {
        return 0;
    }

    return write_node(file, *node_id, &node, error);
}

/*
 * 무엇을 하는가:
 * - 루트에서 시작해 주어진 키가 들어갈 leaf 노드를 찾습니다.
 *
 * 왜 필요한가:
 * - B+ 트리 삽입과 조회는 모두 leaf에서 실제 데이터 엔트리를 다루므로,
 *   먼저 leaf 위치를 정확히 찾는 단계가 필요합니다.
 *
 * 입력과 출력:
 * - 입력: 열려 있는 인덱스 파일, 헤더, 찾고 싶은 키
 * - 출력: leaf 노드 id와, 그 leaf까지 내려오며 지나간 내부 노드 경로
 *
 * 핵심 흐름:
 * - 내부 노드에서는 분리 키를 비교해 child를 하나 고르고,
 *   leaf를 만날 때까지 같은 과정을 반복합니다.
 */
static int find_leaf_node(FILE *file,
                          const IndexHeader *header,
                          const char *key,
                          int path[INDEX_MAX_PATH_DEPTH],
                          int *path_length,
                          int *leaf_id,
                          ErrorInfo *error)
{
    int node_id;

    *path_length = 0;
    node_id = header->root_node_id;

    while (1) {
        BTreeNode node;
        int child_index;

        if (!read_node(file, node_id, &node, error)) {
            return 0;
        }

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

        child_index = 0;
        while (child_index < node.key_count &&
               compare_keys(header->column_type, key, node.keys[child_index]) >= 0) {
            child_index += 1;
        }

        node_id = node.child_ids[child_index];
    }
}

static void insert_into_leaf(BTreeNode *leaf,
                             const IndexHeader *header,
                             const char *key,
                             long row_offset)
{
    int insert_index;
    int i;

    insert_index = 0;
    while (insert_index < leaf->key_count) {
        int compare_result;

        compare_result = compare_keys(header->column_type, leaf->keys[insert_index], key);
        if (compare_result > 0) {
            break;
        }

        if (compare_result == 0 && leaf->row_offsets[insert_index] > row_offset) {
            break;
        }

        insert_index += 1;
    }

    for (i = leaf->key_count; i > insert_index; i--) {
        snprintf(leaf->keys[i], sizeof(leaf->keys[i]), "%s", leaf->keys[i - 1]);
        leaf->row_offsets[i] = leaf->row_offsets[i - 1];
    }

    snprintf(leaf->keys[insert_index], sizeof(leaf->keys[insert_index]), "%s", key);
    leaf->row_offsets[insert_index] = row_offset;
    leaf->key_count += 1;
}

/*
 * 무엇을 하는가:
 * - 꽉 찬 leaf 노드를 두 개의 leaf로 나누고, 오른쪽 leaf의 첫 키를 부모에
 *   올릴 준비를 합니다.
 *
 * 왜 필요한가:
 * - leaf에 더 이상 빈 자리가 없을 때도 새 엔트리를 유지하려면 노드를
 *   둘로 나눠 B+ 트리 규칙을 지켜야 하기 때문입니다.
 *
 * 입력과 출력:
 * - 입력: 꽉 찬 leaf, 새 키/오프셋, 새로 할당한 오른쪽 leaf id
 * - 출력: 수정된 왼쪽 leaf, 새 오른쪽 leaf, 부모에 올릴 분리 키
 *
 * 핵심 흐름:
 * - 기존 값과 새 값을 임시 배열에 정렬해 넣고,
 *   왼쪽과 오른쪽으로 나눠 다시 leaf 두 개에 채워 넣습니다.
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
    int insert_index;
    int i;
    int total_keys;
    int left_count;

    memset(right_leaf, 0, sizeof(*right_leaf));
    right_leaf->is_leaf = 1;
    right_leaf->next_leaf_id = left_leaf->next_leaf_id;

    total_keys = SQLPROC_BTREE_MAX_KEYS + 1;
    insert_index = 0;

    while (insert_index < left_leaf->key_count) {
        int compare_result;

        compare_result = compare_keys(header->column_type, left_leaf->keys[insert_index], key);
        if (compare_result > 0) {
            break;
        }

        if (compare_result == 0 && left_leaf->row_offsets[insert_index] > row_offset) {
            break;
        }

        insert_index += 1;
    }

    for (i = 0; i < insert_index; i++) {
        snprintf(temp_keys[i], sizeof(temp_keys[i]), "%s", left_leaf->keys[i]);
        temp_offsets[i] = left_leaf->row_offsets[i];
    }

    snprintf(temp_keys[insert_index], sizeof(temp_keys[insert_index]), "%s", key);
    temp_offsets[insert_index] = row_offset;

    for (i = insert_index; i < left_leaf->key_count; i++) {
        snprintf(temp_keys[i + 1], sizeof(temp_keys[i + 1]), "%s", left_leaf->keys[i]);
        temp_offsets[i + 1] = left_leaf->row_offsets[i];
    }

    left_count = (total_keys + 1) / 2;
    memset(left_leaf->keys, 0, sizeof(left_leaf->keys));
    memset(left_leaf->row_offsets, 0, sizeof(left_leaf->row_offsets));
    left_leaf->key_count = left_count;

    for (i = 0; i < left_count; i++) {
        snprintf(left_leaf->keys[i], sizeof(left_leaf->keys[i]), "%s", temp_keys[i]);
        left_leaf->row_offsets[i] = temp_offsets[i];
    }

    right_leaf->key_count = total_keys - left_count;
    for (i = 0; i < right_leaf->key_count; i++) {
        snprintf(right_leaf->keys[i], sizeof(right_leaf->keys[i]), "%s", temp_keys[left_count + i]);
        right_leaf->row_offsets[i] = temp_offsets[left_count + i];
    }

    left_leaf->next_leaf_id = right_leaf_id;
    snprintf(promoted_key, SQLPROC_MAX_VALUE_LEN, "%s", right_leaf->keys[0]);
}

/*
 * 무엇을 하는가:
 * - 꽉 찬 내부 노드에 새 분리 키를 넣은 뒤 왼쪽/오른쪽 내부 노드로 나누고,
 *   가운데 키를 상위 부모로 올립니다.
 *
 * 왜 필요한가:
 * - B+ 트리에서 내부 노드도 최대 키 개수를 넘길 수 없기 때문에,
 *   상위로 분할을 전파할 준비가 필요합니다.
 *
 * 입력과 출력:
 * - 입력: 기존 내부 노드, 삽입할 키/오른쪽 자식, 새 오른쪽 내부 노드 id
 * - 출력: 수정된 왼쪽 내부 노드, 새 오른쪽 내부 노드, 상위 부모로 올릴 키
 *
 * 핵심 흐름:
 * - 먼저 부모 안에서 `left_child_id`가 있던 자리를 찾아 삽입 위치를 정합니다.
 * - 그 위치 기준으로 새 분리 키와 오른쪽 자식을 임시 배열에 끼워 넣습니다.
 * - 가운데 키 하나는 상위 부모로 올리고, 나머지를 좌우 노드에 나눠 다시 씁니다.
 */
static void split_internal_node(const IndexHeader *header,
                                BTreeNode *left_node,
                                const char *key,
                                int right_child_id,
                                int left_child_id,
                                BTreeNode *right_node,
                                char promoted_key[SQLPROC_MAX_VALUE_LEN])
{
    char temp_keys[SQLPROC_BTREE_MAX_KEYS + 1][SQLPROC_MAX_VALUE_LEN];
    int temp_children[SQLPROC_BTREE_MAX_KEYS + 2];
    int insert_index;
    int i;
    int total_keys;
    int middle_index;

    (void)header;
    memset(right_node, 0, sizeof(*right_node));
    right_node->is_leaf = 0;

    insert_index = 0;
    while (insert_index <= left_node->key_count &&
           left_node->child_ids[insert_index] != left_child_id) {
        insert_index += 1;
    }

    for (i = 0; i < insert_index; i++) {
        temp_children[i] = left_node->child_ids[i];
    }

    temp_children[insert_index] = left_child_id;
    temp_children[insert_index + 1] = right_child_id;

    for (i = insert_index + 1; i <= left_node->key_count; i++) {
        temp_children[i + 1] = left_node->child_ids[i];
    }

    for (i = 0; i < insert_index; i++) {
        snprintf(temp_keys[i], sizeof(temp_keys[i]), "%s", left_node->keys[i]);
    }

    snprintf(temp_keys[insert_index], sizeof(temp_keys[insert_index]), "%s", key);

    for (i = insert_index; i < left_node->key_count; i++) {
        snprintf(temp_keys[i + 1], sizeof(temp_keys[i + 1]), "%s", left_node->keys[i]);
    }

    total_keys = SQLPROC_BTREE_MAX_KEYS + 1;
    middle_index = total_keys / 2;
    snprintf(promoted_key, SQLPROC_MAX_VALUE_LEN, "%s", temp_keys[middle_index]);

    memset(left_node->keys, 0, sizeof(left_node->keys));
    memset(left_node->child_ids, 0, sizeof(left_node->child_ids));
    left_node->key_count = middle_index;

    for (i = 0; i < middle_index; i++) {
        snprintf(left_node->keys[i], sizeof(left_node->keys[i]), "%s", temp_keys[i]);
        left_node->child_ids[i] = temp_children[i];
    }
    left_node->child_ids[middle_index] = temp_children[middle_index];

    right_node->key_count = total_keys - middle_index - 1;
    for (i = 0; i < right_node->key_count; i++) {
        snprintf(right_node->keys[i], sizeof(right_node->keys[i]), "%s", temp_keys[middle_index + 1 + i]);
        right_node->child_ids[i] = temp_children[middle_index + 1 + i];
    }
    right_node->child_ids[right_node->key_count] =
        temp_children[middle_index + 1 + right_node->key_count];
}

/*
 * 무엇을 하는가:
 * - 분리된 자식 노드의 분리 키를 부모 내부 노드에 반영합니다.
 *
 * 왜 필요한가:
 * - leaf나 내부 노드가 분할되면 상위 부모가 새 오른쪽 자식을 알도록
 *   연결 정보를 갱신해야 트리 전체 탐색 경로가 유지됩니다.
 *
 * 입력과 출력:
 * - 입력: 왼쪽 자식 id, 부모로 올릴 키, 오른쪽 자식 id, 현재 부모 경로
 * - 출력: 필요하면 새 루트를 만들거나 상위 부모까지 분할을 전파합니다.
 *
 * 핵심 흐름:
 * - 부모가 없으면 새 루트를 만들어 왼쪽/오른쪽 자식을 바로 연결합니다.
 * - 부모에 자리가 있으면 그 자리에서 끝납니다.
 * - 부모도 꽉 차 있으면 방금 쪼갠 부모를 새로운 `left_child_id`로 보고,
 *   새로 생긴 오른쪽 내부 노드와 함께 한 단계 위 부모로 계속 올라갑니다.
 *
 * 재귀 대신 반복문을 쓰는 이유:
 * - 초심자가 호출 스택보다 "현재 부모를 고치고, 필요하면 한 단계 위로 간다"
 *   는 흐름으로 따라가기 쉽도록 하기 위해서입니다.
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
        BTreeNode new_root;
        int new_root_id;

        if (!allocate_node(file, header, &new_root_id, error)) {
            return 0;
        }

        memset(&new_root, 0, sizeof(new_root));
        new_root.is_leaf = 0;
        new_root.key_count = 1;
        snprintf(new_root.keys[0], sizeof(new_root.keys[0]), "%s", current_key);
        new_root.child_ids[0] = left_child_id;
        new_root.child_ids[1] = right_child_id;
        header->root_node_id = new_root_id;

        if (!write_header(file, header, error)) {
            return 0;
        }

        return write_node(file, new_root_id, &new_root, error);
    }

    while (path_length > 0) {
        int parent_id;
        BTreeNode parent;

        parent_id = path[path_length - 1];
        if (!read_node(file, parent_id, &parent, error)) {
            return 0;
        }

        if (parent.key_count < SQLPROC_BTREE_MAX_KEYS) {
            int insert_index;
            int i;

            insert_index = 0;
            while (insert_index <= parent.key_count &&
                   parent.child_ids[insert_index] != left_child_id) {
                insert_index += 1;
            }

            for (i = parent.key_count; i > insert_index; i--) {
                snprintf(parent.keys[i], sizeof(parent.keys[i]), "%s", parent.keys[i - 1]);
            }

            for (i = parent.key_count + 1; i > insert_index + 1; i--) {
                parent.child_ids[i] = parent.child_ids[i - 1];
            }

            snprintf(parent.keys[insert_index], sizeof(parent.keys[insert_index]), "%s", current_key);
            parent.child_ids[insert_index + 1] = right_child_id;
            parent.key_count += 1;
            return write_node(file, parent_id, &parent, error);
        }

        {
            BTreeNode right_node;
            char promoted_key[SQLPROC_MAX_VALUE_LEN];
            int new_right_id;

            if (!allocate_node(file, header, &new_right_id, error)) {
                return 0;
            }

            split_internal_node(header,
                                &parent,
                                current_key,
                                right_child_id,
                                left_child_id,
                                &right_node,
                                promoted_key);

            if (!write_node(file, parent_id, &parent, error)) {
                return 0;
            }

            if (!write_node(file, new_right_id, &right_node, error)) {
                return 0;
            }

            left_child_id = parent_id;
            right_child_id = new_right_id;
            snprintf(current_key, sizeof(current_key), "%s", promoted_key);
            path_length -= 1;
        }
    }

    {
        BTreeNode new_root;
        int new_root_id;

        if (!allocate_node(file, header, &new_root_id, error)) {
            return 0;
        }

        memset(&new_root, 0, sizeof(new_root));
        new_root.is_leaf = 0;
        new_root.key_count = 1;
        snprintf(new_root.keys[0], sizeof(new_root.keys[0]), "%s", current_key);
        new_root.child_ids[0] = left_child_id;
        new_root.child_ids[1] = right_child_id;
        header->root_node_id = new_root_id;

        if (!write_header(file, header, error)) {
            return 0;
        }

        return write_node(file, new_root_id, &new_root, error);
    }
}

static int insert_entry(FILE *file,
                        IndexHeader *header,
                        const char *key,
                        long row_offset,
                        ErrorInfo *error)
{
    int path[INDEX_MAX_PATH_DEPTH];
    int path_length;
    int leaf_id;
    BTreeNode leaf;

    if (!find_leaf_node(file, header, key, path, &path_length, &leaf_id, error)) {
        return 0;
    }

    if (!read_node(file, leaf_id, &leaf, error)) {
        return 0;
    }

    if (leaf.key_count < SQLPROC_BTREE_MAX_KEYS) {
        insert_into_leaf(&leaf, header, key, row_offset);
        return write_node(file, leaf_id, &leaf, error);
    }

    {
        BTreeNode right_leaf;
        int right_leaf_id;
        char promoted_key[SQLPROC_MAX_VALUE_LEN];

        if (!allocate_node(file, header, &right_leaf_id, error)) {
            return 0;
        }

        split_leaf_node(header,
                        &leaf,
                        key,
                        row_offset,
                        right_leaf_id,
                        &right_leaf,
                        promoted_key);

        if (!write_node(file, leaf_id, &leaf, error)) {
            return 0;
        }

        if (!write_node(file, right_leaf_id, &right_leaf, error)) {
            return 0;
        }

        return insert_into_parent(file,
                                  header,
                                  leaf_id,
                                  promoted_key,
                                  right_leaf_id,
                                  path,
                                  path_length,
                                  error);
    }
}

static int initialize_index_file(const char *path,
                                 const char *index_name,
                                 const char *table_name,
                                 const char *column_name,
                                 int column_index,
                                 int column_type,
                                 ErrorInfo *error)
{
    FILE *file;
    IndexHeader header;
    BTreeNode root;

    file = fopen(path, "wb+");
    if (file == NULL) {
        set_file_error(error, "인덱스 파일을 만들 수 없습니다.");
        return 0;
    }

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

    if (!write_header(file, &header, error)) {
        fclose(file);
        return 0;
    }

    if (!write_node(file, 0, &root, error)) {
        fclose(file);
        return 0;
    }

    fclose(file);
    return 1;
}

static int append_existing_rows(const AppConfig *config,
                                const TableSchema *schema,
                                const IndexHeader *header,
                                const char *path,
                                ErrorInfo *error)
{
    char data_path[INDEX_MAX_PATH_LEN];
    char line[INDEX_ROW_BUFFER_SIZE];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    FILE *data_file;
    FILE *index_file;

    snprintf(data_path, sizeof(data_path), "%s/%s.csv", config->data_dir, schema->table_name);

    data_file = fopen(data_path, "rb");
    if (data_file == NULL) {
        if (errno == ENOENT) {
            return 1;
        }

        set_file_error(error, "기존 데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!validate_header(data_file, schema, error)) {
        fclose(data_file);
        return 0;
    }

    index_file = fopen(path, "rb+");
    if (index_file == NULL) {
        fclose(data_file);
        set_file_error(error, "생성한 인덱스 파일을 다시 열 수 없습니다.");
        return 0;
    }

    while (1) {
        long row_offset;
        int value_count;
        IndexHeader current_header;

        row_offset = ftell(data_file);
        if (fgets(line, sizeof(line), data_file) == NULL) {
            break;
        }

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

        if (!read_header(index_file, &current_header, error)) {
            fclose(index_file);
            fclose(data_file);
            return 0;
        }

        if (!insert_entry(index_file,
                          &current_header,
                          values[header->column_index],
                          row_offset,
                          error)) {
            fclose(index_file);
            fclose(data_file);
            return 0;
        }
    }

    fclose(index_file);
    fclose(data_file);
    return 1;
}

int create_index_from_statement(const AppConfig *config,
                                const CreateIndexStatement *statement,
                                ErrorInfo *error)
{
    TableSchema schema;
    char path[INDEX_MAX_PATH_LEN];
    FILE *existing_file;
    int column_index;
    IndexHeader header;
    int i;

    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    column_index = -1;
    for (i = 0; i < schema.column_count; i++) {
        if (strcmp(schema.columns[i].name, statement->column_name) == 0) {
            column_index = i;
            break;
        }
    }

    if (column_index < 0) {
        set_runtime_error(error,
                          "CREATE INDEX 대상 컬럼이 스키마에 없습니다.",
                          statement->column_location);
        return 0;
    }

    build_index_path(path, sizeof(path), config->index_dir, statement->index_name);
    existing_file = fopen(path, "rb");
    if (existing_file != NULL) {
        fclose(existing_file);
        set_runtime_error(error,
                          "같은 이름의 인덱스 파일이 이미 존재합니다.",
                          statement->index_location);
        return 0;
    }

    if (!initialize_index_file(path,
                               statement->index_name,
                               statement->table_name,
                               statement->column_name,
                               column_index,
                               schema.columns[column_index].type,
                               error)) {
        return 0;
    }

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

static int open_matching_index(const char *path,
                               const char *table_name,
                               const char *column_name,
                               FILE **file,
                               IndexHeader *header,
                               ErrorInfo *error)
{
    *file = fopen(path, "rb+");
    if (*file == NULL) {
        return 0;
    }

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

static int header_matches_current_schema(const IndexHeader *header,
                                         const TableSchema *schema)
{
    if (header->column_index < 0 || header->column_index >= schema->column_count) {
        return 0;
    }

    if (strcmp(schema->columns[header->column_index].name, header->column_name) != 0) {
        return 0;
    }

    return (int)schema->columns[header->column_index].type == header->column_type;
}

int update_all_indexes_for_row(const AppConfig *config,
                               const TableSchema *schema,
                               char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                               long row_offset,
                               int *changed_index,
                               ErrorInfo *error)
{
    char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN];
    int path_count;
    int path_index;

    *changed_index = 0;
    if (!collect_table_index_paths(config, schema, paths, &path_count, error)) {
        return 0;
    }

    for (path_index = 0; path_index < path_count; path_index++) {
        FILE *file;
        IndexHeader header;

        file = fopen(paths[path_index], "rb+");
        if (file == NULL) {
            set_file_error(error, "인덱스 파일을 열 수 없습니다.");
            return 0;
        }

        if (!read_header(file, &header, error)) {
            fclose(file);
            return 0;
        }

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

        /*
         * insert_entry는 노드 할당과 여러 번의 파일 쓰기를 포함합니다.
         * 중간에 실패해도 인덱스 파일이 부분 수정됐을 수 있으므로,
         * 실제 삽입을 시작하기 전에 복구 필요 상태를 먼저 표시합니다.
         */
        *changed_index = 1;
        if (!insert_entry(file, &header, row_values[header.column_index], row_offset, error)) {
            fclose(file);
            return 0;
        }

        fclose(file);
    }

    return 1;
}

static int key_matches_predicate(int data_type,
                                 const char *index_key,
                                 const Predicate *predicate)
{
    int compare_result;

    compare_result = compare_keys(data_type, index_key, predicate->value.text);

    if (predicate->operator_type == COMPARE_EQUAL) {
        return compare_result == 0;
    }

    if (predicate->operator_type == COMPARE_LESS) {
        return compare_result < 0;
    }

    if (predicate->operator_type == COMPARE_LESS_EQUAL) {
        return compare_result <= 0;
    }

    if (predicate->operator_type == COMPARE_GREATER) {
        return compare_result > 0;
    }

    return compare_result >= 0;
}

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

static void sort_offsets(long offsets[SQLPROC_MAX_INDEX_RESULTS], int offset_count)
{
    int i;
    int j;

    for (i = 0; i < offset_count; i++) {
        for (j = i + 1; j < offset_count; j++) {
            if (offsets[i] > offsets[j]) {
                long temp;

                temp = offsets[i];
                offsets[i] = offsets[j];
                offsets[j] = temp;
            }
        }
    }
}

/*
 * 무엇을 하는가:
 * - 선택된 인덱스 파일을 따라가며 WHERE 조건에 맞는 row offset 후보를
 *   수집합니다.
 *
 * 왜 필요한가:
 * - 실행기가 CSV 전체를 돌지 않고도 후보 행만 빠르게 읽을 수 있게 해,
 *   인덱스 기반 조회 경로를 만들기 위해서입니다.
 *
 * 입력과 출력:
 * - 입력: 열려 있는 인덱스 파일, 인덱스 헤더, 사용할 predicate
 * - 출력: 조건에 맞는 row offset 배열과 개수
 *
 * 핵심 흐름:
 * - 현재 구현은 모든 leaf 노드를 한 번씩 읽어 후보를 모읍니다.
 * - leaf 안의 키를 predicate와 비교해 맞는 row offset만 담고,
 *   마지막에는 row offset 순서로 다시 정렬해 조회 결과를 안정적으로 맞춥니다.
 */
static int collect_offsets_from_index(FILE *file,
                                      const IndexHeader *header,
                                      const Predicate *predicate,
                                      long offsets[SQLPROC_MAX_INDEX_RESULTS],
                                      int *offset_count,
                                      ErrorInfo *error)
{
    int node_id;

    *offset_count = 0;

    for (node_id = 0; node_id < header->next_node_id; node_id++) {
        BTreeNode leaf;
        int i;

        if (!read_node(file, node_id, &leaf, error)) {
            return 0;
        }

        if (!leaf.is_leaf) {
            continue;
        }

        for (i = 0; i < leaf.key_count; i++) {
            if (!key_matches_predicate(header->column_type,
                                       leaf.keys[i],
                                       predicate)) {
                continue;
            }

            if (!append_offset(offsets, offset_count, leaf.row_offsets[i], error)) {
                return 0;
            }
        }
    }

    sort_offsets(offsets, *offset_count);
    return 1;
}

static int collect_table_index_paths(const AppConfig *config,
                                     const TableSchema *schema,
                                     char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN],
                                     int *path_count,
                                     ErrorInfo *error)
{
    DIR *directory;
    struct dirent *entry;

    *path_count = 0;
    directory = opendir(config->index_dir);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return 1;
        }

        set_file_error(error, "인덱스 디렉터리를 열 수 없습니다.");
        return 0;
    }

    while ((entry = readdir(directory)) != NULL) {
        char path[INDEX_MAX_PATH_LEN];
        FILE *file;
        IndexHeader header;

        if (strstr(entry->d_name, ".idx") == NULL) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", config->index_dir, entry->d_name);
        file = fopen(path, "rb");
        if (file == NULL) {
            continue;
        }

        if (!read_header(file, &header, error)) {
            fclose(file);
            closedir(directory);
            return 0;
        }

        fclose(file);

        if (strcmp(header.table_name, schema->table_name) != 0) {
            continue;
        }

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

static int rebuild_single_index_file(const AppConfig *config,
                                     const TableSchema *schema,
                                     const char *path,
                                     ErrorInfo *error)
{
    FILE *file;
    IndexHeader header;
    IndexHeader rebuild_header;
    char temp_path[INDEX_MAX_PATH_LEN];

    file = fopen(path, "rb");
    if (file == NULL) {
        set_file_error(error, "복구할 인덱스 파일을 열 수 없습니다.");
        return 0;
    }

    if (!read_header(file, &header, error)) {
        fclose(file);
        return 0;
    }

    fclose(file);

    if (!header_matches_current_schema(&header, schema)) {
        set_file_error(error, "인덱스와 현재 스키마가 맞지 않습니다.");
        return 0;
    }

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    remove(temp_path);

    if (!initialize_index_file(temp_path,
                               header.index_name,
                               header.table_name,
                               header.column_name,
                               header.column_index,
                               header.column_type,
                               error)) {
        remove(temp_path);
        return 0;
    }

    memset(&rebuild_header, 0, sizeof(rebuild_header));
    snprintf(rebuild_header.table_name,
             sizeof(rebuild_header.table_name),
             "%s",
             header.table_name);
    snprintf(rebuild_header.column_name,
             sizeof(rebuild_header.column_name),
             "%s",
             header.column_name);
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

int rebuild_indexes_for_table(const AppConfig *config,
                              const TableSchema *schema,
                              ErrorInfo *error)
{
    char paths[INDEX_MAX_TABLE_INDEXES][INDEX_MAX_PATH_LEN];
    int path_count;
    int i;

    if (!collect_table_index_paths(config, schema, paths, &path_count, error)) {
        return 0;
    }

    for (i = 0; i < path_count; i++) {
        if (!rebuild_single_index_file(config, schema, paths[i], error)) {
            return 0;
        }
    }

    return 1;
}

/*
 * 인덱스 선택 규칙:
 * - WHERE 조건 중 인덱스가 있는 조건이 하나라도 있으면 그 후보를 사용합니다.
 * - 동등 비교(`=`) 인덱스가 보이면 범위 비교보다 우선합니다.
 * - 선택된 인덱스 하나로 row offset 후보를 모은 뒤,
 *   나머지 조건은 executor에서 다시 검사합니다.
 */
int try_collect_offsets_from_indexes(const AppConfig *config,
                                     const TableSchema *schema,
                                     const SelectStatement *statement,
                                     long offsets[SQLPROC_MAX_INDEX_RESULTS],
                                     int *offset_count,
                                     int *used_index,
                                     ErrorInfo *error)
{
    DIR *directory;
    struct dirent *entry;
    char chosen_path[INDEX_MAX_PATH_LEN];
    int chosen_predicate;
    int prefer_equality;

    *offset_count = 0;
    *used_index = 0;
    chosen_path[0] = '\0';
    chosen_predicate = -1;
    prefer_equality = 0;

    if (statement->where_clause.count == 0) {
        return 1;
    }

    directory = opendir(config->index_dir);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return 1;
        }

        set_file_error(error, "인덱스 디렉터리를 열 수 없습니다.");
        return 0;
    }

    while ((entry = readdir(directory)) != NULL) {
        int predicate_index;

        if (strstr(entry->d_name, ".idx") == NULL) {
            continue;
        }

        for (predicate_index = 0; predicate_index < statement->where_clause.count; predicate_index++) {
            char path[INDEX_MAX_PATH_LEN];
            FILE *file;
            IndexHeader header;

            if (chosen_predicate >= 0 && prefer_equality) {
                continue;
            }

            snprintf(path, sizeof(path), "%s/%s", config->index_dir, entry->d_name);
            if (!open_matching_index(path,
                                     schema->table_name,
                                     statement->where_clause.items[predicate_index].column_name,
                                     &file,
                                     &header,
                                     error)) {
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

    if (chosen_predicate < 0) {
        return 1;
    }

    {
        FILE *file;
        IndexHeader header;

        file = fopen(chosen_path, "rb");
        if (file == NULL) {
            set_file_error(error, "선택한 인덱스 파일을 열 수 없습니다.");
            return 0;
        }

        if (!read_header(file, &header, error)) {
            fclose(file);
            return 0;
        }

        if (!header_matches_current_schema(&header, schema)) {
            fclose(file);
            set_file_error(error, "인덱스와 현재 스키마가 맞지 않습니다.");
            return 0;
        }

        if (!collect_offsets_from_index(file,
                                        &header,
                                        &statement->where_clause.items[chosen_predicate],
                                        offsets,
                                        offset_count,
                                        error)) {
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
