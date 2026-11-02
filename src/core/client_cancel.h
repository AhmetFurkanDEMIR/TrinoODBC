#pragma once
// Cancel hook for the catalog functions (private to the client group: api_catalog.cpp installs it,
// catalog.cpp consumes it). The catalog entry points of catalog.h take only the Connection, so the
// statement's SQLCancel flag reaches the metadata queries they run through this thread-local scope
// instead of a signature change of that frozen header.
#include <atomic>

namespace trino_odbc {

/// While alive, every metadata query started on this thread polls `flag` (Statement::cancelRequested)
/// and aborts with CancelledError (HY008) once it is set. Scopes nest; the previous flag is restored.
class MetadataCancelScope {
 public:
  explicit MetadataCancelScope(const std::atomic<bool>* flag);
  ~MetadataCancelScope();
  MetadataCancelScope(const MetadataCancelScope&) = delete;
  MetadataCancelScope& operator=(const MetadataCancelScope&) = delete;
  /// Flag of the innermost scope on this thread (nullptr when none).
  static const std::atomic<bool>* current();

 private:
  const std::atomic<bool>* previous_;
};

}  // namespace trino_odbc
