#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>
using namespace std;

// Represents a parsed query with its components.
struct Query {
    enum Type { CREATE, INSERT, SELECT, INVALID };
    Type type;
    string tableName;
    vector<string> columns;
    vector<string> values;
    string whereColumn;
    string whereValue;

    Query();  // default constructor
};

// Parses a SQL-like query string and returns a Query object.
Query parseQuery(const string& queryString);

#endif  // PARSER_H
