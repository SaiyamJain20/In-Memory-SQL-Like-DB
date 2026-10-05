#pragma once

#include "catalog/catalog.h"

namespace cdb {

// An in-process database instance. Owns the catalog; Phase 3+ adds Connection / Query on top.
class Database {
  public:
    Catalog& catalog() noexcept { return catalog_; }
    const Catalog& catalog() const noexcept { return catalog_; }

  private:
    Catalog catalog_;
};

} // namespace cdb
