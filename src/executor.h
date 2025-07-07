#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h"

// Executes a CREATE TABLE query.
void executeCreate(const Query& q);

// Executes an INSERT INTO query.
void executeInsert(const Query& q);

// Executes a SELECT query.
void executeSelect(const Query& q);

// Replays all log entries to rebuild database state on startup.
void replayLogs();

#endif  // EXECUTOR_H
