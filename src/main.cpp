#include "executor.h"
#include <iostream>

int main() {
    replayLogs();

    string query;
    while (true) {
        cout << "> ";
        getline(cin, query);
        Query q = parseQuery(query);

        switch (q.type) {
            case Query::CREATE: executeCreate(q); break;
            case Query::INSERT: executeInsert(q); break;
            case Query::SELECT: executeSelect(q); break;
            default:
                cout << "Invalid or unsupported query.\n";
        }
    }
}
