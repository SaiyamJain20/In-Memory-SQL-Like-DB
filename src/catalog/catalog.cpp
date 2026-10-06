#include "catalog/catalog.h"

#include "common/error.h"

#include <algorithm>
#include <cctype>

namespace cdb {

std::string Catalog::Key(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

std::shared_ptr<Table> Catalog::CreateTable(const std::string& name,
                                            std::vector<ColumnDefinition> schema,
                                            bool if_not_exists, idx_t row_group_size) {
    if (name.empty()) {
        throw Error(ErrorCode::Catalog, "table name must not be empty");
    }
    const std::string key = Key(name);
    std::unique_lock lock(mutex_);
    auto it = tables_.find(key);
    if (it != tables_.end()) {
        if (if_not_exists) {
            return it->second;
        }
        throw Error(ErrorCode::Catalog, "table \"" + name + "\" already exists");
    }
    auto table = std::make_shared<Table>(name, std::move(schema), row_group_size);
    tables_.emplace(key, table);
    return table;
}

void Catalog::AddTable(std::shared_ptr<Table> table) {
    CDB_CHECK(table != nullptr);
    const std::string key = Key(table->name());
    std::unique_lock lock(mutex_);
    if (!tables_.emplace(key, table).second) {
        throw Error(ErrorCode::Catalog, "table \"" + table->name() + "\" already exists");
    }
}

std::shared_ptr<Table> Catalog::TryGetTable(const std::string& name) const {
    std::shared_lock lock(mutex_);
    auto it = tables_.find(Key(name));
    return it == tables_.end() ? nullptr : it->second;
}

std::shared_ptr<Table> Catalog::GetTable(const std::string& name) const {
    auto table = TryGetTable(name);
    if (!table) {
        throw Error(ErrorCode::Catalog, "table \"" + name + "\" does not exist");
    }
    return table;
}

void Catalog::DropTable(const std::string& name, bool if_exists) {
    std::unique_lock lock(mutex_);
    if (tables_.erase(Key(name)) == 0 && !if_exists) {
        throw Error(ErrorCode::Catalog, "table \"" + name + "\" does not exist");
    }
}

std::vector<std::string> Catalog::ListTables() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& [key, table] : tables_) {
        names.push_back(table->name());
    }
    return names; // std::map iterates in key (lower-cased) order
}

} // namespace cdb
