#include "parser.h"
#include "utils.h"
#include <iostream>
#include <sstream>
#include <algorithm>

// Default constructor initializes type as INVALID
Query::Query() {
    type = INVALID;
}

// Parses the query string and builds a Query struct.
Query parseQuery(const string& queryString) {
    Query q;
    string query = trim(queryString);
    if (query.empty())
        return q;

    vector<string> tokens = split(query, ' ');
    if (tokens.empty())
        return q;

    string command = toUpper(tokens[0]);

    // Handle CREATE TABLE
    if (command == "CREATE" && tokens.size() >= 3 && toUpper(tokens[1]) == "TABLE") {
        q.type = Query::CREATE;
        q.tableName = tokens[2];

        // Find columns inside brackets
        size_t open = query.find('(');
        size_t close = query.find_last_of(')');

        if (open != string::npos && close != string::npos) {
            string colsStr = query.substr(open + 1, close - open - 1);
            q.columns = split(colsStr, ',');
            // Trim each column definition
            for (auto& col : q.columns) {
                col = trim(col);
            }
        }
    }
    // Handle INSERT INTO
    else if (command == "INSERT" && tokens.size() >= 4 && toUpper(tokens[1]) == "INTO") {
        q.type = Query::INSERT;
        q.tableName = tokens[2];

        size_t valuesPos = query.find("VALUES");
        if (valuesPos == string::npos) {
            valuesPos = query.find("values");  // Try lowercase
        }
        
        if (valuesPos != string::npos) {
            size_t open = query.find('(', valuesPos);
            size_t close = query.find_last_of(')');
            if (open != string::npos && close != string::npos) {
                string valsStr = query.substr(open + 1, close - open - 1);
                
                // Parse values with proper quote handling
                vector<string> values;
                string current;
                bool inQuotes = false;
                
                for (size_t i = 0; i < valsStr.length(); i++) {
                    char c = valsStr[i];
                    
                    if (c == '\'') {
                        inQuotes = !inQuotes;
                    } else if (c == ',' && !inQuotes) {
                        values.push_back(trim(current));
                        current.clear();
                    } else {
                        current += c;
                    }
                }
                
                if (!current.empty()) {
                    values.push_back(trim(current));
                }
                
                // Clean quotes from values
                for (auto& val : values) {
                    if (val.length() >= 2 && val[0] == '\'' && val[val.length()-1] == '\'') {
                        val = val.substr(1, val.length() - 2);
                    }
                }
                
                q.values = values;
            }
        }
    }
    // Handle SELECT
    else if (command == "SELECT") {
        q.type = Query::SELECT;

        // Find the FROM keyword and extract table name
        bool fromFound = false;
        string tableName;
        
        for (size_t i = 1; i < tokens.size() - 1; i++) {
            if (toUpper(tokens[i]) == "FROM" && i + 1 < tokens.size()) {
                fromFound = true;
                tableName = tokens[i + 1];
                
                // Remove quotes if present
                if (tableName.length() >= 2 && 
                    (tableName[0] == '\'' || tableName[0] == '\"') && 
                    tableName[0] == tableName[tableName.length()-1]) {
                    tableName = tableName.substr(1, tableName.length() - 2);
                }
                
                q.tableName = tableName;
                break;
            }
        }
        
        if (!fromFound || q.tableName.empty()) {
            cout << "WARNING: Could not find valid FROM clause in: " << query << endl;
            q.type = Query::INVALID;
            return q;
        }

        // Check for WHERE clause
        for (size_t i = 0; i < tokens.size() - 2; i++) {
            if (toUpper(tokens[i]) == "WHERE" && i + 2 < tokens.size()) {
                q.whereColumn = tokens[i + 1];
                
                // Assuming format is WHERE col = val
                if (i + 3 < tokens.size() && tokens[i + 2] == "=") {
                    q.whereValue = tokens[i + 3];
                    
                    // Remove quotes if present
                    if (q.whereValue.length() >= 2 && 
                        (q.whereValue[0] == '\'' || q.whereValue[0] == '\"') && 
                        q.whereValue[0] == q.whereValue[q.whereValue.length()-1]) {
                        q.whereValue = q.whereValue.substr(1, q.whereValue.length() - 2);
                    }
                }
                break;
            }
        }
    }
    else {
        q.type = Query::INVALID;
    }

    return q;
}