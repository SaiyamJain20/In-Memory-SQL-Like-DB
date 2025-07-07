#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
using namespace std;

// Splits a string by the given delimiter and returns the list of substrings.
vector<string> split(const string& str, char delimiter);

// Trims leading and trailing whitespace from a string.
string trim(const string& s);

// Checks if a string represents a valid integer or float.
bool isNumber(const string& s);

// Converts a string to uppercase (useful for normalizing SQL keywords).
string toUpper(const string& s);

#endif  // UTILS_H
