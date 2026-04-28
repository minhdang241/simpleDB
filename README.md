SimpleDB - A didactic RDBMS inspired from childb (http://chi.cs.uchicago.edu/)

strengthen my knowledge about DBMS

## Logical Organization
A chidb file contains:
- file header: 
    - contains metadata.
- 0 or more tables
    - each table is stored as a B+tree. The entries of this tree are **database records**.
    - key for each entry will be its primary key.
- 0 or more indexes
    - stored as B-tree (not B+tree)

## Physical Organization
- A chidb file is divided into pages, numbered from 1. 
- Each page is used to store table or indexes. Page includes:
    - **header**: meta data such as type of page, #cells, etc.
    - **cell offset array**
    - **free space**: store cells:
        - Leaf table cell: <KeyPk, DBRecord>
        - Internal table cell: <Key, ChildPage> where `ChildPage` is the page number containing entries with keys <= `Key`
        - Leaf index cell: <KeyIdx, KeyPk> 
        - Internal index cell: <KeyIdx, KeyPk, ChildPage> where `ChildPage` is the page number containing entries with keys <= `Key`

- Page 1 is special with first 100 bytes are used by the file header.

- All multi-byte integers are in big-endian, so use `get2byte`, `get4byte`, `put2byte`, `put4byte` instead of reading directly from the buffer `pager->data`.

