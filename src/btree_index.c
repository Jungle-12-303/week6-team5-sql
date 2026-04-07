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

static void set_file_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

static void build_index_path(char *dest,
                             size_t dest_size,
                             const char *index_dir,
                             const char *index_name)
{
    snprintf(dest, dest_size, "%s/%s.idx", index_dir, index_name);
}

static long node_offset(int node_id)
{
    return (long)sizeof(IndexHeader) + ((long)node_id * (long)sizeof(BTreeNode));
}

static int parse_int_text(const char *text, long *value)
{
    char *end_pointer;

    if (text[0] == '\0') {
        return 0;
    }

    *value = strtol(text, &end_pointer, 10);
    return *end_pointer == '\0';
}

static int compare_keys(int data_type, const char *left, const char *right)
{
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
 * - 임시 키/자식 배열에 새 항목을 삽입한 뒤 가운데 키를 떼어 올리고,
 *   나머지를 좌우 노드에 나눠 다시 씁니다.
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
    right_node->child_ids[right_node->key_count] = temp_children[total_keys + 1];
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
 * - 부모가 비어 있으면 새 루트를 만들고,
 *   부모가 꽉 차면 내부 노드 분할을 반복해 위로 전파합니다.
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

    return 1;
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

    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    column_index = -1;
    for (int i = 0; i < schema.column_count; i++) {
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

    return append_existing_rows(config, &schema, &header, path, error);
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

int update_all_indexes_for_row(const AppConfig *config,
                               const TableSchema *schema,
                               char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                               long row_offset,
                               ErrorInfo *error)
{
    DIR *directory;
    struct dirent *entry;

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
        file = fopen(path, "rb+");
        if (file == NULL) {
            continue;
        }

        if (!read_header(file, &header, error)) {
            fclose(file);
            closedir(directory);
            return 0;
        }

        if (strcmp(header.table_name, schema->table_name) != 0) {
            fclose(file);
            continue;
        }

        if (header.column_index < 0 || header.column_index >= schema->column_count) {
            fclose(file);
            continue;
        }

        if (header.column_type == DATA_TYPE_INT &&
            row_values[header.column_index][0] == '\0') {
            fclose(file);
            continue;
        }

        if (!insert_entry(file, &header, row_values[header.column_index], row_offset, error)) {
            fclose(file);
            closedir(directory);
            return 0;
        }

        fclose(file);
    }

    closedir(directory);
    return 1;
}

static int find_leftmost_leaf(FILE *file,
                              const IndexHeader *header,
                              int *leaf_id,
                              ErrorInfo *error)
{
    int node_id;

    node_id = header->root_node_id;
    while (1) {
        BTreeNode node;

        if (!read_node(file, node_id, &node, error)) {
            return 0;
        }

        if (node.is_leaf) {
            *leaf_id = node_id;
            return 1;
        }

        node_id = node.child_ids[0];
    }
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
 * - `=`와 `>=`/`>`는 해당 leaf부터 오른쪽으로 훑고,
 *   `<`와 `<=`는 가장 왼쪽 leaf부터 시작해 범위를 벗어나면 멈춥니다.
 */
static int collect_offsets_from_index(FILE *file,
                                      const IndexHeader *header,
                                      const Predicate *predicate,
                                      long offsets[SQLPROC_MAX_INDEX_RESULTS],
                                      int *offset_count,
                                      ErrorInfo *error)
{
    int leaf_id;
    int path[INDEX_MAX_PATH_DEPTH];
    int path_length;

    *offset_count = 0;

    if (predicate->operator_type == COMPARE_LESS ||
        predicate->operator_type == COMPARE_LESS_EQUAL) {
        if (!find_leftmost_leaf(file, header, &leaf_id, error)) {
            return 0;
        }
    } else {
        if (!find_leaf_node(file,
                            header,
                            predicate->value.text,
                            path,
                            &path_length,
                            &leaf_id,
                            error)) {
            return 0;
        }
    }

    while (leaf_id != -1) {
        BTreeNode leaf;
        int i;

        if (!read_node(file, leaf_id, &leaf, error)) {
            return 0;
        }

        for (i = 0; i < leaf.key_count; i++) {
            int compare_result;

            compare_result = compare_keys(header->column_type,
                                          leaf.keys[i],
                                          predicate->value.text);

            if (predicate->operator_type == COMPARE_EQUAL && compare_result != 0) {
                if (*offset_count > 0 && compare_result > 0) {
                    return 1;
                }
                continue;
            }

            if (predicate->operator_type == COMPARE_LESS && compare_result >= 0) {
                return 1;
            }

            if (predicate->operator_type == COMPARE_LESS_EQUAL && compare_result > 0) {
                return 1;
            }

            if (predicate->operator_type == COMPARE_GREATER && compare_result <= 0) {
                continue;
            }

            if (predicate->operator_type == COMPARE_GREATER_EQUAL && compare_result < 0) {
                continue;
            }

            if (!append_offset(offsets, offset_count, leaf.row_offsets[i], error)) {
                return 0;
            }
        }

        if (predicate->operator_type == COMPARE_EQUAL && *offset_count > 0) {
            int last_compare;

            last_compare = compare_keys(header->column_type,
                                        leaf.keys[leaf.key_count - 1],
                                        predicate->value.text);
            if (last_compare > 0) {
                return 1;
            }
        }

        if (predicate->operator_type == COMPARE_LESS ||
            predicate->operator_type == COMPARE_LESS_EQUAL) {
            leaf_id = leaf.next_leaf_id;
        } else {
            leaf_id = leaf.next_leaf_id;
        }
    }

    return 1;
}

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

        if (!collect_offsets_from_index(file,
                                        &header,
                                        &statement->where_clause.items[chosen_predicate],
                                        offsets,
                                        offset_count,
                                        error)) {
            fclose(file);
            return 0;
        }

        fclose(file);
    }

    *used_index = 1;
    return 1;
}
