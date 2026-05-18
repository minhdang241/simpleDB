/*
 *  chidb - a didactic relational database management system
 *
 *  Database Machine operations.
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

#include "chidb/chidb.h"
#include "chisql/chisql.h"
#include "dbm.h"
#include "btree.h"
#include "record.h"
#include <stdint.h>

/* Function pointer for dispatch table */
typedef int (*handler_function)(chidb_stmt *stmt, chidb_dbm_op_t *op);

/* Single entry in the instruction dispatch table */
struct handler_entry
{
    opcode_t opcode;
    handler_function func;
};

/* This generates all the instruction handler prototypes. It expands to:
 *
 * int chidb_dbm_op_OpenRead(chidb_stmt *stmt, chidb_dbm_op_t *op);
 * int chidb_dbm_op_OpenWrite(chidb_stmt *stmt, chidb_dbm_op_t *op);
 * ...
 * int chidb_dbm_op_Halt(chidb_stmt *stmt, chidb_dbm_op_t *op);
 */
#define HANDLER_PROTOTYPE(OP) int chidb_dbm_op_## OP (chidb_stmt *stmt, chidb_dbm_op_t *op);
FOREACH_OP(HANDLER_PROTOTYPE)


/* Ladies and gentlemen, the dispatch table. */
#define HANDLER_ENTRY(OP) { Op_ ## OP, chidb_dbm_op_## OP},

struct handler_entry dbm_handlers[] =
{
    FOREACH_OP(HANDLER_ENTRY)
};

int chidb_dbm_op_handle (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    return dbm_handlers[op->opcode].func(stmt, op);
}


/*** INSTRUCTION HANDLER IMPLEMENTATIONS ***/


int chidb_dbm_op_Noop (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    return CHIDB_OK;
}

static int open_cursor(chidb_stmt *stmt, chidb_dbm_op_t *op,
                       chidb_dbm_cursor_type_t type) {
    int32_t cursor_index = op->p1;
    if (cursor_index < 0) return CHIDB_EMISMATCH;

    int32_t reg_number = op->p2;
    if (!EXISTS_REGISTER(stmt, reg_number) ||
        stmt->reg[reg_number].type != REG_INT32) {
        return CHIDB_EMISMATCH;
    }
    npage_t root_page = (npage_t)stmt->reg[reg_number].value.i;

    if ((uint32_t)cursor_index >= stmt->nCursors) {
        uint32_t new_size = cursor_index + 1;
        chidb_dbm_cursor_t *new_cursors =
            realloc(stmt->cursors, sizeof(chidb_dbm_cursor_t) * new_size);
        if (!new_cursors) return CHIDB_ENOMEM;
        for (uint32_t i = stmt->nCursors; i < new_size; i++) {
            new_cursors[i].type = CURSOR_UNSPECIFIED;
        }
        stmt->cursors = new_cursors;
        stmt->nCursors = new_size;
    }
    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    cur->type = type;
    cur->tree = stmt->db->bt;
    cur->root_page = root_page;
    cur->top = -1;
    return CHIDB_OK;
}

int chidb_dbm_op_OpenRead(chidb_stmt *stmt, chidb_dbm_op_t *op) {
    return open_cursor(stmt, op, CURSOR_READ);
}

int chidb_dbm_op_OpenWrite (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    return open_cursor(stmt, op, CURSOR_WRITE);
}


int chidb_dbm_op_Close (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */
    int32_t cursor_index = op->p1;
    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    cur->type = CURSOR_UNSPECIFIED;
    cur->top = -1;
    return CHIDB_OK;
}


int chidb_dbm_op_Rewind (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    err = cursor_rewind(cur);
    if (err) return err;
    BTreeCell cell;
    if (cursor_get_cell(cur, &cell) == CHIDB_DONE) {
        stmt->pc = jump_addr;
    }

    return CHIDB_OK;
}


int chidb_dbm_op_Next (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    err = cursor_next(cur);
    if (err == CHIDB_DONE) return CHIDB_OK;
    if (err) return err;
    stmt->pc = jump_addr;
    return CHIDB_OK;
}


int chidb_dbm_op_Prev (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    err = cursor_prev(cur);
    if (err == CHIDB_DONE) return CHIDB_OK;
    if (err) return err;
    stmt->pc = jump_addr;
    return CHIDB_OK;
}


int chidb_dbm_op_Seek (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    int32_t reg_number = op->p3;

    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (!EXISTS_REGISTER(stmt, reg_number) ||
        stmt->reg[reg_number].type != REG_INT32)
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    chidb_key_t key = (chidb_key_t)stmt->reg[reg_number].value.i;

    err = cursor_seek(cur, key);
    if (err == CHIDB_ENOTFOUND) {
        stmt->pc = jump_addr;
        return CHIDB_OK;
    }
    return err; /* CHIDB_OK on hit, or a real error */
}


int chidb_dbm_op_SeekGt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    int32_t reg_number = op->p3;

    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (!EXISTS_REGISTER(stmt, reg_number) ||
        stmt->reg[reg_number].type != REG_INT32)
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    chidb_key_t target = (chidb_key_t)stmt->reg[reg_number].value.i;

    err = cursor_seek_gt(cur, target);
    if (err == CHIDB_ENOTFOUND) {
        stmt->pc = jump_addr;
        return CHIDB_OK;
    }
    return err;
}


int chidb_dbm_op_SeekGe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    int32_t reg_number = op->p3;

    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (!EXISTS_REGISTER(stmt, reg_number) ||
        stmt->reg[reg_number].type != REG_INT32)
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    chidb_key_t target = (chidb_key_t)stmt->reg[reg_number].value.i;

    err = cursor_seek_ge(cur, target);
    if (err == CHIDB_ENOTFOUND) {
        stmt->pc = jump_addr;
        return CHIDB_OK;
    }
    return err;
}

int chidb_dbm_op_SeekLt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    int32_t reg_number = op->p3;

    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (!EXISTS_REGISTER(stmt, reg_number) ||
        stmt->reg[reg_number].type != REG_INT32)
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    chidb_key_t target = (chidb_key_t)stmt->reg[reg_number].value.i;

    err = cursor_seek_lt(cur, target);
    if (err == CHIDB_ENOTFOUND) {
        stmt->pc = jump_addr;
        return CHIDB_OK;
    }
    return err;
}


int chidb_dbm_op_SeekLe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t jump_addr = op->p2;
    int32_t reg_number = op->p3;

    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (!EXISTS_REGISTER(stmt, reg_number) ||
        stmt->reg[reg_number].type != REG_INT32)
        return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    chidb_key_t target = (chidb_key_t)stmt->reg[reg_number].value.i;

    err = cursor_seek_le(cur, target);
    if (err == CHIDB_ENOTFOUND) {
        stmt->pc = jump_addr;
        return CHIDB_OK;
    }
    return err;
}

int chidb_dbm_op_Column (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int32_t cursor_index = op->p1;
    int32_t column_index = op->p2;
    int32_t reg_number = op->p3;
    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (column_index < 0 || reg_number < 0) return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    BTreeCell cell;
    err = cursor_get_cell(cur, &cell);
    if (err) return err;
    if (cell.type != PGTYPE_TABLE_LEAF) return CHIDB_EMISMATCH;
    DBRecord *r;
    err = chidb_DBRecord_unpack(&r, cell.fields.tableLeaf.data);
    if (err) return err;
    if (column_index >= r->nfields) {
        chidb_DBRecord_destroy(r);
        return CHIDB_EMISMATCH;
    }
    if ((uint32_t)reg_number >= stmt->nReg) {
        err = chidb_stmt_set_reg(stmt, reg_number + 1, REG_UNSPECIFIED);
        if (err) {
            chidb_DBRecord_destroy(r);
            return err;
        }
    }
    chidb_dbm_register_t *dst = &stmt->reg[reg_number];
    int type = chidb_DBRecord_getType(r, column_index);
    if (type == SQL_NULL) {
        dst->type = REG_NULL;
    } else if (type == SQL_INTEGER_1BYTE) {
        int8_t v;
        chidb_DBRecord_getInt8(r, column_index, &v);
        dst->type = REG_INT32;
        dst->value.i = v;
    } else if (type == SQL_INTEGER_2BYTE) {
        int16_t v;
        chidb_DBRecord_getInt16(r, column_index, &v);
        dst->type = REG_INT32;
        dst->value.i = v;
    } else if (type == SQL_INTEGER_4BYTE) {
        int32_t v;
        chidb_DBRecord_getInt32(r, column_index, &v);
        dst->type = REG_INT32;
        dst->value.i = v;
    } else if (type >= SQL_TEXT) {
        char *s;
        err = chidb_DBRecord_getString(r, column_index, &s);
        if (err) {
            chidb_DBRecord_destroy(r);
            return err;
        }
        dst->type = REG_STRING;
        dst->value.s = s; /* getString
malloc'd it */
    } else {
        chidb_DBRecord_destroy(r);
        return CHIDB_EMISMATCH;
    }
    chidb_DBRecord_destroy(r);
    return CHIDB_OK;
}


int chidb_dbm_op_Key (chidb_stmt *stmt, chidb_dbm_op_t *op)
{

    int err;
    int32_t cursor_index = op->p1;
    int32_t reg_number = op->p2;
    if (cursor_index < 0 || !EXISTS_CURSOR(stmt, cursor_index))
        return CHIDB_EMISMATCH;
    if (reg_number < 0) return CHIDB_EMISMATCH;

    chidb_dbm_cursor_t *cur = &stmt->cursors[cursor_index];
    BTreeCell cell;
    err = cursor_get_cell(cur, &cell);
    if (err) return err;
    if ((uint32_t)reg_number >= stmt->nReg) {
        err = chidb_stmt_set_reg(stmt, reg_number + 1, REG_UNSPECIFIED);
        if (err) {
            return err;
        }
    }
    chidb_dbm_register_t *dst = &stmt->reg[reg_number];
    dst->type = REG_INT32;
    dst->value.i = (int32_t) cell.key;
    return CHIDB_OK;
}


int chidb_dbm_op_Integer (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int reg_number = op->p2;
    int val = op->p1;
    int size = stmt->nReg;
    if (reg_number >= size) {
        size = reg_number + 1;
    }
    err = chidb_stmt_set_reg(stmt, size, REG_INT32);
    if (err) {
        return err;
    }
    stmt->reg[reg_number].type = REG_INT32;
    stmt->reg[reg_number].value.i = val;
    return CHIDB_OK;
}


int chidb_dbm_op_String (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int reg_number = op->p2;
    int len = op->p1;
    char *s = op->p4;
    int size = stmt->nReg;
    if (reg_number >= size) {
        size = reg_number + 1;
    }
    err = chidb_stmt_set_reg(stmt, size, REG_STRING);
    if (err) {
        return err;
    }
    stmt->reg[reg_number].type = REG_STRING;
    stmt->reg[reg_number].value.s = malloc(sizeof(char) * len);
    memcpy(stmt->reg[reg_number].value.s, s, len);
    return CHIDB_OK;
}


int chidb_dbm_op_Null (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int err;
    int reg_number = op->p2;
    int size = stmt->nReg;
    if (reg_number >= size) {
        size = reg_number + 1;
    }
    err = chidb_stmt_set_reg(stmt, size, REG_NULL);
    if (err) {
        return err;
    }
    stmt->reg[reg_number].type = REG_NULL;
    return CHIDB_OK;
}


int chidb_dbm_op_ResultRow (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int32_t start_reg = op->p1;
    int32_t ncols = op->p2;
    if (start_reg < 0 || ncols < 0) return CHIDB_EMISMATCH;
    if (ncols > 0 && !EXISTS_REGISTER(stmt, start_reg + ncols - 1))
        return CHIDB_EMISMATCH;
    stmt->startRR = (uint32_t)start_reg;
    stmt->nRR = (uint32_t)ncols;
    return CHIDB_ROW;
}


int chidb_dbm_op_MakeRecord (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */

    return CHIDB_OK;
}


int chidb_dbm_op_Insert (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */

    return CHIDB_OK;
}


int chidb_dbm_op_Eq (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int32_t r1 = op->p1;
    int32_t addr = op->p2;
    int32_t r2 = op->p3;
    register_type_t type = stmt->reg[r1].type;
    if (type == stmt->reg[r2].type) {

        switch (type) {
        case REG_INT32:
            if (stmt->reg[r1].value.i == stmt->reg[r2].value.i) {
                stmt->pc = addr;
            }
            break;
        case REG_STRING:
            if (strcmp(stmt->reg[r1].value.s, stmt->reg[r2].value.s) == 0) {
                stmt->pc = addr;
            }
            break;
        case REG_BINARY:
            /* TODO: Check value. Currently not supported by DMB file format. */
            break;
        case REG_UNSPECIFIED:
        case REG_NULL:
            /* Nothing to do, since we've already checked that the types of the
             * registers match */
            break;
        }
    }
    return CHIDB_OK;
}

int chidb_dbm_op_Ne(chidb_stmt *stmt, chidb_dbm_op_t *op) {

    int32_t r1 = op->p1;
    int32_t addr = op->p2;
    int32_t r2 = op->p3;
    register_type_t type = stmt->reg[r1].type;
    if (type == stmt->reg[r2].type) {

        switch (type) {
        case REG_INT32:
            if (stmt->reg[r1].value.i != stmt->reg[r2].value.i) {
                stmt->pc = addr;
            }
            break;
        case REG_STRING:
            if (strcmp(stmt->reg[r1].value.s, stmt->reg[r2].value.s) != 0) {
                stmt->pc = addr;
            }
            break;
        case REG_BINARY:
            /* TODO: Check value. Currently not supported by DMB file format. */
            break;
        case REG_UNSPECIFIED:
        case REG_NULL:
            /* Nothing to do, since we've already checked that the types of the
             * registers match */
            break;
        }
    } else {
        stmt->pc = addr;
    }
    return CHIDB_OK;
}

int chidb_dbm_op_Lt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */
    int32_t r1 = op->p1;
    int32_t addr = op->p2;
    int32_t r2 = op->p3;
    register_type_t type = stmt->reg[r1].type;
    if (type == stmt->reg[r2].type) {

        switch (type) {
        case REG_INT32:
            if (stmt->reg[r1].value.i > stmt->reg[r2].value.i) {
                stmt->pc = addr;
            }
            break;
        case REG_STRING:
            if (strcmp(stmt->reg[r1].value.s, stmt->reg[r2].value.s) > 0) {
                stmt->pc = addr;
            }
            break;
        case REG_BINARY:
            /* TODO: Check value. Currently not supported by DMB file format. */
            break;
        case REG_UNSPECIFIED:
        case REG_NULL:
            /* Nothing to do, since we've already checked that the types of the
             * registers match */
            break;
        }
    }
    return CHIDB_OK;
}


int chidb_dbm_op_Le (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    int32_t r1 = op->p1;
    int32_t addr = op->p2;
    int32_t r2 = op->p3;
    register_type_t type = stmt->reg[r1].type;
    if (type == stmt->reg[r2].type) {

        switch (type) {
        case REG_INT32:
            if (stmt->reg[r1].value.i >= stmt->reg[r2].value.i) {
                stmt->pc = addr;
            }
            break;
        case REG_STRING:
            if (strcmp(stmt->reg[r1].value.s, stmt->reg[r2].value.s) >= 0) {
                stmt->pc = addr;
            }
            break;
        case REG_BINARY:
            /* TODO: Check value. Currently not supported by DMB file format. */
            break;
        case REG_UNSPECIFIED:
        case REG_NULL:
            /* Nothing to do, since we've already checked that the types of the
             * registers match */
            break;
        }
    }

    return CHIDB_OK;
}


int chidb_dbm_op_Gt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */
    /* Your code goes here */
    int32_t r1 = op->p1;
    int32_t addr = op->p2;
    int32_t r2 = op->p3;
    register_type_t type = stmt->reg[r1].type;
    if (type == stmt->reg[r2].type) {

        switch (type) {
        case REG_INT32:
            if (stmt->reg[r1].value.i < stmt->reg[r2].value.i) {
                stmt->pc = addr;
            }
            break;
        case REG_STRING:
            if (strcmp(stmt->reg[r1].value.s, stmt->reg[r2].value.s) < 0) {
                stmt->pc = addr;
            }
            break;
        case REG_BINARY:
            /* TODO: Check value. Currently not supported by DMB file format. */
            break;
        case REG_UNSPECIFIED:
        case REG_NULL:
            /* Nothing to do, since we've already checked that the types of the
             * registers match */
            break;
        }
    }

    return CHIDB_OK;
}


int chidb_dbm_op_Ge (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */
    /* Your code goes here */
    int32_t r1 = op->p1;
    int32_t addr = op->p2;
    int32_t r2 = op->p3;
    register_type_t type = stmt->reg[r1].type;
    if (type == stmt->reg[r2].type) {

        switch (type) {
        case REG_INT32:
            if (stmt->reg[r1].value.i <= stmt->reg[r2].value.i) {
                stmt->pc = addr;
            }
            break;
        case REG_STRING:
            if (strcmp(stmt->reg[r1].value.s, stmt->reg[r2].value.s) <= 0) {
                stmt->pc = addr;
            }
            break;
        case REG_BINARY:
            /* TODO: Check value. Currently not supported by DMB file format. */
            break;
        case REG_UNSPECIFIED:
        case REG_NULL:
            /* Nothing to do, since we've already checked that the types of the
             * registers match */
            break;
        }
    }

    return CHIDB_OK;
}


/* IdxGt p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 * 
 * if (idxkey at cursor p1) > k, jump
 */
int chidb_dbm_op_IdxGt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
  fprintf(stderr,"todo: chidb_dbm_op_IdxGt\n");
  exit(1);
}

/* IdxGe p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 * 
 * if (idxkey at cursor p1) >= k, jump
 */
int chidb_dbm_op_IdxGe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
  fprintf(stderr,"todo: chidb_dbm_op_IdxGe\n");
  exit(1);
}

/* IdxLt p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 * 
 * if (idxkey at cursor p1) < k, jump
 */
int chidb_dbm_op_IdxLt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
  fprintf(stderr,"todo: chidb_dbm_op_IdxLt\n");
  exit(1);
}

/* IdxLe p1 p2 p3 *
 *
 * p1: cursor
 * p2: jump addr
 * p3: register containing value k
 * 
 * if (idxkey at cursor p1) <= k, jump
 */
int chidb_dbm_op_IdxLe (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
  fprintf(stderr,"todo: chidb_dbm_op_IdxLe\n");
  exit(1);
}


/* IdxPKey p1 p2 * *
 *
 * p1: cursor
 * p2: register
 *
 * store pkey from (cell at cursor p1) in (register at p2)
 */
int chidb_dbm_op_IdxPKey (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
  fprintf(stderr,"todo: chidb_dbm_op_IdxKey\n");
  exit(1);
}

/* IdxInsert p1 p2 p3 *
 *
 * p1: cursor
 * p2: register containing IdxKey
 * p2: register containing PKey
 *
 * add new (IdkKey,PKey) entry in index BTree pointed at by cursor at p1
 */
int chidb_dbm_op_IdxInsert (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
  fprintf(stderr,"todo: chidb_dbm_op_IdxInsert\n");
  exit(1);
}


int chidb_dbm_op_CreateTable (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */

    return CHIDB_OK;
}


int chidb_dbm_op_CreateIndex (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */

    return CHIDB_OK;
}


int chidb_dbm_op_Copy (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */

    return CHIDB_OK;
}


int chidb_dbm_op_SCopy (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */

    return CHIDB_OK;
}


int chidb_dbm_op_Halt (chidb_stmt *stmt, chidb_dbm_op_t *op)
{
    /* Your code goes here */
    stmt->endOp = 0;
    return CHIDB_OK;
}

