#pragma once

#include "storage/table.h"

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace cdb {

// Name -> Table registry. Names are case-insensitive (stored lower-cased, displayed as created).
// Thread-safe.
class Catalog {
  public:
    // Throws Error(Catalog) if the name exists, unless `if_not_exists` (then returns the existing
    // table). Throws Error(Binder) for an invalid schema (empty, duplicate column names).
    std::shared_ptr<Table> CreateTable(const std::string& name,
                                       std::vector<ColumnDefinition> schema,
                                       bool if_not_exists = false,
                                       idx_t row_group_size = kRowGroupSize);

    // Throws Error(Catalog) if the table does not exist.
    std::shared_ptr<Table> GetTable(const std::string& name) const;
    std::shared_ptr<Table> TryGetTable(const std::string& name) const;

    // Throws Error(Catalog) if the table does not exist, unless `if_exists`. Scans already
    // holding a snapshot keep working.
    void DropTable(const std::string& name, bool if_exists = false);

    // Display names, sorted case-insensitively.
    std::vector<std::string> ListTables() const;

  private:
    static std::string Key(const std::string& name);

    mutable std::shared_mutex mutex_;
    std::map<std::string, std::shared_ptr<Table>> tables_;
};

} // namespace cdb
