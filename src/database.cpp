#include "database.h"

// --------- Column Implementation ---------

Column::Column(string name, string type) {
    this->name = name;
    this->type = type;
}

// --------- Row Implementation ---------

Row::Row(vector<string> values) {
    this->values = values;
}

// --------- Table Implementation ---------

Table::Table() {}

Table::Table(string name, vector<Column> columns) {
    this->name = name;
    this->columns = columns;
}

void Table::insertRow(const Row& row) {
    lock_guard<mutex> lock(tableMutex);
    
    if (row.values.size() != columns.size()) {
        cerr << "Error: Number of values (" << row.values.size() 
             << ") does not match number of columns (" << columns.size() << ").\n";
        return;
    }
    
    rows.push_back(row);
}

void Table::displayRows() {
    lock_guard<mutex> lock(tableMutex);  // Lock while reading
    displayColumns();
    for (const auto& row : rows) {
        for (const auto& value : row.values) {
            cout << value << " | ";
        }
        cout << endl;
    }
}

void Table::displayColumns() {
    for (const auto& col : columns) {
        cout << col.name << " | ";
    }
    cout << endl;
}

Table::Table(Table&& other) noexcept {
    name = move(other.name);
    columns = move(other.columns);
    rows = move(other.rows);
    // mutex cannot be moved, so leave it default constructed
}

Table& Table::operator=(Table&& other) noexcept {
    if (this != &other) {
        name = move(other.name);
        columns = move(other.columns);
        rows = move(other.rows);
        // mutex remains default constructed
    }
    return *this;
}


// --------- Global database map definition ---------

unordered_map<string, Table> database;
