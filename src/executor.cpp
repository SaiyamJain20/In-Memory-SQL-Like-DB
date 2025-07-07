#include "executor.h"
#include "database.h"
#include "logger.h"
#include "utils.h"
#include <iostream>

// Executes a CREATE TABLE operation.
void executeCreate(const Query& q) {
    if (database.find(q.tableName) != database.end()) {
        cerr << "Error: Table already exists.\n";
        return;
    }

    vector<Column> columns;
    for (const string& colDef : q.columns) {
        vector<string> parts = split(colDef, ' ');
        if (parts.size() != 2) {
            cerr << "Error: Invalid column definition: " << colDef << endl;
            return;
        }
        columns.push_back(Column(parts[0], parts[1]));
    }

    Table newTable(q.tableName, columns);
    database.emplace(q.tableName, move(newTable));

    // Create a well-formatted log entry
    string logEntry = "CREATE TABLE " + q.tableName + " (";
    for (size_t i = 0; i < q.columns.size(); i++) {
        logEntry += q.columns[i];
        if (i < q.columns.size() - 1) {
            logEntry += ", ";
        }
    }
    logEntry += ")";
    
    writeLog(logEntry);
    
    cout << "Table '" << q.tableName << "' created successfully.\n";
}

// Executes an INSERT INTO operation.
void executeInsert(const Query& q) {
    auto it = database.find(q.tableName);
    if (it == database.end()) {
        cerr << "Error: Table not found.\n";
        return;
    }

    Table& table = it->second;
    table.insertRow(Row(q.values));

    // Create a well-formatted log entry
    string logEntry = "INSERT INTO " + q.tableName + " VALUES (";
    for (size_t i = 0; i < q.values.size(); i++) {
        logEntry += q.values[i];
        if (i < q.values.size() - 1) {
            logEntry += ", ";
        }
    }
    logEntry += ")";
    
    writeLog(logEntry);

    cout << "Row inserted into '" << q.tableName << "' successfully.\n";
}

// Executes a SELECT operation.
void executeSelect(const Query& q) {
    auto it = database.find(q.tableName);
    if (it == database.end()) {
        cerr << "Error: Table not found.\n";
        return;
    }

    Table& table = it->second;
    lock_guard<mutex> lock(table.tableMutex);

    table.displayColumns();

    for (const Row& row : table.rows) {
        bool match = true;

        if (!q.whereColumn.empty()) {
            // Find the index of the whereColumn
            int colIndex = -1;
            for (size_t i = 0; i < table.columns.size(); ++i) {
                if (table.columns[i].name == q.whereColumn) {
                    colIndex = i;
                    break;
                }
            }
            if (colIndex == -1) {
                cerr << "Error: Column '" << q.whereColumn << "' not found.\n";
                return;
            }

            if (row.values[colIndex] != q.whereValue)
                match = false;
        }

        if (match) {
            for (const string& val : row.values)
                cout << val << " | ";
            cout << endl;
        }
    }
}

// Replays log.txt to reconstruct in-memory database state.
void replayLogs() {
    vector<string> logs = readLogs();
    cout << "Replaying " << logs.size() << " log entries...\n";
    
    for (const string& logEntry : logs) {
        cout << "Replaying: " << logEntry << endl; // Debug line
        Query q = parseQuery(logEntry);
        
        if (q.type == Query::CREATE) {
            auto it = database.find(q.tableName);
            if (it != database.end()) {
                continue;
            }
            
            vector<Column> columns;
            for (const string& colDef : q.columns) {
                vector<string> parts = split(colDef, ' ');
                if (parts.size() != 2) {
                    cerr << "Error in log replay: Invalid column definition: " << colDef << endl;
                    continue;
                }
                columns.push_back(Column(parts[0], parts[1]));
            }

            Table newTable(q.tableName, columns);
            database.emplace(q.tableName, move(newTable));
            cout << "Restored table '" << q.tableName << "'\n";
        }
        else if (q.type == Query::INSERT) {
            auto it = database.find(q.tableName);
            if (it == database.end()) {
                cerr << "Error in log replay: Table not found: " << q.tableName << endl;
                continue;
            }

            Table& table = it->second;
            
            if (q.values.size() == table.columns.size()) {
                table.insertRow(Row(q.values));
                // cout << "Row inserted during replay" << endl;
            } else {
                cerr << "Error: Number of values does not match number of columns.\n";
            }
        }
    }
    
    cout << "Database state restored from logs.\n";
}
