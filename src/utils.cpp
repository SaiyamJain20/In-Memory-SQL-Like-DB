#include "utils.h"
#include <sstream>
#include <cctype>
#include <algorithm>

// Splits a string by the given delimiter.
vector<string> split(const string& str, char delimiter) {
    vector<string> tokens;
    string token;
    istringstream tokenStream(str);
    
    while (getline(tokenStream, token, delimiter)) {
        tokens.push_back(trim(token));  // remove surrounding whitespace
    }

    return tokens;
}

// Trims leading and trailing whitespace from a string.
string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");

    if (start == string::npos || end == string::npos)
        return "";  // all spaces

    return s.substr(start, end - start + 1);
}

// Checks if a string represents a valid integer or float.
bool isNumber(const string& s) {
    if (s.empty())
        return false;

    char* endptr = nullptr;
    strtod(s.c_str(), &endptr);
    return *endptr == '\0';
}

// Converts a string to uppercase.
string toUpper(const string& s) {
    string result = s;
    transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return toupper(c); });
    return result;
}
