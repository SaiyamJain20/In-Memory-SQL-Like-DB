#ifndef DATABASE_H
#define DATABASE_H

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
using namespace std;

// Represents a column in a table.
class Column {
public:
    string name;
    string type;  // e.g., "INT", "STRING", "FLOAT"

    Column(string name, string type);
};

// Represents a single row in a table.
class Row {
public:
    vector<string> values;  // Storing all values as string for simplicity

    Row(vector<string> values);
};

// Represents an in-memory table.
class Table {
public:
    string name;
    vector<Column> columns;
    vector<Row> rows;
    mutex tableMutex;  // Lock for concurrency control on table operations

    Table();
    Table(string name, vector<Column> columns);

    // Disable copy constructor and assignment
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // Allow move constructor and assignment
    Table(Table&&) noexcept;
    Table& operator=(Table&&) noexcept;

    void insertRow(const Row& row);  // Adds a row to the table.
    void displayRows();              // Prints all rows in the table.
    void displayColumns();           // Prints column names.
};

// Global in-memory database map (table name → Table object)
extern unordered_map<string, Table> database;

#endif  // DATABASE_H
