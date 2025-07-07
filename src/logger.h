#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <vector>
using namespace std;

// Appends a single log entry (a query string) to log.txt.
void writeLog(const string& logEntry);

// Reads log.txt line by line and returns a vector of query strings.
vector<string> readLogs();

// Clears (empties) the existing log file. (useful for resets)
void clearLogs();

#endif  // LOGGER_H
