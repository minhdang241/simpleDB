/*
 *  chidb - a didactic relational database management system
 *
 * This module contains functions to manipulate a B-Tree file. In this context,
 * "BTree" refers not to a single B-Tree but to a "file of B-Trees" ("chidb
 * file" and "file of B-Trees" are essentially equivalent terms).
 *
 * However, this module does *not* read or write to the database file directly.
 * All read/write operations must be done through the pager module.
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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <chidb/log.h>
#include <unistd.h>
#include "chidb/chidb.h"
#include "chidbInt.h"
#include "btree.h"
#include "record.h"
#include "pager.h"
#include "util.h"

#define HEADER_SIZE 100

static bool node_is_full(BTreeNode *btn, BTreeCell *btc) {
    uint16_t cell_size;
    switch (btc->type) {
    case PGTYPE_TABLE_LEAF:
        cell_size =
            TABLELEAFCELL_SIZE_WITHOUTDATA + btc->fields.tableLeaf.data_size;
        break;
    case PGTYPE_TABLE_INTERNAL:
        cell_size = TABLEINTCELL_SIZE;
        break;
    case PGTYPE_INDEX_LEAF:
        cell_size = INDEXLEAFCELL_SIZE;
        break;
    case PGTYPE_INDEX_INTERNAL:
        cell_size = INDEXINTCELL_SIZE;
        break;
    default:
        return true;
    }
    /* +2 for the new offset-array entry */
    return (btn->cells_offset - btn->free_offset) < cell_size + 2;
}

/* Open a B-Tree file
 *
 * This function opens a database file and verifies that the file
 * header is correct. If the file is empty (which will happen
 * if the pager is given a filename for a file that does not exist)
 * then this function will (1) initialize the file header using
 * the default page size and (2) create an empty table leaf node
 * in page 1.
 *
 * Parameters
 * - filename: Database file (might not exist)
 * - db: A chidb struct. Its bt field must be set to the newly
 *       created BTree.
 * - bt: An out parameter. Used to return a pointer to the
 *       newly created BTree.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ECORRUPTHEADER: Database file contains an invalid header
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_open(const char *filename, chidb *db, BTree **bt)
{
    int err;
    uint8_t header[HEADER_SIZE];
    BTree *tree = NULL;
    Pager *pager = NULL;

    err = chidb_Pager_open(&pager, filename);
    if (err) return err;

    err = chidb_Pager_readHeader(pager, header);

    // Explicit handle unexpected erros during read
    if (err != CHIDB_OK && err != CHIDB_NOHEADER) {
        chidb_Pager_close(pager);
        return err;
    }

    tree = (BTree *)malloc(sizeof(BTree));
    if (!tree) {
        err = CHIDB_ENOMEM;
        goto cleanup_pager;
    }

    tree->db = db;
    tree->pager = pager;

    if (err == CHIDB_NOHEADER) {
        err = chidb_Pager_setPageSize(pager, DEFAULT_PAGE_SIZE);
        if (err) goto cleanup_tree;

        npage_t npage;
        err = chidb_Pager_allocatePage(pager, &npage);
        if (err) goto cleanup_tree;

        err = chidb_Btree_initEmptyNode(tree, npage, PGTYPE_TABLE_LEAF);
        if (err) goto cleanup_tree;

        BTreeNode *node;
        err = chidb_Btree_getNodeByPage(tree, pager->n_pages, &node);
        if (err) goto cleanup_tree;

        memcpy(node->page->data, "SQLite format 3\0", 16);
        put2byte(&(node->page->data[16]), pager->page_size);
        put4byte(&(node->page->data[24]), 0);     // file change counter
        put4byte(&(node->page->data[40]), 0);     // file change number
        put4byte(&(node->page->data[48]), 20000); // page cache size
        put4byte(&(node->page->data[60]), 0);     // user cookies

        err = chidb_Btree_writeNode(tree, node);
        chidb_Btree_freeMemNode(tree, node);
        if (err) goto cleanup_tree;

    } else {
        if (memcmp(header, "SQLite format 3\0", 16) != 0) {
            err = CHIDB_ECORRUPTHEADER;
            goto cleanup_tree;
        }

        uint16_t pagesize = get2byte(&(header[16]));
        err = chidb_Pager_setPageSize(pager, pagesize);
        if (err) goto cleanup_tree;

        if (header[18] != 1 || header[19] != 1 || header[20] != 0 ||
            header[21] != 64 || header[22] != 32 || header[23] != 32 ||
            get4byte(&header[48]) != 20000 || get4byte(&header[52]) != 0 ||
            get4byte(&header[56]) != 1 || get4byte(&header[60]) != 0) {
            err = CHIDB_ECORRUPTHEADER;
            goto cleanup_tree;
        }
    }
    *bt = tree;
    db->bt = tree;
    return CHIDB_OK;

// Centralized error cleanup
cleanup_tree:
    free(tree);
cleanup_pager:
    chidb_Pager_close(pager);
    return err;
}


/* Close a B-Tree file
 *
 * This function closes a database file, freeing any resource
 * used in memory, such as the pager.
 *
 * Parameters
 * - bt: B-Tree file to close
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_close(BTree *bt)
{
    /* Your code goes here */
    chidb_Pager_close(bt->pager);
    free(bt);
    return CHIDB_OK;
}


/* Loads a B-Tree node from disk
 *
 * Reads a B-Tree node from a page in the disk. All the information regarding
 * the node is stored in a BTreeNode struct (see header file for more details
 * on this struct). *This is the only function that can allocate memory for
 * a BTreeNode struct*. Always use chidb_Btree_freeMemNode to free the memory
 * allocated for a BTreeNode (do not use free() directly on a BTreeNode variable)
 * Any changes made to a BTreeNode variable will not be effective in the database
 * until chidb_Btree_writeNode is called on that BTreeNode.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage: Page of node to load
 * - btn: Out parameter. Used to return a pointer to newly creater BTreeNode
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EPAGENO: The provided page number is not valid
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_getNodeByPage(BTree *bt, npage_t npage, BTreeNode **btn)
{
    /* Your code goes here */
    int err;
    BTreeNode *node;

    // Allocate node
    node = (BTreeNode *)malloc(sizeof(BTreeNode));
    if (!node) {
      return CHIDB_ENOMEM;
    }

    // Read page
    err = chidb_Pager_readPage(bt->pager, npage, &(node->page));
    if (err) {
      free(node);
      return err;
    }

    // Initialize other fields
    uint8_t *data = node->page->data;
    // Page 1 is special since the first 100bytes are used to store file header
    int header = (npage == 1) ? HEADER_SIZE : 0;

    node->type = data[header + PGHEADER_PGTYPE_OFFSET];
    node->free_offset = get2byte(&data[header + PGHEADER_FREE_OFFSET]);
    node->n_cells = get2byte(&data[header + PGHEADER_NCELLS_OFFSET]);
    node->cells_offset = get2byte(&data[header + PGHEADER_CELL_OFFSET]);

    if (node->type == PGTYPE_TABLE_INTERNAL ||
        node->type == PGTYPE_INDEX_INTERNAL) {
        // + 4 since the right page takes 4 bytes
        node->right_page = get4byte(&data[header + PGHEADER_RIGHTPG_OFFSET]);
        node->celloffset_array = &data[header + PGHEADER_RIGHTPG_OFFSET + 4];
    } else {
        node->right_page = 0; // Safely zero it out
        node->celloffset_array =
            &data[header +
                  PGHEADER_RIGHTPG_OFFSET]; // Starts where right_page would be
    }

    // Update the output node
    *btn = node;
    return CHIDB_OK;
}


/* Frees the memory allocated to an in-memory B-Tree node
 *
 * Frees the memory allocated to an in-memory B-Tree node, and
 * the in-memory page returned by the pages (stored in the
 * "page" field of BTreeNode)
 *
 * Parameters
 * - bt: B-Tree file
 * - btn: BTreeNode to free
 *
 * Return
 * - CHIDB_OK: Operation successful
 */
int chidb_Btree_freeMemNode(BTree *bt, BTreeNode *btn)
{
    /* Your code goes here */
    chidb_Pager_releaseMemPage(bt->pager, btn->page);
    free(btn);
    return CHIDB_OK;
}


/* Create a new B-Tree node
 *
 * Allocates a new page in the file and initializes it as a B-Tree node.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage: Out parameter. Returns the number of the page that
 *          was allocated.
 * - type: Type of B-Tree node (PGTYPE_TABLE_INTERNAL, PGTYPE_TABLE_LEAF,
 *         PGTYPE_INDEX_INTERNAL, or PGTYPE_INDEX_LEAF)
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_newNode(BTree *bt, npage_t *npage, uint8_t type)
{
    /* Your code goes here */
    int err;
    err = chidb_Pager_allocatePage(bt->pager, npage);
    if (err) {
        chilog(DEBUG, "Failed to allocatePage %d", err);
        return err;
    }

    err = chidb_Btree_initEmptyNode(bt, *npage, type);
    if (err) {
        chilog(DEBUG, "Failed to initEmptyNode %d", err);
        return err;
    }
    return CHIDB_OK;
}


/* Initialize a B-Tree node
 *
 * Initializes a database page to contain an empty B-Tree node. The
 * database page is assumed to exist and to have been already allocated
 * by the pager.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage: Database page where the node will be created.
 * - type: Type of B-Tree node (PGTYPE_TABLE_INTERNAL, PGTYPE_TABLE_LEAF,
 *         PGTYPE_INDEX_INTERNAL, or PGTYPE_INDEX_LEAF)
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_initEmptyNode(BTree *bt, npage_t npage, uint8_t type)
{
    assert(npage <= bt->pager->n_pages);
    int err;
    BTreeNode *node;

    /* Allocate and initialize empty node */
    node = (BTreeNode *)malloc(sizeof(BTreeNode));
    if (!node) {
        return CHIDB_ENOMEM;
    }
    err = chidb_Pager_readPage(bt->pager, npage, &node->page);
    if (err) {
        chilog(DEBUG, "Failed to readPage %d", err);
        free(node);
        return CHIDB_EIO;
    }
    int header = (npage == 1) ? HEADER_SIZE : 0;
    node->type = type;
    node->free_offset = header + ((type == PGTYPE_TABLE_INTERNAL || type == PGTYPE_INDEX_INTERNAL)
                                  ? INTPG_CELLSOFFSET_OFFSET : LEAFPG_CELLSOFFSET_OFFSET);
    node->n_cells = 0;
    node->cells_offset = bt->pager->page_size;
    node->right_page = 0;
    node->celloffset_array = &(node->page->data[node->free_offset]);

    /* Write to page */
    err = chidb_Btree_writeNode(bt, node);
    if (err) {
        chilog(DEBUG, "Failed to writeNode %d", err);
        chidb_Btree_freeMemNode(bt, node);
        return CHIDB_EIO;
    }
    chidb_Btree_freeMemNode(bt, node);
    return CHIDB_OK;
}



/* Write an in-memory B-Tree node to disk
 *
 * Writes an in-memory B-Tree node to disk. To do this, we need to update
 * the in-memory page according to the chidb page format. Since the cell
 * offset array and the cells themselves are modified directly on the
 * page, the only thing to do is to store the values of "type",
 * "free_offset", "n_cells", "cells_offset" and "right_page" in the
 * in-memory page.
 *
 * Parameters
 * - bt: B-Tree file
 * - btn: BTreeNode to write to disk
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_writeNode(BTree *bt, BTreeNode *btn)
{
    int err;
    uint8_t *data;

    /* Update the in-memory page */
    int header = (btn->page->npage == 1) ? HEADER_SIZE : 0;
    data = btn->page->data;
    data[header + PGHEADER_PGTYPE_OFFSET] = btn->type;
    put2byte(&data[header + PGHEADER_FREE_OFFSET], btn->free_offset);
    put2byte(&data[header + PGHEADER_NCELLS_OFFSET], btn->n_cells);
    put2byte(&data[header + PGHEADER_CELL_OFFSET], btn->cells_offset);
    if (btn->type == PGTYPE_TABLE_INTERNAL || btn->type == PGTYPE_INDEX_INTERNAL)
        put4byte(&data[header + PGHEADER_RIGHTPG_OFFSET], btn->right_page);

    err = chidb_Pager_writePage(bt->pager, btn->page);
    if (err) {
        chilog(DEBUG, "Failed to write page with error %d", err);
        return CHIDB_EIO;
    }
    return CHIDB_OK;
}


/* Read the contents of a cell
 *
 * Reads the contents of a cell from a BTreeNode and stores them in a BTreeCell.
 * This involves the following:
 *  1. Find out the offset of the requested cell.
 *  2. Read the cell from the in-memory page, and parse its
 *     contents (refer to The chidb File Format document for
 *     the format of cells).
 *
 * Parameters
 * - btn: BTreeNode where cell is contained
 * - ncell: Cell number
 * - cell: BTreeCell where contents must be stored.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ECELLNO: The provided cell number is invalid
 */
int chidb_Btree_getCell(BTreeNode *btn, ncell_t ncell, BTreeCell *cell)
{
    int err = 0;
    if (ncell >= btn->n_cells) return CHIDB_ECELLNO;
    uint16_t offset = get2byte(&(btn->celloffset_array[2 * ncell]));
    uint8_t *data = &(btn->page->data[offset]);
    cell->type = btn->type;

    switch (btn->type) {
    case PGTYPE_TABLE_LEAF: {
        err = getVarint32(data + TABLELEAFCELL_SIZE_OFFSET,
                          &cell->fields.tableLeaf.data_size);
        if (err) return err;

        err = getVarint32(data + TABLELEAFCELL_KEY_OFFSET,
                          (uint32_t *)&cell->key);
        if (err) return err;

        cell->fields.tableLeaf.data =
            (uint8_t *)malloc(cell->fields.tableLeaf.data_size);

        if (!cell->fields.tableLeaf.data) {
            return CHIDB_ENOMEM;
        }
        memcpy(cell->fields.tableLeaf.data, data + TABLELEAFCELL_DATA_OFFSET,
               cell->fields.tableLeaf.data_size);
        break;
    }
    case PGTYPE_TABLE_INTERNAL: {
        cell->fields.tableInternal.child_page =
            get4byte(data + TABLEINTCELL_CHILD_OFFSET);
        err =
            getVarint32(data + TABLEINTCELL_KEY_OFFSET, (uint32_t *)&cell->key);
        if (err) return err;
        break;
    }
    case PGTYPE_INDEX_LEAF: {
        cell->key = get4byte(data + INDEXLEAFCELL_KEYIDX_OFFSET);
        cell->fields.indexLeaf.keyPk =
            get4byte(data + INDEXLEAFCELL_KEYPK_OFFSET);
        break;
    }
    case PGTYPE_INDEX_INTERNAL: {
        cell->fields.indexInternal.child_page =
            get4byte(data + INDEXINTCELL_CHILD_OFFSET);

        cell->key = get4byte(data + INDEXINTCELL_KEYIDX_OFFSET);
        cell->fields.indexInternal.keyPk =
            get4byte(data + INDEXINTCELL_KEYPK_OFFSET);
        break;
    }
    default:
        break;
    }
    return CHIDB_OK;
}

/* Insert a new cell into a B-Tree node
 *
 * Inserts a new cell into a B-Tree node at a specified position ncell.
 * This involves the following:
 *  1. Add the cell at the top of the cell area. This involves "translating"
 *     the BTreeCell into the chidb format (refer to The chidb File Format
 *     document for the format of cells).
 *  2. Modify cells_offset in BTreeNode to reflect the growth in the cell area.
 *  3. Modify the cell offset array so that all values in positions >= ncell
 *     are shifted one position forward in the array. Then, set the value of
 *     position ncell to be the offset of the newly added cell.
 *
 * This function assumes that there is enough space for this cell in this node.
 *
 * Parameters
 * - btn: BTreeNode to insert cell in
 * - ncell: Cell number
 * - cell: BTreeCell to insert.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ECELLNO: The provided cell number is invalid
 */
int chidb_Btree_insertCell(BTreeNode *btn, ncell_t ncell, BTreeCell *cell)
{
    if (ncell > btn->n_cells) {
        return CHIDB_ECELLNO;
    }

    /* Translate BTreeCell into chidb format */
    uint16_t cell_size;
    switch (cell->type) {
    case PGTYPE_TABLE_LEAF: {
        cell_size =
            TABLELEAFCELL_SIZE_WITHOUTDATA + cell->fields.tableLeaf.data_size;
        btn->cells_offset -= cell_size;
        uint8_t *dst = &btn->page->data[btn->cells_offset];
        putVarint32(dst + TABLELEAFCELL_SIZE_OFFSET,
                    cell->fields.tableLeaf.data_size);
        putVarint32(dst + TABLELEAFCELL_KEY_OFFSET, cell->key);
        memcpy(dst + TABLELEAFCELL_DATA_OFFSET, cell->fields.tableLeaf.data,
               cell->fields.tableLeaf.data_size);
        break;
    }
    case PGTYPE_TABLE_INTERNAL: {
        cell_size = TABLEINTCELL_SIZE;
        btn->cells_offset -= cell_size;
        uint8_t *dst = &btn->page->data[btn->cells_offset];

        put4byte(dst + TABLEINTCELL_CHILD_OFFSET,
                 cell->fields.tableInternal.child_page);
        putVarint32(dst + TABLEINTCELL_KEY_OFFSET, cell->key);
        break;
    }
    case PGTYPE_INDEX_LEAF: {
        cell_size = INDEXLEAFCELL_SIZE;
        btn->cells_offset -= cell_size;
        uint8_t *dst = &btn->page->data[btn->cells_offset];

        put4byte(dst + INDEXLEAFCELL_KEYIDX_OFFSET, cell->key);
        put4byte(dst + INDEXLEAFCELL_KEYPK_OFFSET,
                 cell->fields.indexLeaf.keyPk);
        break;
    }
    case PGTYPE_INDEX_INTERNAL: {
        cell_size = INDEXINTCELL_SIZE;
        btn->cells_offset -= cell_size;
        uint8_t *dst = &btn->page->data[btn->cells_offset];

        put4byte(dst + INDEXINTCELL_CHILD_OFFSET,
                 cell->fields.indexInternal.child_page);
        put4byte(dst + INDEXINTCELL_KEYIDX_OFFSET, cell->key);
        put4byte(dst + INDEXINTCELL_KEYPK_OFFSET,
                 cell->fields.indexInternal.keyPk);
        break;
    }
    default:
        chilog(DEBUG, "Should not be here with type %d", cell->type);
        return CHIDB_ECELLNO;
    }

    /* Shift all values in positions >= ncell one position forward */
    memmove(&btn->celloffset_array[2 * (ncell + 1)],
            &btn->celloffset_array[2 * ncell], (btn->n_cells - ncell) * 2);
    put2byte(&(btn->celloffset_array[2 * (ncell)]), btn->cells_offset);
    btn->n_cells++;
    btn->free_offset += 2;
    return CHIDB_OK;
}

static void chidb_Btree_freeCell(BTreeCell cell) {
    if (cell.type == PGTYPE_TABLE_LEAF) free(cell.fields.tableLeaf.data);
}
/* Find an entry in a table B-Tree
 *
 * Finds the data associated for a given key in a table B-Tree
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want search in
 * - key: Entry key
 * - data: Out-parameter where a copy of the data must be stored
 * - size: Out-parameter where the number of bytes of data must be stored
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOTFOUND: No entry with the given key way found
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_find(BTree *bt, npage_t nroot, chidb_key_t key, uint8_t **data, uint16_t *size)
{
    int err;
    BTreeNode *root;
    BTreeCell cell;
    err = chidb_Btree_getNodeByPage(bt, nroot, &root);
    if (err) return err;

    while (1) {
        if (root->n_cells == 0) {
            if (root->type == PGTYPE_TABLE_LEAF) {
                err = CHIDB_ENOTFOUND;
                goto cleanup_root;
            }
            npage_t next = root->right_page;
            chidb_Btree_freeMemNode(bt, root);
            err = chidb_Btree_getNodeByPage(bt, next, &root);
            if (err) return err;
            continue;
        }

        ncell_t lo = 0, hi = root->n_cells;
        bool found = false;

        // Binary search
        while (lo < hi) {
            ncell_t mid = lo + (hi - lo) / 2;
            err = chidb_Btree_getCell(root, mid, &cell);
            if (err) {
                chidb_Btree_freeMemNode(bt, root);
                return err;
            }
            if (key < cell.key) {
                hi = mid;
                chidb_Btree_freeCell(cell);
            } else if (key > cell.key) {
                lo = mid + 1;
                chidb_Btree_freeCell(cell);
            } else {
                lo = mid;
                found = true;
                break;
            }
        }

        // Process leaf node
        if (root->type == PGTYPE_TABLE_LEAF) {
            if (!found) {
                err = CHIDB_ENOTFOUND;
                goto cleanup_root;
            }
            *data = malloc(cell.fields.tableLeaf.data_size);
            if (!*data) {
                chidb_Btree_freeCell(cell);
                err = CHIDB_ENOMEM;
                goto cleanup_root;
            }
            memcpy(*data, cell.fields.tableLeaf.data,
                   cell.fields.tableLeaf.data_size);
            *size = cell.fields.tableLeaf.data_size;

            chidb_Btree_freeCell(cell);
            chidb_Btree_freeMemNode(bt, root);
            return CHIDB_OK;
        }

        /* Process internal node, move to child node */
        npage_t next;
        if (lo == root->n_cells) {
            next = root->right_page;
        } else {
            /* If we didn't break early, we need to freshly fetch the cell */
            if (!found) {
                err = chidb_Btree_getCell(root, lo, &cell);
                if (err) goto cleanup_root;
            }
            next = cell.fields.tableInternal.child_page;
            chidb_Btree_freeCell(cell);
        }
        chidb_Btree_freeMemNode(bt, root);
        err = chidb_Btree_getNodeByPage(bt, next, &root);
        if (err) return err;
    }
cleanup_root:
    chidb_Btree_freeMemNode(bt, root);
    return err;
}
/* Insert an entry into a table B-Tree
 *
 * This is a convenience function that wraps around chidb_Btree_insert.
 * It takes a key and data, and creates a BTreeCell that can be passed
 * along to chidb_Btree_insert.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this entry in.
 * - key: Entry key
 * - data: Pointer to data we want to insert
 * - size: Number of bytes of data
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insertInTable(BTree *bt, npage_t nroot, chidb_key_t key, uint8_t *data, uint16_t size)
{
    /* Your code goes here */
    BTreeCell btc;
    btc.type = PGTYPE_TABLE_LEAF;
    btc.key = key;
    btc.fields.tableLeaf.data_size = size;
    btc.fields.tableLeaf.data = data;
    return chidb_Btree_insert(bt, nroot, &btc);
}


/* Insert an entry into an index B-Tree
 *
 * This is a convenience function that wraps around chidb_Btree_insert.
 * It takes a KeyIdx and a KeyPk, and creates a BTreeCell that can be passed
 * along to chidb_Btree_insert.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this entry in.
 * - keyIdx: See The chidb File Format.
 * - keyPk: See The chidb File Format.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insertInIndex(BTree *bt, npage_t nroot, chidb_key_t keyIdx, chidb_key_t keyPk)
{
    BTreeCell btc;
    btc.type = PGTYPE_INDEX_LEAF;
    btc.key = keyIdx;
    btc.fields.indexLeaf.keyPk = keyPk;
    return chidb_Btree_insert(bt, nroot, &btc);
}


/* Insert a BTreeCell into a B-Tree
 *
 * The chidb_Btree_insert and chidb_Btree_insertNonFull functions
 * are responsible for inserting new entries into a B-Tree, although
 * chidb_Btree_insertNonFull is the one that actually does the
 * insertion. chidb_Btree_insert, however, first checks if the root
 * has to be split (a splitting operation that is different from
 * splitting any other node). If so, chidb_Btree_split is called
 * before calling chidb_Btree_insertNonFull.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this cell in.
 * - btc: BTreeCell to insert into B-Tree
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insert(BTree *bt, npage_t nroot, BTreeCell *btc)
{
    /* Your code goes here */
    // TODO: check the need to split first
    int err;
    BTreeNode *root;
    err = chidb_Btree_getNodeByPage(bt, nroot, &root);
    if (err) return err;
    bool full = node_is_full(root, btc);
    uint8_t old_type = root->type;
    if (full) {
        /* Allocate m2 */
        npage_t npage_m2;
        BTreeNode *m2;
        BTreeCell cell;
        err = chidb_Btree_newNode(bt, &npage_m2, old_type);
        if (err) {
            chidb_Btree_freeMemNode(bt, root);
            return err;
        }
        uint8_t new_root_type =
            (old_type == PGTYPE_INDEX_LEAF || old_type == PGTYPE_INDEX_INTERNAL)
                ? PGTYPE_INDEX_INTERNAL
                : PGTYPE_TABLE_INTERNAL;
        /* Copy value from root to m2 */
        err = chidb_Btree_getNodeByPage(bt, npage_m2, &m2);
        if (err) {
            chidb_Btree_freeMemNode(bt, root);
            return err;
        }
        m2->right_page = root->right_page;
        for (int i = 0; i < root->n_cells; i++) {
            err = chidb_Btree_getCell(root, i, &cell);
            if (err) {
                chidb_Btree_freeMemNode(bt, root);
                chidb_Btree_freeMemNode(bt, m2);
                return err;
            }
            err = chidb_Btree_insertCell(m2, i, &cell);
            if (err) {
                chidb_Btree_freeCell(cell);
                chidb_Btree_freeMemNode(bt, root);
                chidb_Btree_freeMemNode(bt, m2);
                return err;
            }
            chidb_Btree_freeCell(cell);
        }

        chidb_Btree_freeMemNode(bt, root);
        err = chidb_Btree_writeNode(bt, m2);
        if (err) {
            chidb_Btree_freeMemNode(bt, m2);
            return err;
        }
        chidb_Btree_freeMemNode(bt, m2);

        /* re-init root as internal */
        err = chidb_Btree_initEmptyNode(bt, nroot, new_root_type);
        if (err) return err;

        err = chidb_Btree_getNodeByPage(bt, nroot, &root);
        if (err) return err;

        root->right_page = npage_m2;

        err = chidb_Btree_writeNode(bt, root);
        if (err) {
            chidb_Btree_freeMemNode(bt, root);
            return err;
        }
        chidb_Btree_freeMemNode(bt, root);

        /* Split */
        npage_t npage_new;
        err = chidb_Btree_split(bt, nroot, npage_m2, 0, &npage_new);
        if (err) return err;
        return chidb_Btree_insertNonFull(bt, nroot, btc);
    }
    chidb_Btree_freeMemNode(bt, root);
    return chidb_Btree_insertNonFull(bt, nroot, btc);
}

/* Insert a BTreeCell into a non-full B-Tree node
 *
 * chidb_Btree_insertNonFull inserts a BTreeCell into a node that is
 * assumed not to be full (i.e., does not require splitting). If the
 * node is a leaf node, the cell is directly added in the appropriate
 * position according to its key. If the node is an internal node, the
 * function will determine what child node it must insert it in, and
 * calls itself recursively on that child node. However, before doing so
 * it will check if the child node is full or not. If it is, then it will
 * have to be split first.
 *
 * Parameters
 * - bt: B-Tree file
 * - nroot: Page number of the root node of the B-Tree we want to insert
 *          this cell in.
 * - btc: BTreeCell to insert into B-Tree
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_EDUPLICATE: An entry with that key already exists
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_insertNonFull(BTree *bt, npage_t npage, BTreeCell *btc)
{
    /* Your code goes here */
    int err;
    BTreeNode *root;
    BTreeCell cell;
    err = chidb_Btree_getNodeByPage(bt, npage, &root);
    if (err) {
        return err;
    }

    if (root->type == PGTYPE_TABLE_LEAF || root->type == PGTYPE_INDEX_LEAF) {
        ncell_t lo = 0, hi = root->n_cells;
        bool found = false;
        while (lo < hi) {
            ncell_t mid = lo + (hi - lo) / 2;
            err = chidb_Btree_getCell(root, mid, &cell);
            if (err) {
                chidb_Btree_freeMemNode(bt, root);
                return err;
            }
            if (btc->key < cell.key) {
                hi = mid;
                chidb_Btree_freeCell(cell);
            } else if (btc->key > cell.key) {
                lo = mid + 1;
                chidb_Btree_freeCell(cell);
            } else {
                lo = mid;
                found = true;
                break;
            }
        }
        if (found) {
            chidb_Btree_freeMemNode(bt, root);
            chidb_Btree_freeCell(cell);
            return CHIDB_EDUPLICATE;
        }

        err = chidb_Btree_insertCell(root, lo, btc);
        if (err) goto cleanup_root;

        err = chidb_Btree_writeNode(bt, root);
        if (err) goto cleanup_root;

        chidb_Btree_freeMemNode(bt, root);
        return CHIDB_OK;
    } else {
        npage_t next;
        BTreeNode *child;
        ncell_t lo = 0, hi = root->n_cells;
        bool found = false;

        while (lo < hi) {
            ncell_t mid = lo + (hi - lo) / 2;
            err = chidb_Btree_getCell(root, mid, &cell);
            if (err) {
                chidb_Btree_freeMemNode(bt, root);
                return err;
            }
            if (btc->key < cell.key) {
                hi = mid;
                chidb_Btree_freeCell(cell);
            } else if (btc->key > cell.key) {
                lo = mid + 1;
                chidb_Btree_freeCell(cell);
            } else {
                lo = mid;
                found = true;
                break;
            }
        }

        if (found && root->type == PGTYPE_INDEX_INTERNAL) {
            chidb_Btree_freeCell(cell);
            chidb_Btree_freeMemNode(bt, root);
            return CHIDB_EDUPLICATE;
        }

        /* Determine which child page to descend into */
        if (lo == root->n_cells) {
            next = root->right_page;
        } else {
            if (!found) {
                err = chidb_Btree_getCell(root, lo, &cell);
                if (err) goto cleanup_root;
            }
            if (root->type == PGTYPE_TABLE_INTERNAL)
                next = cell.fields.tableInternal.child_page;
            else
                next = cell.fields.indexInternal.child_page;
            chidb_Btree_freeCell(cell);
        }
        err = chidb_Btree_getNodeByPage(bt, next, &child);
        if (err) goto cleanup_root;
        if (node_is_full(child, btc)) {
            chidb_Btree_freeMemNode(bt, child);
            chidb_Btree_freeMemNode(bt, root);
            npage_t npage_new;
            err = chidb_Btree_split(bt, npage, next, lo, &npage_new);
            if (err) return err;
            return chidb_Btree_insertNonFull(bt, npage, btc);
        }
        chidb_Btree_freeMemNode(bt, child);
        chidb_Btree_freeMemNode(bt, root);
        return chidb_Btree_insertNonFull(bt, next, btc);
    }
cleanup_root:
    chidb_Btree_freeMemNode(bt, root);
    return err;
}


/* Split a B-Tree node
 *
 * Splits a B-Tree node N. This involves the following:
 * - Find the median cell in N.
 * - Create a new B-Tree node M.
 * - Move the cells before the median cell to M (if the
 *   cell is a table leaf cell, the median cell is moved too)
 * - Add a cell to the parent (which, by definition, will be an
 *   internal page) with the median key and the page number of M.
 *
 * Parameters
 * - bt: B-Tree file
 * - npage_parent: Page number of the parent node
 * - npage_child: Page number of the node to split
 * - parent_ncell: Position in the parent where the new cell will
 *                 be inserted.
 * - npage_child2: Out parameter. Used to return the page of the new child node.
 *
 * Return
 * - CHIDB_OK: Operation successful
 * - CHIDB_ENOMEM: Could not allocate memory
 * - CHIDB_EIO: An I/O error has occurred when accessing the file
 */
int chidb_Btree_split(BTree *bt, npage_t npage_parent, npage_t npage_child, ncell_t parent_ncell, npage_t *npage_child2)
{
    int err;
    BTreeNode *parent;
    BTreeNode *child;
    BTreeNode *new;
    npage_t npage_new;
    ncell_t median;
    BTreeCell cell;
    BTreeCell median_cell;

    /* Get the child node*/
    err = chidb_Btree_getNodeByPage(bt, npage_child, &child);
    if (err) {
        return err;
    }

    /* Create new node */
    err = chidb_Btree_newNode(bt, &npage_new, child->type);
    if (err) goto cleanup_child;

    err = chidb_Btree_getNodeByPage(bt, npage_new, &new);
    if (err) goto cleanup_child;

    median = child->n_cells / 2;
    ncell_t limit = (child->type == PGTYPE_TABLE_LEAF) ? median + 1 : median;
    for (int i = 0; i < limit; i++) {
        err = chidb_Btree_getCell(child, i, &cell);
        if (err) goto cleanup_new;

        err = chidb_Btree_insertCell(new, i, &cell);
        if (err) {
            chidb_Btree_freeCell(cell);
            goto cleanup_new;
        }
        chidb_Btree_freeCell(cell);
    }

    /* Update the parent node */
    err = chidb_Btree_getCell(child, median, &median_cell);
    if (err) goto cleanup_new;

    /* Capture right-half cells */
    ncell_t n_right = child->n_cells - (median + 1);
    BTreeCell *right_cells = NULL;
    if (n_right > 0) {
        right_cells = malloc(sizeof(BTreeCell) * n_right);
        if (!right_cells) {
            chidb_Btree_freeCell(median_cell);
            err = CHIDB_ENOMEM;
            goto cleanup_new;
        }
        for (ncell_t i = 0; i < n_right; i++) {
            err = chidb_Btree_getCell(child, median + 1 + i, &right_cells[i]);
            if (err) {
                for (ncell_t j = 0; j < i; j++)
                    chidb_Btree_freeCell(right_cells[j]);
                free(right_cells);
                chidb_Btree_freeCell(median_cell);
                goto cleanup_new;
            }
        }
    }

    if (child->type == PGTYPE_TABLE_INTERNAL) {
        new->right_page = median_cell.fields.tableInternal.child_page;
    } else if (child->type == PGTYPE_INDEX_INTERNAL) {
        new->right_page = median_cell.fields.indexInternal.child_page;
    }

    npage_t right_page = child->right_page;

    err = chidb_Btree_getNodeByPage(bt, npage_parent, &parent);
    if (err) {
        for (ncell_t i = 0; i < n_right; i++)
            chidb_Btree_freeCell(right_cells[i]);
        free(right_cells);
        chidb_Btree_freeCell(median_cell);
        goto cleanup_new;
    }

    cell.type = parent->type;
    cell.key = median_cell.key;
    if (cell.type == PGTYPE_TABLE_INTERNAL) {
        cell.fields.tableInternal.child_page = npage_new;
    } else {
        cell.fields.indexInternal.child_page = npage_new;
        cell.fields.indexInternal.keyPk =
            median_cell.type == PGTYPE_INDEX_INTERNAL
                ? median_cell.fields.indexInternal.keyPk
                : median_cell.fields.indexLeaf.keyPk;
    }

    err = chidb_Btree_insertCell(parent, parent_ncell, &cell);
    if (err) {
        for (ncell_t i = 0; i < n_right; i++)
            chidb_Btree_freeCell(right_cells[i]);
        free(right_cells);
        chidb_Btree_freeCell(median_cell);
        goto cleanup_parent;
    }
    chidb_Btree_freeCell(median_cell);
    chidb_Btree_freeCell(cell);

    /* Rebuild child */
    err = chidb_Btree_initEmptyNode(bt, npage_child, child->type);
    if (err) {
        for (ncell_t i = 0; i < n_right; i++)
            chidb_Btree_freeCell(right_cells[i]);
        free(right_cells);
        goto cleanup_parent;
    }

    chidb_Btree_freeMemNode(bt, child);
    err = chidb_Btree_getNodeByPage(bt, npage_child, &child);

    if (err) {
        for (ncell_t i = 0; i < n_right; i++)
            chidb_Btree_freeCell(right_cells[i]);
        free(right_cells);
        child = NULL;
        goto cleanup_parent;
    }

    for (int i = 0; i < n_right; i++) {
        err = chidb_Btree_insertCell(child, i, &right_cells[i]);
        chidb_Btree_freeCell(right_cells[i]);
        if (err) {
            for (ncell_t j = i + 1; j < n_right; j++)
                chidb_Btree_freeCell(right_cells[j]);
            free(right_cells);
            goto cleanup_parent;
        }
    }
    free(right_cells);
    child->right_page = right_page;

    /* Persist all three modified nodes */
    err = chidb_Btree_writeNode(bt, new);
    if (err) goto cleanup_parent;
    err = chidb_Btree_writeNode(bt, child);
    if (err) goto cleanup_parent;
    err = chidb_Btree_writeNode(bt, parent);
    if (err) goto cleanup_parent;

    *npage_child2 = npage_new;
    chidb_Btree_freeMemNode(bt, parent);
    chidb_Btree_freeMemNode(bt, new);
    chidb_Btree_freeMemNode(bt, child);
    return CHIDB_OK;

cleanup_parent:
    chidb_Btree_freeMemNode(bt, parent);
cleanup_new:
    chidb_Btree_freeMemNode(bt, new);
cleanup_child:
    if (child) chidb_Btree_freeMemNode(bt, child);
    return err;
}

