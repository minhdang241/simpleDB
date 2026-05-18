/*
 *  chidb - a didactic relational database management system
 *
 *  Database Machine cursors
 *
 */

/*
 *  Copyright (c) 2009-2015, The University of Chicago
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or withsend
 *  modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  - Neither the name of The University of Chicago nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software withsend specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY send OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include "dbm-cursor.h"
#include "chidb/chidb.h"

/* Your code goes here */

static inline chidb_key_t cell_search_key(BTreeCell *cell) {
    return cell->key;
}

int cursor_traverse_leftmost(chidb_dbm_cursor_t *cur, uint32_t start_page_num) {
    uint32_t current_page = start_page_num;
    while (true) {
        BTreeNode *node;
        int err;
        err = chidb_Btree_getNodeByPage(cur->tree, current_page, &node);
        if (err) {
            return err;
        }
        cur->top++;
        if (cur->top >= MAX_CURSOR_DEPTH) {
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_ENOMEM;
        }
        cur->path_stack[cur->top].pagenum = current_page;
        cur->path_stack[cur->top].cell_idx = 0;
        if (node->type == PGTYPE_TABLE_LEAF ||
            node->type == PGTYPE_INDEX_LEAF) {
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_OK;
        }

        // get the left most page
        BTreeCell cell;
        err = chidb_Btree_getCell(node, 0, &cell);
        if (err) {
            chidb_Btree_freeMemNode(cur->tree, node);
            return err;
        }
        current_page = node->type == PGTYPE_TABLE_INTERNAL
                           ? cell.fields.tableInternal.child_page
                           : cell.fields.indexInternal.child_page;
        chidb_Btree_freeMemNode(cur->tree, node);
    }
    return CHIDB_OK;
}

int cursor_traverse_rightmost(chidb_dbm_cursor_t *cur,
                              uint32_t start_page_num) {
    uint32_t current_page = start_page_num;
    while (true) {
        BTreeNode *node;
        int err;
        err = chidb_Btree_getNodeByPage(cur->tree, current_page, &node);
        if (err) {
            return err;
        }
        cur->top++;
        if (cur->top >= MAX_CURSOR_DEPTH) {
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_ENOMEM;
        }
        cur->path_stack[cur->top].pagenum = current_page;
        if (node->type == PGTYPE_TABLE_LEAF ||
            node->type == PGTYPE_INDEX_LEAF) {
            cur->path_stack[cur->top].cell_idx =
                (node->n_cells > 0) ? node->n_cells - 1 : 0;
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_OK;
        }

        // get the right most page
        cur->path_stack[cur->top].cell_idx = node->n_cells;
        current_page = node->right_page;
        chidb_Btree_freeMemNode(cur->tree, node);
    }
    return CHIDB_OK;
}

int cursor_rewind(chidb_dbm_cursor_t *cur) {
    cur->top = -1;
    return cursor_traverse_leftmost(cur, cur->root_page);
}

int cursor_next(chidb_dbm_cursor_t *cur) {
    int err;
    while (cur->top >= 0) {
        CursorFrame *frame = &cur->path_stack[cur->top];
        BTreeNode *node;
        err = chidb_Btree_getNodeByPage(cur->tree, frame->pagenum, &node);
        if (err) {
            return err;
        }

        if (node->type == PGTYPE_TABLE_LEAF ||
            node->type == PGTYPE_INDEX_LEAF) {
            frame->cell_idx++;
            if (frame->cell_idx < node->n_cells) {
                chidb_Btree_freeMemNode(cur->tree, node);
                return CHIDB_OK;
            } else {
                chidb_Btree_freeMemNode(cur->tree, node);
                cur->top--;
                continue;
            }
        } else {
            frame->cell_idx++;
            if (frame->cell_idx <= node->n_cells) {
                npage_t next_branch_page;
                if (frame->cell_idx == node->n_cells) {
                    next_branch_page = node->right_page;
                } else {
                    BTreeCell cell;
                    err = chidb_Btree_getCell(node, frame->cell_idx, &cell);
                    if (err) {
                        chidb_Btree_freeMemNode(cur->tree, node);
                        return err;
                    }
                    next_branch_page =
                        (cell.type == PGTYPE_TABLE_INTERNAL)
                            ? cell.fields.tableInternal.child_page
                            : cell.fields.indexInternal.child_page;
                }
                chidb_Btree_freeMemNode(cur->tree, node);
                return cursor_traverse_leftmost(cur, next_branch_page);
            } else {
                chidb_Btree_freeMemNode(cur->tree, node);
                cur->top--;
                continue;
            }
        }
    }
    return CHIDB_DONE;
}

int cursor_prev(chidb_dbm_cursor_t *cur) {
    int err;
    while (cur->top >= 0) {
        CursorFrame *frame = &cur->path_stack[cur->top];
        BTreeNode *node;
        err = chidb_Btree_getNodeByPage(cur->tree, frame->pagenum, &node);
        if (err) return err;
        if (node->type == PGTYPE_TABLE_LEAF ||
            node->type == PGTYPE_INDEX_LEAF) {
            if (frame->cell_idx > 0) {
                frame->cell_idx--;
                chidb_Btree_freeMemNode(cur->tree, node);
                return CHIDB_OK;
            }
            chidb_Btree_freeMemNode(cur->tree, node);
            cur->top--;
            continue;
        }
        if (frame->cell_idx > 0) {
            frame->cell_idx--;
            BTreeCell cell;
            err = chidb_Btree_getCell(node, frame->cell_idx, &cell);
            if (err) {
                chidb_Btree_freeMemNode(cur->tree, node);
                return err;
            }
            npage_t prev_branch_page =
                (cell.type == PGTYPE_TABLE_INTERNAL)
                    ? cell.fields.tableInternal.child_page
                    : cell.fields.indexInternal.child_page;
            chidb_Btree_freeMemNode(cur->tree, node);
            return cursor_traverse_rightmost(cur, prev_branch_page);
        } else {
            chidb_Btree_freeMemNode(cur->tree, node);
            cur->top--;
            continue;
        }
    }
    return CHIDB_DONE;
}

int cursor_get_cell(chidb_dbm_cursor_t *cur, BTreeCell *out_cell) {
    int err;
    if (cur->top < 0) return CHIDB_DONE;
    CursorFrame *frame = &cur->path_stack[cur->top];
    BTreeNode *node;
    err = chidb_Btree_getNodeByPage(cur->tree, frame->pagenum, &node);
    if (err) {
        return err;
    }
    if (frame->cell_idx >= node->n_cells) {
        chidb_Btree_freeMemNode(cur->tree, node);
        return CHIDB_DONE;
    }
    BTreeCell cell;
    err = chidb_Btree_getCell(node, frame->cell_idx, &cell);
    if (err) {
        chidb_Btree_freeMemNode(cur->tree, node);
        return err;
    }
    chidb_Btree_freeMemNode(cur->tree, node);
    *out_cell = cell;
    return CHIDB_OK;
}

int cursor_seek(chidb_dbm_cursor_t *cur, chidb_key_t key) {
    int err;
    cur->top = -1;
    uint32_t current_page = cur->root_page;
    while (true) {
        BTreeNode *node;
        err = chidb_Btree_getNodeByPage(cur->tree, current_page, &node);
        if (err) return err;
        cur->top++;
        if (cur->top >= MAX_CURSOR_DEPTH) {
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_ENOMEM;
        }
        cur->path_stack[cur->top].pagenum = current_page;
        if (node->type == PGTYPE_TABLE_LEAF) {
            for (ncell_t i = 0; i < node->n_cells; i++) {
                BTreeCell cell;
                err = chidb_Btree_getCell(node, i, &cell);
                if (err) {
                    chidb_Btree_freeMemNode(cur->tree, node);
                    return err;
                }
                if (cell.key == key) {
                    cur->path_stack[cur->top].cell_idx = i;
                    chidb_Btree_freeMemNode(cur->tree, node);
                    return CHIDB_OK;
                }
                if (cell.key > key) break;
            }
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_ENOTFOUND;
        }
        if (node->type == PGTYPE_TABLE_INTERNAL) {
            ncell_t descend_idx = node->n_cells;
            for (ncell_t i = 0; i < node->n_cells; i++) {
                BTreeCell cell;
                err = chidb_Btree_getCell(node, i, &cell);
                if (err) {
                    chidb_Btree_freeMemNode(cur->tree, node);
                    return err;
                }
                if (cell.key >= key) {
                    descend_idx = i;
                    break;
                }
            }
            cur->path_stack[cur->top].cell_idx = descend_idx;
            npage_t next_page;
            if (descend_idx == node->n_cells) {
                next_page = node->right_page;
            } else {
                BTreeCell cell;
                err = chidb_Btree_getCell(node, descend_idx, &cell);
                if (err) {
                    chidb_Btree_freeMemNode(cur->tree, node);
                    return err;
                }
                next_page = cell.fields.tableInternal.child_page;
            }
            chidb_Btree_freeMemNode(cur->tree, node);
            current_page = next_page;
            continue;
        }
        chidb_Btree_freeMemNode(cur->tree, node);
        return CHIDB_ENOTFOUND;
    }
}

typedef enum { SEEK_OP_GT, SEEK_OP_GE } cursor_seek_op_t;

static inline bool seek_matches(chidb_key_t key, chidb_key_t target,
                                cursor_seek_op_t op) {
    return (op == SEEK_OP_GT) ? (key > target) : (key >= target);
}

static int cursor_seek_forward(chidb_dbm_cursor_t *cur, chidb_key_t target,
                               cursor_seek_op_t op) {
    int err;
    cur->top = -1;
    uint32_t current_page = cur->root_page;

    while (true) {
        BTreeNode *node;
        err = chidb_Btree_getNodeByPage(cur->tree, current_page, &node);
        if (err) return err;

        cur->top++;
        if (cur->top >= MAX_CURSOR_DEPTH) {
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_ENOMEM;
        }
        cur->path_stack[cur->top].pagenum = current_page;

        bool is_leaf = (node->type == PGTYPE_TABLE_LEAF ||
                        node->type == PGTYPE_INDEX_LEAF);

        if (is_leaf) {
            for (ncell_t i = 0; i < node->n_cells; i++) {
                BTreeCell cell;
                err = chidb_Btree_getCell(node, i, &cell);
                if (err) {
                    chidb_Btree_freeMemNode(cur->tree, node);
                    return err;
                }
                if (seek_matches(cell_search_key(&cell), target, op)) {
                    cur->path_stack[cur->top].cell_idx = i;
                    chidb_Btree_freeMemNode(cur->tree, node);
                    return CHIDB_OK;
                }
            }
            chidb_Btree_freeMemNode(cur->tree, node);
            return CHIDB_ENOTFOUND;
        }

        ncell_t descend_idx = node->n_cells;
        for (ncell_t i = 0; i < node->n_cells; i++) {
            BTreeCell cell;
            err = chidb_Btree_getCell(node, i, &cell);
            if (err) {
                chidb_Btree_freeMemNode(cur->tree, node);
                return err;
            }
            if (seek_matches(cell_search_key(&cell), target, op)) {
                descend_idx = i;
                break;
            }
        }
        cur->path_stack[cur->top].cell_idx = descend_idx;

        npage_t next_page;
        if (descend_idx == node->n_cells) {
            next_page = node->right_page;
        } else {
            BTreeCell cell;
            err = chidb_Btree_getCell(node, descend_idx, &cell);
            if (err) {
                chidb_Btree_freeMemNode(cur->tree, node);
                return err;
            }
            next_page = (cell.type == PGTYPE_TABLE_INTERNAL)
                ? cell.fields.tableInternal.child_page
                : cell.fields.indexInternal.child_page;
        }
        chidb_Btree_freeMemNode(cur->tree, node);
        current_page = next_page;
    }
}

int cursor_seek_gt(chidb_dbm_cursor_t *cur, chidb_key_t target) {
    return cursor_seek_forward(cur, target, SEEK_OP_GT);
}

int cursor_seek_ge(chidb_dbm_cursor_t *cur, chidb_key_t target) {
    return cursor_seek_forward(cur, target, SEEK_OP_GE);
}

/* SeekLt / SeekLe reduce to "forward seek + step back one".
 *
 * SeekLt(target) = largest key < target  =  (smallest key >= target) - 1
 * SeekLe(target) = largest key <= target =  (smallest key > target)  - 1
 *
 * If the forward seek fails (no key on the right side of target), the answer,
 * if any, is the rightmost key in the tree. */
static int cursor_seek_predecessor(chidb_dbm_cursor_t *cur, chidb_key_t target,
                                   cursor_seek_op_t fwd_op) {
    int err = cursor_seek_forward(cur, target, fwd_op);
    if (err == CHIDB_OK) {
        int rc = cursor_prev(cur);
        return (rc == CHIDB_DONE) ? CHIDB_ENOTFOUND : rc;
    }
    if (err != CHIDB_ENOTFOUND) return err;

    /* Forward seek found nothing => every key < target (or tree is empty).
     * Largest key (if any) is the rightmost leaf cell. */
    cur->top = -1;
    err = cursor_traverse_rightmost(cur, cur->root_page);
    if (err) return err;
    BTreeCell unused;
    int rc = cursor_get_cell(cur, &unused);
    return (rc == CHIDB_DONE) ? CHIDB_ENOTFOUND : rc;
}

int cursor_seek_lt(chidb_dbm_cursor_t *cur, chidb_key_t target) {
    return cursor_seek_predecessor(cur, target, SEEK_OP_GE);
}

int cursor_seek_le(chidb_dbm_cursor_t *cur, chidb_key_t target) {
    return cursor_seek_predecessor(cur, target, SEEK_OP_GT);
}
