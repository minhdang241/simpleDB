simpleDB - A didactic RDBMS inspired from childb (http://chi.cs.uchicago.edu/)
=====================================

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