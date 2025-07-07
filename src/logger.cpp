#include "logger.h"
#include <fstream>
#include <iostream>

// Appends a query string to log.txt, followed by a newline.
void writeLog(const string& logEntry) {
    ofstream logfile("log.txt", ios::app);
    if (!logfile) {
        cerr << "Error: Unable to open log file for writing.\n";
        return;
    }
    logfile << logEntry << endl;
    logfile.close();
}

// Reads log.txt line by line and returns a vector of queries.
vector<string> readLogs() {
    vector<string> logs;
    ifstream logfile("log.txt");
    if (!logfile) {
        cerr << "Warning: Log file not found, starting fresh.\n";
        return logs;
    }

    string line;
    while (getline(logfile, line)) {
        if (!line.empty())
            logs.push_back(line);
    }

    logfile.close();
    return logs;
}

// Clears the log file by opening it in truncation mode.
void clearLogs() {
    ofstream logfile("log.txt", ios::trunc);
    if (!logfile) {
        cerr << "Error: Unable to clear log file.\n";
        return;
    }
    logfile.close();
}
