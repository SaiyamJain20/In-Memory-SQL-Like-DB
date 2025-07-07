# In-Memory SQL-Like Database System

## Overview

A lightweight, multithreaded in-memory database system written in C++17, supporting a SQL-like syntax for basic operations such as `CREATE TABLE`, `INSERT INTO`, and `SELECT`. The system persists data through a simple log-based recovery mechanism, enabling it to reconstruct the in-memory state upon restart.

This project was built to deepen understanding of database internals, concurrency management, and lightweight persistence mechanisms in C++.

## Features

* In-memory data storage for fast, low-latency read/write operations
* SQL-like syntax providing a familiar query interface
* Operation logging with log-based recovery at startup
* Thread-safe table operations using `std::mutex`
* Basic support for `INT` and `STRING` data types

## Technologies Used

* C++17
* Standard Template Library (STL)
* File I/O for logging
* `std::mutex` for concurrency control
* Custom utility functions for string handling

## Architecture

| Component | Description                                                                   |
| :-------- | :---------------------------------------------------------------------------- |
| Parser    | Converts SQL-like query strings into structured `Query` objects               |
| Executor  | Processes `Query` objects and performs table operations                       |
| Database  | Manages in-memory storage of tables, rows, and schema                         |
| Logger    | Appends all operations to a log file (`log.txt`)                              |
| Utils     | Provides helper functions for string trimming, splitting, and case conversion |

## Folder Structure

```
in_memory_database/
├── src/
│   ├── main.cpp
│   ├── database.h/.cpp
│   ├── parser.h/.cpp
│   ├── executor.h/.cpp
│   ├── logger.h/.cpp
│   ├── utils.h/.cpp
├── log.txt
├── Makefile
└── README.md
```

## Building and Running

### Prerequisites

* C++17 compatible compiler (`g++ 9+` or `clang++ 10+`)
* `make`

### Build the Project

```bash
make
```

### Clean the Project

```bash
make clean
```

### Run the Database

```bash
make run
```

or directly:

```bash
./bin/in_memory_db
```

## Usage Examples

### Creating a Table

```
> CREATE TABLE students (id INT, name STRING)
Table 'students' created successfully.
```

### Inserting Data

```
> INSERT INTO students VALUES ('1', 'Saiyam')
Row inserted into 'students' successfully.

> INSERT INTO students VALUES ('2', 'Rohit')
Row inserted into 'students' successfully.
```

### Querying Data

```
> SELECT * FROM students
id | name | 
1 | Saiyam | 
2 | Rohit | 
```

### Using WHERE Clause

```
> SELECT * FROM students WHERE id = '1'
id | name | 
1 | Saiyam | 
```

## Persistence

The database state is recorded in `log.txt` as plain-text SQL-like commands.
On startup, the system replays these logs to reconstruct all tables and data as it existed before shutdown, providing a simple log-based persistence mechanism.

## Limitations

* Only supports `CREATE TABLE`, `INSERT`, and `SELECT` operations
* Limited to `INT` and `STRING` data types
* No support for complex queries such as `UPDATE`, `DELETE`, or multi-table `JOIN`s
* No indexing (linear row scans only)
* In-memory only, with size limited by available RAM
* Simple log-based persistence without transactions or rollback

## Future Enhancements

* Support for `DELETE` and `UPDATE` operations
* Additional data types such as `FLOAT`, `BOOL`, `DATE`
* Implementation of index structures for faster lookups
* Support for query optimization and multi-table `JOIN`s
* Transaction management with ACID properties
* Configurable disk-based storage backend

## Educational Value

This project was built to explore how databases internally handle:

* Query parsing and execution
* In-memory table management
* Concurrent data access
* Log-based recovery systems

It serves as a foundation for understanding real-world relational database concepts and implementation techniques.

## Acknowledgments

* Inspired by relational database architectures such as PostgreSQL and SQLite
* Developed as an educational project to study database systems and concurrency models in C++